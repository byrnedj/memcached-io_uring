/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Huge page allocation support for memcached.
 */

#include "hugepage.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <linux/mman.h>

/* Ensure MAP_HUGE_* macros are defined */
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#elif defined(__FreeBSD__)
#include <sys/mman.h>
#endif

/* Verbose logging level (set by memcached) */
extern int verbose_hugepage;
int verbose_hugepage = 0;

size_t hugepage_align_size(size_t size, size_t page_size) {
    if (page_size == 0) return size;
    return (size + page_size - 1) & ~(page_size - 1);
}

const char *hugepage_type_name(hugepage_alloc_type_t type) {
    switch (type) {
        case HUGEPAGE_ALLOC_1GB:    return "1GB hugepages";
        case HUGEPAGE_ALLOC_2MB:    return "2MB hugepages";
        case HUGEPAGE_ALLOC_THP:    return "transparent hugepages";
        case HUGEPAGE_ALLOC_MALLOC: return "malloc";
        default:                    return "failed";
    }
}

#if defined(__linux__)

/* Try to allocate with explicit huge pages */
static void *try_hugepage_mmap(size_t size, int huge_flag, bool prefault) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | huge_flag;
    if (prefault) {
#ifdef MAP_POPULATE
        flags |= MAP_POPULATE;
#endif
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }
    return ptr;
}

/* Try to allocate with transparent huge pages (THP) */
static void *try_thp_alloc(size_t size, bool prefault) {
    void *ptr = NULL;
    int ret;

    /* Align to 2MB boundary for THP */
    ret = posix_memalign(&ptr, HUGEPAGE_SIZE_2MB, size);
    if (ret != 0 || ptr == NULL) {
        return NULL;
    }

#ifdef MADV_HUGEPAGE
    /* Request THP promotion */
    ret = madvise(ptr, size, MADV_HUGEPAGE);
    if (ret < 0) {
        if (verbose_hugepage > 1) {
            fprintf(stderr, "hugepage: madvise MADV_HUGEPAGE failed: %s\n",
                    strerror(errno));
        }
        /* Continue anyway - THP might still work */
    }
#endif

    if (prefault) {
#ifdef MADV_POPULATE_WRITE
        /* Pre-fault pages with write semantics */
        ret = madvise(ptr, size, MADV_POPULATE_WRITE);
        if (ret < 0) {
            if (verbose_hugepage > 1) {
                fprintf(stderr, "hugepage: madvise MADV_POPULATE_WRITE failed: %s\n",
                        strerror(errno));
            }
            /* Fall back to manual prefaulting */
            volatile char *p = (volatile char *)ptr;
            long page_size = sysconf(_SC_PAGESIZE);
            if (page_size > 0) {
                for (size_t off = 0; off < size; off += page_size) {
                    char v = p[off];
                    p[off] = v;  /* Write-fault the page */
                }
            }
        }
#else
        /* Manual prefaulting */
        volatile char *p = (volatile char *)ptr;
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0) {
            for (size_t off = 0; off < size; off += page_size) {
                char v = p[off];
                p[off] = v;
            }
        }
#endif
    }

    return ptr;
}

void *hugepage_alloc(size_t size, unsigned int flags, hugepage_alloc_t *result) {
    void *ptr = NULL;
    hugepage_alloc_type_t alloc_type = HUGEPAGE_ALLOC_FAIL;
    size_t alloc_size = size;
    bool is_mmap = false;
    bool prefault = (flags & HUGEPAGE_FLAG_PREFAULT) != 0;

    /* Try 1GB huge pages for very large allocations */
    if (size >= HUGEPAGE_SIZE_1GB) {
        alloc_size = hugepage_align_size(size, HUGEPAGE_SIZE_1GB);
        ptr = try_hugepage_mmap(alloc_size, MAP_HUGE_1GB, prefault);
        if (ptr != NULL) {
            alloc_type = HUGEPAGE_ALLOC_1GB;
            is_mmap = true;
            if (verbose_hugepage > 0) {
                fprintf(stderr, "hugepage: allocated %zu bytes with 1GB pages\n", alloc_size);
            }
            goto done;
        }
        if (verbose_hugepage > 1) {
            fprintf(stderr, "hugepage: 1GB page allocation failed: %s\n", strerror(errno));
        }
    }

    /* Try 2MB huge pages */
    if (size >= HUGEPAGE_SIZE_2MB) {
        alloc_size = hugepage_align_size(size, HUGEPAGE_SIZE_2MB);
        ptr = try_hugepage_mmap(alloc_size, MAP_HUGE_2MB, prefault);
        if (ptr != NULL) {
            alloc_type = HUGEPAGE_ALLOC_2MB;
            is_mmap = true;
            if (verbose_hugepage > 0) {
                fprintf(stderr, "hugepage: allocated %zu bytes with 2MB pages\n", alloc_size);
            }
            goto done;
        }
        if (verbose_hugepage > 1) {
            fprintf(stderr, "hugepage: 2MB page allocation failed: %s\n", strerror(errno));
        }
    }

    /* Try transparent huge pages */
    if (flags & HUGEPAGE_FLAG_THP_FALLBACK) {
        if (size >= HUGEPAGE_SIZE_2MB) {
            alloc_size = hugepage_align_size(size, HUGEPAGE_SIZE_2MB);
        } else {
            alloc_size = size;
        }
        ptr = try_thp_alloc(alloc_size, prefault);
        if (ptr != NULL) {
            alloc_type = HUGEPAGE_ALLOC_THP;
            is_mmap = false;  /* THP uses posix_memalign, freed with free() */
            if (verbose_hugepage > 0) {
                fprintf(stderr, "hugepage: allocated %zu bytes with THP\n", alloc_size);
            }
            goto done;
        }
        if (verbose_hugepage > 1) {
            fprintf(stderr, "hugepage: THP allocation failed\n");
        }
    }

    /* Fall back to regular malloc */
    if (flags & HUGEPAGE_FLAG_MALLOC_FALLBACK) {
        alloc_size = size;
        ptr = malloc(alloc_size);
        if (ptr != NULL) {
            alloc_type = HUGEPAGE_ALLOC_MALLOC;
            is_mmap = false;
            if (verbose_hugepage > 0) {
                fprintf(stderr, "hugepage: allocated %zu bytes with malloc\n", alloc_size);
            }
            goto done;
        }
    }

done:
    /* Apply mlock if requested and allocation succeeded */
    if (ptr != NULL && (flags & HUGEPAGE_FLAG_MLOCK)) {
        if (mlock(ptr, alloc_size) != 0) {
            if (verbose_hugepage > 1) {
                fprintf(stderr, "hugepage: mlock failed: %s\n", strerror(errno));
            }
            /* Continue anyway */
        }
    }

    /* Fill in result if provided */
    if (result != NULL) {
        result->ptr = ptr;
        result->size = (ptr != NULL) ? alloc_size : 0;
        result->requested_size = size;
        result->type = alloc_type;
        result->is_mmap = is_mmap;
    }

    return ptr;
}

void hugepage_free(hugepage_alloc_t *alloc) {
    if (alloc == NULL || alloc->ptr == NULL) {
        return;
    }
    hugepage_free_ptr(alloc->ptr, alloc->size, alloc->is_mmap);
    alloc->ptr = NULL;
    alloc->size = 0;
}

void hugepage_free_ptr(void *ptr, size_t size, bool is_mmap) {
    if (ptr == NULL) {
        return;
    }

    /* Unlock if locked */
    munlock(ptr, size);

    if (is_mmap) {
        munmap(ptr, size);
    } else {
        free(ptr);
    }
}

void hugepage_check_available(bool *size_1gb, bool *size_2mb) {
    /* Try a small test allocation to check availability */
    void *ptr;

    if (size_2mb != NULL) {
        *size_2mb = false;
        ptr = mmap(NULL, HUGEPAGE_SIZE_2MB, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
        if (ptr != MAP_FAILED) {
            munmap(ptr, HUGEPAGE_SIZE_2MB);
            *size_2mb = true;
        }
    }

    if (size_1gb != NULL) {
        *size_1gb = false;
        ptr = mmap(NULL, HUGEPAGE_SIZE_1GB, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0);
        if (ptr != MAP_FAILED) {
            munmap(ptr, HUGEPAGE_SIZE_1GB);
            *size_1gb = true;
        }
    }
}

#elif defined(__FreeBSD__)

void *hugepage_alloc(size_t size, unsigned int flags, hugepage_alloc_t *result) {
    void *ptr = NULL;
    hugepage_alloc_type_t alloc_type = HUGEPAGE_ALLOC_FAIL;
    size_t alloc_size = size;
    bool is_mmap = false;

    /* FreeBSD uses super pages with MAP_ALIGNED_SUPER */
    if (size >= HUGEPAGE_SIZE_2MB) {
        size_t align = (sizeof(size_t) * 8 - (__builtin_clzl(4095)));
        int mmap_flags = MAP_SHARED | MAP_ANON | MAP_ALIGNED(align) | MAP_ALIGNED_SUPER;

        alloc_size = hugepage_align_size(size, HUGEPAGE_SIZE_2MB);
        ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
        if (ptr != MAP_FAILED) {
            alloc_type = HUGEPAGE_ALLOC_2MB;
            is_mmap = true;
            if (verbose_hugepage > 0) {
                fprintf(stderr, "hugepage: allocated %zu bytes with super pages\n", alloc_size);
            }
            goto done;
        }
        ptr = NULL;
    }

    /* Fall back to malloc */
    if (flags & HUGEPAGE_FLAG_MALLOC_FALLBACK) {
        alloc_size = size;
        ptr = malloc(alloc_size);
        if (ptr != NULL) {
            alloc_type = HUGEPAGE_ALLOC_MALLOC;
            is_mmap = false;
        }
    }

done:
    if (result != NULL) {
        result->ptr = ptr;
        result->size = (ptr != NULL) ? alloc_size : 0;
        result->requested_size = size;
        result->type = alloc_type;
        result->is_mmap = is_mmap;
    }

    return ptr;
}

void hugepage_free(hugepage_alloc_t *alloc) {
    if (alloc == NULL || alloc->ptr == NULL) {
        return;
    }
    hugepage_free_ptr(alloc->ptr, alloc->size, alloc->is_mmap);
    alloc->ptr = NULL;
    alloc->size = 0;
}

void hugepage_free_ptr(void *ptr, size_t size, bool is_mmap) {
    if (ptr == NULL) {
        return;
    }
    if (is_mmap) {
        munmap(ptr, size);
    } else {
        free(ptr);
    }
}

void hugepage_check_available(bool *size_1gb, bool *size_2mb) {
    if (size_1gb != NULL) *size_1gb = false;
    if (size_2mb != NULL) *size_2mb = true;  /* FreeBSD super pages are available */
}

#else
/* Fallback for other platforms */

void *hugepage_alloc(size_t size, unsigned int flags, hugepage_alloc_t *result) {
    void *ptr = NULL;
    hugepage_alloc_type_t alloc_type = HUGEPAGE_ALLOC_FAIL;

    if (flags & HUGEPAGE_FLAG_MALLOC_FALLBACK) {
        ptr = malloc(size);
        if (ptr != NULL) {
            alloc_type = HUGEPAGE_ALLOC_MALLOC;
        }
    }

    if (result != NULL) {
        result->ptr = ptr;
        result->size = (ptr != NULL) ? size : 0;
        result->requested_size = size;
        result->type = alloc_type;
        result->is_mmap = false;
    }

    return ptr;
}

void hugepage_free(hugepage_alloc_t *alloc) {
    if (alloc == NULL || alloc->ptr == NULL) {
        return;
    }
    free(alloc->ptr);
    alloc->ptr = NULL;
    alloc->size = 0;
}

void hugepage_free_ptr(void *ptr, size_t size, bool is_mmap) {
    (void)size;
    (void)is_mmap;
    free(ptr);
}

void hugepage_check_available(bool *size_1gb, bool *size_2mb) {
    if (size_1gb != NULL) *size_1gb = false;
    if (size_2mb != NULL) *size_2mb = false;
}

#endif /* platform checks */
