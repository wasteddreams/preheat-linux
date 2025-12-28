/* readahead.c - Readahead implementation for Preheat
 *
 * Based on preload 0.6.4 readahead.c (VERBATIM implementation)
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: Readahead Execution
 * =============================================================================
 *
 * This module performs the actual disk I/O to preload files into memory.
 * It uses the Linux readahead() system call, which:
 *   - Initiates asynchronous read of file data into page cache
 *   - Returns immediately (non-blocking)
 *   - Makes subsequent file access faster (no disk wait)
 *
 * I/O OPTIMIZATION STRATEGIES:
 *
 *   1. SORTING: Files are sorted to minimize disk seek time:
 *      - SORT_PATH:  Alphabetically by path (good for SSDs)
 *      - SORT_INODE: By inode number (good for HDDs)
 *      - SORT_BLOCK: By physical block number (best for HDDs)
 *
 *   2. MERGING: Adjacent file regions are merged into single requests
 *      to reduce system call overhead.
 *
 *   3. PARALLELISM: Fork child processes (configurable) to overlap
 *      I/O operations across multiple files.
 *
 * FLOW:
 *   kp_readahead(files, count)
 *     └─ sort_files()       → Optimize read order
 *        └─ for each file:
 *           └─ merge adjacent regions
 *           └─ process_file() → readahead() syscall (possibly forked)
 *        └─ wait_for_children()
 *
 * =============================================================================
 */

#include "common.h"
#include "readahead.h"
#include "../utils/logging.h"
#include "../config/config.h"
#include "../daemon/stats.h"

#include <sys/ioctl.h>
#include <sys/wait.h>
#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif

/**
 * Retrieve physical block number or inode for a file
 *
 * Used to sort files by their physical disk location, which minimizes
 * seek time on rotational hard drives. SSDs don't benefit from this,
 * but it doesn't hurt either.
 *
 * @param file       Map structure to update with block info
 * @param use_inode  If TRUE, use inode number instead of physical block
 *
 * ALGORITHM:
 *   1. Open the file read-only
 *   2. If use_inode=FALSE and FIBMAP is available:
 *      - Calculate block number from offset and block size
 *      - Use ioctl(FIBMAP) to get physical block
 *   3. Fall back to inode number (from fstat)
 *   4. Store result in file->block
 *
 * SIDE EFFECTS:
 *   - Updates file->block field
 *   - Sets file->block to 0 on any error (to prevent retries)
 */
static void
set_block(kp_map_t *file, gboolean use_inode)
{
    int fd = -1;
    int block = 0;
    struct stat buf;

    /* In case we can get block, set to 0 to not retry */
    file->block = 0;

    fd = open(file->path, O_RDONLY);
    if (fd < 0)
        return;

    if (0 > fstat(fd, &buf)) {
        close(fd);
        return;
    }

#ifdef FIBMAP
    if (!use_inode) {
        block = file->offset / buf.st_blksize;
        if (0 > ioctl(fd, FIBMAP, &block))
            block = 0;
    }
#endif

    /* Fall back to inode number */
    block = buf.st_ino;

    file->block = block;

    close(fd);
}

/**
 * Compare two maps by file path (for qsort)
 *
 * Used when sorting files alphabetically, which groups related files
 * (from same directory) together. Good for SSDs and for making
 * block lookups faster on HDDs.
 *
 * @param pa  Pointer to pointer to first map
 * @param pb  Pointer to pointer to second map
 * @return    <0 if a < b, 0 if equal, >0 if a > b
 *
 * TIE-BREAKING ORDER:
 *   1. Compare paths alphabetically
 *   2. If same path: earlier offset first
 *   3. If same offset: larger length first (optimize for coverage)
 */
static int
map_path_compare(const kp_map_t **pa, const kp_map_t **pb)
{
    const kp_map_t *a = *pa, *b = *pb;
    int i;

    i = strcmp(a->path, b->path);
    if (!i) { /* same file - compare offsets safely */
        if (a->offset < b->offset) i = -1;
        else if (a->offset > b->offset) i = 1;
    }
    if (!i) { /* same offset?! - compare lengths safely */
        if (a->length < b->length) i = 1;  /* larger first */
        else if (a->length > b->length) i = -1;
    }

    return i;
}

/**
 * Compare two maps by physical block number (for qsort)
 *
 * Used when sorting files by disk location, which minimizes head
 * movement on rotational HDDs. Reading files in block order can be
 * 10x faster than random order on spinning disks.
 *
 * @param pa  Pointer to pointer to first map
 * @param pb  Pointer to pointer to second map
 * @return    <0 if a < b, 0 if equal, >0 if a > b
 *
 * TIE-BREAKING ORDER:
 *   1. Compare block numbers
 *   2. If same block (or both 0): compare paths
 *   3. If same path: earlier offset first
 *   4. If same offset: larger length first
 */
static int
map_block_compare(const kp_map_t **pa, const kp_map_t **pb)
{
    const kp_map_t *a = *pa, *b = *pb;
    int i;

    /* Compare blocks safely */
    if (a->block < b->block) i = -1;
    else if (a->block > b->block) i = 1;
    else i = 0;

    if (!i) /* no block? */
        i = strcmp(a->path, b->path);
    if (!i) { /* same file - compare offsets safely */
        if (a->offset < b->offset) i = -1;
        else if (a->offset > b->offset) i = 1;
    }
    if (!i) { /* same offset?! - compare lengths safely */
        if (a->length < b->length) i = 1;  /* larger first */
        else if (a->length > b->length) i = -1;
    }

    return i;
}

/*
 * Process tracking for parallel readahead.
 * Counts active child processes to enforce maxprocs limit.
 */
static int procs = 0;

/**
 * Wait for all child processes to complete
 *
 * Called when we've reached maxprocs limit or when finishing
 * the readahead batch. Blocks until all children exit.
 *
 * B006 FIX: Handle EINTR properly - retry wait() if interrupted.
 */
static void
wait_for_children(void)
{
    while (procs > 0) {
        int status;
        pid_t pid = wait(&status);
        if (pid > 0) {
            procs--;
        } else if (pid < 0 && errno != EINTR) {
            /* Real error - reset to avoid infinite loop */
            procs = 0;
            break;
        }
        /* EINTR: just retry */
    }
}

/**
 * Issue readahead system call for a single file region
 *
 * This is where the actual I/O happens. The readahead() syscall tells
 * the kernel to start reading the specified file region into the page
 * cache in the background.
 *
 * @param path    Absolute path to the file
 * @param offset  Start offset within the file (bytes)
 * @param length  Number of bytes to readahead
 *
 * PARALLELISM:
 *   If maxprocs > 0, this function forks a child process to do the
 *   readahead. This allows overlapping multiple disk reads. The parent
 *   returns immediately while the child does the I/O and exits.
 *
 * FILE FLAGS:
 *   O_RDONLY  - Read-only access
 *   O_NOCTTY  - Don't make this our controlling terminal
 *   O_NOATIME - Don't update access time (if available)
 */
static void
process_file(const char *path, size_t offset, size_t length)
{
    int fd = -1;
    int maxprocs = kp_conf->system.maxprocs;

    if (procs >= maxprocs)
        wait_for_children();

    if (maxprocs > 0) {
        /* B005 FIX: Increment procs BEFORE fork to prevent race.
         * If SIGTERM arrives between fork and procs++, child could be orphaned.
         * By incrementing first, wait_for_children() will always wait for it. */
        procs++;
        int status = fork();

        if (status == -1) {
            /* Fork failed - decrement counter and return */
            procs--;
            return;
        }

        /* Return immediately in the parent */
        if (status > 0) {
            return;  /* procs already incremented */
        }
    }

    /*
     * SECURITY: O_NOFOLLOW prevents following symlinks.
     * Files are already validated by trusted path checks in config.c,
     * but this provides defense-in-depth.
     */
    fd = open(path,
              O_RDONLY
            | O_NOCTTY
            | O_NOFOLLOW
#ifdef O_NOATIME
            | O_NOATIME
#endif
           );
    if (fd >= 0) {
        readahead(fd, offset, length);
        close(fd);
    }

    if (maxprocs > 0) {
        /* We're in a child process, exit */
        exit(0);
    }
}

/**
 * Sort files by physical block or inode number
 *
 * Two-pass algorithm:
 *   1. If any file is missing block info, sort by path first (makes
 *      subsequent stat() calls faster due to directory caching), then
 *      call set_block() to retrieve block/inode info.
 *   2. Sort by block number for optimal disk read order.
 *
 * @param files       Array of map pointers to sort in-place
 * @param file_count  Number of elements in the array
 *
 * PERFORMANCE:
 *   The initial path sort makes the block lookup O(n) instead of O(n²)
 *   because files in the same directory are grouped together.
 */
static void
sort_by_block_or_inode(kp_map_t **files, int file_count)
{
    int i;
    gboolean need_block = FALSE;

    /* First see if any file doesn't have block/inode info */
    for (i=0; i<file_count; i++)
        if (files[i]->block == -1) {
            need_block = TRUE;
            break;
        }

    if (need_block) {
        /* Sorting by path, to make stat fast. */
        qsort(files, file_count, sizeof(*files), (GCompareFunc)map_path_compare);

        for (i=0; i<file_count; i++)
            if (files[i]->block == -1)
                set_block(files[i], kp_conf->system.sortstrategy == SORT_INODE);
    }

    /* Sorting by block. */
    qsort(files, file_count, sizeof(*files), (GCompareFunc)map_block_compare);
}

/**
 * Sort files according to configured strategy
 *
 * Dispatcher function that selects the appropriate sorting algorithm
 * based on the sortstrategy configuration option.
 *
 * @param files       Array of map pointers to sort in-place
 * @param file_count  Number of elements in the array
 *
 * STRATEGIES:
 *   SORT_NONE  - No sorting (process in prediction priority order)
 *   SORT_PATH  - Alphabetical by path (groups related files)
 *   SORT_INODE - By inode number (fast on all filesystems)
 *   SORT_BLOCK - By physical block (optimal for HDDs, requires FIBMAP)
 */
static void
sort_files(kp_map_t **files, int file_count)
{
    switch (kp_conf->system.sortstrategy) {
        case SORT_NONE:
            break;

        case SORT_PATH:
            qsort(files, file_count, sizeof(*files), (GCompareFunc)map_path_compare);
            break;

        case SORT_INODE:
        case SORT_BLOCK:
            sort_by_block_or_inode(files, file_count);
            break;

        default:
            g_warning("Invalid value for config key system.sortstrategy: %d",
                      kp_conf->system.sortstrategy);
            /* Avoid warning every time */
            kp_conf->system.sortstrategy = SORT_BLOCK;
            break;
    }
}

/**
 * Main readahead entry point - preload files into page cache
 *
 * This is the core function called by the prediction engine to actually
 * load predicted files into memory. It optimizes I/O by:
 *   1. Sorting files to minimize disk seeks
 *   2. Merging adjacent regions in the same file
 *   3. Optionally parallelizing with fork()
 *
 * @param files       Array of kp_map_t pointers (sorted by prediction priority)
 * @param file_count  Number of files to attempt to readahead
 * @return            Number of readahead requests issued (after merging)
 *
 * MERGING LOGIC:
 *   When consecutive array entries refer to the same file and their
 *   regions overlap or are adjacent, they're merged into a single
 *   readahead() call. This reduces syscall overhead.
 *
 * EXAMPLE:
 *   Input:  [libc.so:0-1000, libc.so:500-2000, libm.so:0-500]
 *   Merged: [libc.so:0-2000, libm.so:0-500]
 *   Result: 2 readahead calls instead of 3
 */
int
kp_readahead(kp_map_t **files, int file_count)
{
    int i;
    const char *path = NULL;
    size_t offset = 0, length = 0;
    int processed = 0;

    sort_files(files, file_count);

    for (i=0; i<file_count; i++) {
        if (path &&
            offset <= files[i]->offset &&
            offset + length >= files[i]->offset &&
            0 == strcmp(path, files[i]->path)) {
            /* Merge requests */
            length = files[i]->offset + files[i]->length - offset;
            continue;
        }

        if (path) {
            process_file(path, offset, length);
            kp_stats_record_preload(path);
            processed++;
            path = NULL;
        }

        path   = files[i]->path;
        offset = files[i]->offset;
        length = files[i]->length;
    }

    if (path) {
        process_file(path, offset, length);
        kp_stats_record_preload(path);
        processed++;
        path = NULL;
    }

    wait_for_children();

    return processed;
}
