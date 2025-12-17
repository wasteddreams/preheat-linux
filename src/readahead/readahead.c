/* readahead.c - Readahead implementation for Preheat
 *
 * Based on preload 0.6.4 readahead.c (VERBATIM implementation)
 * Based on the preload daemon
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.h"
#include "readahead.h"
#include "../utils/logging.h"
#include "../config/config.h"

#include <sys/ioctl.h>
#include <sys/wait.h>
#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif

/**
 * Set block number for a file
 * (VERBATIM from upstream set_block)
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
 * Compare files by path
 * (VERBATIM from upstream map_path_compare)
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
 * Compare files by block
 * (VERBATIM from upstream map_block_compare)
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

/**
 * Track number of child processes
 * (VERBATIM from upstream)
 */
static int procs = 0;

/**
 * Wait for child processes to terminate
 * (VERBATIM from upstream wait_for_children)
 */
static void
wait_for_children(void)
{
    /* Wait for child processes to terminate */
    while (procs > 0) {
        int status;
        if (wait(&status) > 0)
            procs--;
    }
}

/**
 * Process a single file with readahead
 * (VERBATIM from upstream process_file)
 */
static void
process_file(const char *path, size_t offset, size_t length)
{
    int fd = -1;
    int maxprocs = kp_conf->system.maxprocs;
    
    if (procs >= maxprocs)
        wait_for_children();
    
    if (maxprocs > 0) {
        /* Parallel reading */
        int status = fork();
        
        if (status == -1) {
            /* Ignore error, return */
            return;
        }
        
        /* Return immediately in the parent */
        if (status > 0) {
            procs++;
            return;
        }
    }
    
    fd = open(path,
              O_RDONLY
            | O_NOCTTY
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
 * Sort by block or inode
 * (VERBATIM from upstream sort_by_block_or_inode)
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
 * Sort files by configured strategy
 * (VERBATIM from upstream sort_files)
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
 * Perform readahead on array of maps
 * (VERBATIM from upstream preload_readahead)
 * 
 * @param files Array of kp_map_t pointers
 * @param file_count Number of files to readahead
 * @return Number of files successfully processed
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
            processed++;
            path = NULL;
        }
        
        path   = files[i]->path;
        offset = files[i]->offset;
        length = files[i]->length;
    }
    
    if (path) {
        process_file(path, offset, length);
        processed++;
        path = NULL;
    }
    
    wait_for_children();
    
    return processed;
}
