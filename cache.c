/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#ifndef NDEBUG
#include <signal.h>
#endif

#include "cache.h"
#include "memcached.h"

#ifndef NDEBUG
const uint64_t redzone_pattern = 0xdeadbeefcafedeed;
int cache_error = 0;
#endif

#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

/* Touch each page and lock it.
   IMPORTANT: we write back the same byte to force a *write* fault,
   which allocates a private anonymous page (avoids zero-page surprises). */
static int prefault_and_lock(void *addr, size_t len)
{
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) return -1;

    uintptr_t start = (uintptr_t)addr & ~( (uintptr_t)pg - 1 );
    uintptr_t end   = ((uintptr_t)addr + len + pg - 1) & ~( (uintptr_t)pg - 1 );
    size_t plen = end - start;
    volatile unsigned char *p = (volatile unsigned char *)start;

    /* Lock first so the pages we fault in become resident and stay resident */
    if (mlock((void *)start, plen) != 0) {
        /* EPERM/ENOMEM → raise RLIMIT_MEMLOCK or use mlockall() (see below) */
        return -1;
    }

    /* Prefault/write-touch one byte per page */
    for (size_t off = 0; off < plen; off += (size_t)pg) {
        unsigned char v = p[off];
        p[off] = v;               /* write back same value */
    }
    /* Optional: also fault the trailing partial page if any */
    if (plen && ((uintptr_t)addr + len) % pg) {
        size_t last = plen - (size_t)pg;
        unsigned char v = p[last];
        p[last] = v;
    }

    /* Optional hints */
    madvise((void *)start, plen, MADV_WILLNEED);   /* harmless for anon memory */
    madvise((void *)start, plen, MADV_DONTDUMP);   /* keep cores smaller */

    return 0;
}

/* Undo at free time */
static void unlock_region(void *addr, size_t len)
{
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) return;
    uintptr_t start = (uintptr_t)addr & ~( (uintptr_t)pg - 1 );
    uintptr_t end   = ((uintptr_t)addr + len + pg - 1) & ~( (uintptr_t)pg - 1 );
    munlock((void *)start, end - start);
}


cache_t* cache_create(const char *name, size_t bufsize, size_t align) {
    cache_t* ret = calloc(1, sizeof(cache_t));
    char* nm = strdup(name);
    if (ret == NULL || nm == NULL ||
        pthread_mutex_init(&ret->mutex, NULL) == -1) {
        free(ret);
        free(nm);
        return NULL;
    }

    ret->name = nm;
    STAILQ_INIT(&ret->head);

#ifndef NDEBUG
    ret->bufsize = bufsize + 2 * sizeof(redzone_pattern);
#else
    ret->bufsize = bufsize;
#endif
    assert(ret->bufsize >= sizeof(struct cache_free_s));

    /* Initialize pool to NULL (will be allocated on demand or via cache_set_limit) */
    ret->pool_base = NULL;
    ret->pool_size = 0;
    ret->pool_current = NULL;
    ret->pool_is_mmap = false;

    return ret;
}

void cache_set_limit(cache_t *cache, int limit) {
    pthread_mutex_lock(&cache->mutex);
    cache->limit = limit;

#ifdef MEMCACHED_MAIN
    /* Pre-allocate hugepage pool if enabled and pool not already allocated */
    if (settings.use_hugepages && cache->pool_base == NULL && limit > 0) {
        size_t pool_size = (size_t)limit * cache->bufsize;
        /* Ensure at least 2MB for hugepage efficiency */
        if (pool_size < HUGEPAGE_SIZE_2MB) {
            pool_size = HUGEPAGE_SIZE_2MB;
        }

        unsigned int flags = HUGEPAGE_FLAG_PREFAULT |
                             HUGEPAGE_FLAG_MLOCK |
                             HUGEPAGE_FLAG_THP_FALLBACK |
                             HUGEPAGE_FLAG_MALLOC_FALLBACK;
        hugepage_alloc_t result;
        void *pool = hugepage_alloc(pool_size, flags, &result);

        if (pool != NULL) {
            cache->pool_base = pool;
            cache->pool_size = result.size;
            cache->pool_current = (char *)pool;
            cache->pool_is_mmap = result.is_mmap;

            if (settings.verbose > 0) {
                fprintf(stderr, "Cache '%s': pre-allocated %zu bytes using %s\n",
                        cache->name, result.size,
                        hugepage_type_name(result.type));
            }
        }
    }
#endif

    pthread_mutex_unlock(&cache->mutex);
}

static inline void* get_object(void *ptr) {
#ifndef NDEBUG
    uint64_t *pre = ptr;
    return pre + 1;
#else
    return ptr;
#endif
}

/* Check if a pointer is within the pre-allocated pool (forward declaration for cache_destroy) */
static inline bool is_from_pool(cache_t *cache, void *ptr);

void cache_destroy(cache_t *cache) {
    /* Free individually allocated objects from freelist (skip pool objects) */
    while (!STAILQ_EMPTY(&cache->head)) {
        struct cache_free_s *o = STAILQ_FIRST(&cache->head);
        STAILQ_REMOVE_HEAD(&cache->head, c_next);
        if (!is_from_pool(cache, o)) {
            /* Only free individually allocated objects */
            cache_alloc_header_t *header = (cache_alloc_header_t *)((char *)o - sizeof(cache_alloc_header_t));
            if (header->is_mmap) {
                munmap(header, header->size);
            } else {
                free(header);
            }
        }
        /* Pool objects are freed with the pool below */
    }

    /* Free the pre-allocated pool */
    if (cache->pool_base != NULL) {
        if (cache->pool_is_mmap) {
            munmap(cache->pool_base, cache->pool_size);
        } else {
            free(cache->pool_base);
        }
    }

    free(cache->name);
    pthread_mutex_destroy(&cache->mutex);
    free(cache);
}

void* cache_alloc(cache_t *cache) {
    void *ret;
    pthread_mutex_lock(&cache->mutex);
    ret = do_cache_alloc(cache);
    pthread_mutex_unlock(&cache->mutex);
    return ret;
}

/* Check if a pointer is within the pre-allocated pool */
static inline bool is_from_pool(cache_t *cache, void *ptr) {
    if (cache->pool_base == NULL) return false;
    char *p = (char *)ptr;
    char *pool_start = (char *)cache->pool_base;
    char *pool_end = pool_start + cache->pool_size;
    return (p >= pool_start && p < pool_end);
}

void* do_cache_alloc(cache_t *cache) {
    void *ret;
    void *object;
    if (cache->freecurr > 0) {
        /* Use object from freelist */
        ret = STAILQ_FIRST(&cache->head);
        STAILQ_REMOVE_HEAD(&cache->head, c_next);
        object = get_object(ret);
        cache->freecurr--;
    } else if (cache->limit == 0 || cache->total < cache->limit) {
        /* Need to allocate a new object */
        ret = NULL;

        /* First, try to carve from pre-allocated pool */
        if (cache->pool_base != NULL) {
            char *pool_end = (char *)cache->pool_base + cache->pool_size;
            if (cache->pool_current + cache->bufsize <= pool_end) {
                ret = cache->pool_current;
                cache->pool_current += cache->bufsize;
                object = get_object(ret);
                cache->total++;
            }
        }

        /* Fall back to individual allocation if pool unavailable or exhausted */
        if (ret == NULL) {
            size_t header_size = sizeof(cache_alloc_header_t);
            size_t alloc_size = header_size + cache->bufsize;
            void *raw_ptr = malloc(alloc_size);

            if (raw_ptr != NULL) {
                prefault_and_lock(raw_ptr, alloc_size);
                /* Store allocation metadata in header */
                cache_alloc_header_t *header = (cache_alloc_header_t *)raw_ptr;
                header->size = alloc_size;
                header->is_mmap = false;
                /* Return pointer past the header */
                ret = (char *)raw_ptr + header_size;
                object = get_object(ret);
                cache->total++;
            } else {
                object = NULL;
            }
        }
    } else {
        object = NULL;
    }

#ifndef NDEBUG
    if (object != NULL) {
        /* add a simple form of buffer-check */
        uint64_t *pre = ret;
        *pre = redzone_pattern;
        ret = pre+1;
        memcpy(((char*)ret) + cache->bufsize - (2 * sizeof(redzone_pattern)),
               &redzone_pattern, sizeof(redzone_pattern));
    }
#endif

    return object;
}

void cache_free(cache_t *cache, void *ptr) {
    pthread_mutex_lock(&cache->mutex);
    do_cache_free(cache, ptr);
    pthread_mutex_unlock(&cache->mutex);
}

void do_cache_free(cache_t *cache, void *ptr) {
#ifndef NDEBUG
    /* validate redzone... */
    if (memcmp(((char*)ptr) + cache->bufsize - (2 * sizeof(redzone_pattern)),
               &redzone_pattern, sizeof(redzone_pattern)) != 0) {
        raise(SIGABRT);
        cache_error = 1;
        return;
    }
    uint64_t *pre = ptr;
    --pre;
    if (*pre != redzone_pattern) {
        raise(SIGABRT);
        cache_error = -1;
        return;
    }
    ptr = pre;
#endif
    /* Check if object is from the pre-allocated pool */
    bool from_pool = is_from_pool(cache, ptr);

    if (!from_pool && cache->limit != 0 && cache->limit < cache->total) {
        /* Object is individually allocated and we're over limit - free to OS */
        cache_alloc_header_t *header = (cache_alloc_header_t *)((char *)ptr - sizeof(cache_alloc_header_t));
        unlock_region(ptr, cache->bufsize);
        if (header->is_mmap) {
            munmap(header, header->size);
        } else {
            free(header);
        }
        cache->total--;
    } else {
        /* Pool objects always go to freelist; individual objects go to freelist if under limit */
        STAILQ_INSERT_HEAD(&cache->head, (struct cache_free_s *)ptr, c_next);
        cache->freecurr++;
    }
}

