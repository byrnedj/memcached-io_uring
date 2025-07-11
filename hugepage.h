/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Huge page allocation support for memcached.
 * Provides unified interface for allocating memory using huge pages
 * with fallback to transparent huge pages (THP) and regular malloc.
 */

#ifndef HUGEPAGE_H
#define HUGEPAGE_H

#include <stddef.h>
#include <stdbool.h>

/* Huge page sizes */
#define HUGEPAGE_SIZE_2MB  (2ULL << 20)
#define HUGEPAGE_SIZE_1GB  (1ULL << 30)

/* Allocation flags */
#define HUGEPAGE_FLAG_PREFAULT     (1 << 0)  /* Pre-fault pages on allocation */
#define HUGEPAGE_FLAG_MLOCK        (1 << 1)  /* Lock pages in memory */
#define HUGEPAGE_FLAG_THP_FALLBACK (1 << 2)  /* Allow THP fallback */
#define HUGEPAGE_FLAG_MALLOC_FALLBACK (1 << 3) /* Allow malloc fallback */

/* Huge page allocation result */
typedef enum {
    HUGEPAGE_ALLOC_FAIL = 0,      /* Allocation failed */
    HUGEPAGE_ALLOC_1GB,           /* Allocated with 1GB huge pages */
    HUGEPAGE_ALLOC_2MB,           /* Allocated with 2MB huge pages */
    HUGEPAGE_ALLOC_THP,           /* Allocated with transparent huge pages */
    HUGEPAGE_ALLOC_MALLOC         /* Allocated with regular malloc */
} hugepage_alloc_type_t;

/* Allocation metadata */
typedef struct {
    void *ptr;                     /* Allocated memory pointer */
    size_t size;                   /* Actual allocated size (may be aligned) */
    size_t requested_size;         /* Originally requested size */
    hugepage_alloc_type_t type;    /* Type of allocation used */
    bool is_mmap;                  /* True if mmap was used (vs malloc) */
} hugepage_alloc_t;

/*
 * Allocate memory with huge page support.
 *
 * @param size       Requested allocation size in bytes
 * @param flags      Allocation flags (HUGEPAGE_FLAG_*)
 * @param result     Output: allocation metadata (can be NULL)
 *
 * @return Pointer to allocated memory, or NULL on failure
 *
 * The function tries allocations in this order:
 *   1. 1GB huge pages (if size >= 1GB)
 *   2. 2MB huge pages (if size >= 2MB)
 *   3. Transparent huge pages with madvise (if THP_FALLBACK set)
 *   4. Regular malloc (if MALLOC_FALLBACK set)
 */
void *hugepage_alloc(size_t size, unsigned int flags, hugepage_alloc_t *result);

/*
 * Free memory allocated with hugepage_alloc.
 *
 * @param alloc      Allocation metadata from hugepage_alloc
 */
void hugepage_free(hugepage_alloc_t *alloc);

/*
 * Free memory by pointer (when metadata not available).
 * Only works for mmap allocations; for malloc, use free() directly.
 *
 * @param ptr        Pointer to memory
 * @param size       Size of allocation
 * @param is_mmap    True if mmap was used
 */
void hugepage_free_ptr(void *ptr, size_t size, bool is_mmap);

/*
 * Get the name of the allocation type.
 *
 * @param type       Allocation type
 * @return           Human-readable name
 */
const char *hugepage_type_name(hugepage_alloc_type_t type);

/*
 * Check if huge pages are available on the system.
 *
 * @param size_1gb   Output: true if 1GB pages available
 * @param size_2mb   Output: true if 2MB pages available
 */
void hugepage_check_available(bool *size_1gb, bool *size_2mb);

/*
 * Align size up to the nearest huge page boundary.
 *
 * @param size       Size to align
 * @param page_size  Page size to align to
 * @return           Aligned size
 */
size_t hugepage_align_size(size_t size, size_t page_size);

#endif /* HUGEPAGE_H */
