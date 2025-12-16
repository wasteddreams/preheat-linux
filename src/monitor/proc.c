/* proc.c - Process monitoring for Preheat
 *
 * Based on preload 0.6.4 proc.c (VERBATIM implementation)
 * Based on the preload daemon
 * Copyright (C) 2004  Soeren Sandmann
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.h"
#include "../utils/logging.h"
#include "proc.h"
#include "../config/config.h"
#include "../state/state.h"

#include <dirent.h>
#include <ctype.h>

/* NOTE ON PRELINK HANDLING (from upstream):
 * Ideally we want to ignore/get-rid-of deleted binaries and maps, BUT,
 * preLINK renames and later deletes them all the time, to replace them
 * with (supposedly better) prelinked ones. If we go by ignoring deleted
 * files, we end up losing all precious information we have gathered.
 * What we do is to consider the renamed file as the original file. In
 * other words, when prelink renames /bin/bash to /bin/bash.#prelink#.12345,
 * we will behave just like it was still /bin/bash. That almost solves the
 * problem. Fortunately prelink does not change map offset/length, only
 * fills in the linking addresses.
 */

/**
 * Sanitize file path (handle prelink, deleted files)
 * (VERBATIM from upstream sanitize_file)
 */
static gboolean
sanitize_file(char *file)
{
    char *p;
    
    if (*file != '/') /* not a file-backed object */
        return FALSE;
    
    /* Get rid of prelink foo and accept it */
    p = strstr(file, ".#prelink#.");
    if (p) {
        *p = '\0';
        return TRUE;
    }
    
    /* And (non-prelinked) deleted files */
    if (strstr(file, "(deleted)"))
        return FALSE;
    
    return TRUE;
}

/**
 * Check if file should be accepted based on prefix rules
 * (VERBATIM from upstream accept_file)
 */
static gboolean
accept_file(char *file, char * const *prefix)
{
    if (prefix)
        for (; *prefix; prefix++) {
            const char *p = *prefix;
            gboolean accept;
            if (*p == '!') {
                p++;
                accept = FALSE;
            } else {
                accept = TRUE;
            }
            if (!strncmp(file, p, strlen(p)))
                return accept;
        }
    
    /* Accept if no match */
    return TRUE;
}

/**
 * Get memory maps for a process
 * (VERBATIM from upstream proc_get_maps)
 * 
 * @return Sum of length of maps in bytes, or 0 if failed
 */
size_t
kp_proc_get_maps(pid_t pid, GHashTable *maps, GSet **exemaps)
{
    char name[32];
    FILE *in;
    size_t size = 0;
    char buffer[1024];
    
    if (exemaps)
        *exemaps = g_set_new();
    
    g_snprintf(name, sizeof(name) - 1, "/proc/%d/maps", pid);
    in = fopen(name, "r");
    if (!in) {
        /* This may fail for a variety of reason. Process terminated
         * for example, or permission denied. */
        return 0;
    }
    
    while (fgets(buffer, sizeof(buffer) - 1, in)) {
        char file[FILELEN];
        long start, end, offset, length;
        int count;
        
        count = sscanf(buffer, "%lx-%lx %*15s %lx %*x:%*x %*u %"FILELENSTR"s",
                       &start, &end, &offset, file);
        
        if (count != 4 || !sanitize_file(file) || !accept_file(file, kp_conf->system.mapprefix))
            continue;
        
        length = end - start;
        size += length;
        
        if (maps || exemaps) {
            gpointer orig_map;
            kp_map_t *map;
            gpointer value;
            
            map = kp_map_new(file, offset, length);
            
            if (maps) {
                if (g_hash_table_lookup_extended(maps, map, &orig_map, &value)) {
                    kp_map_free(map);
                    map = (kp_map_t *)orig_map;
                }
            }
            
            if (exemaps) {
                kp_exemap_t *exemap;
                exemap = kp_exemap_new(map);
                g_set_add(*exemaps, exemap);
            }
        }
    }
    
    fclose(in);
    
    return size;
}

/**
 * Check if string contains only digits
 * (VERBATIM from upstream all_digits)
 */
static gboolean
all_digits(const char *s)
{
    for (; *s; ++s) {
        if (!isdigit(*s))
            return FALSE;
    }
    return TRUE;
}

/**
 * Iterate over all running processes
 * Modified from upstream to handle /proc failures gracefully
 */
void
kp_proc_foreach(GHFunc func, gpointer user_data)
{
    DIR *proc;
    struct dirent *entry;
    pid_t selfpid = getpid();
    static int proc_fail_logged = 0;
    
    proc = opendir("/proc");
    if (!proc) {
        /* Graceful degradation: log once and skip this cycle */
        if (!proc_fail_logged) {
            g_warning("failed opening /proc: %s - will retry next cycle", strerror(errno));
            proc_fail_logged = 1;
        }
        return;  /* Skip this scan cycle, don't crash */
    }
    
    /* Reset failure counter on success */
    proc_fail_logged = 0;
    
    while ((entry = readdir(proc))) {
        if (entry->d_name && all_digits(entry->d_name)) {
            pid_t pid;
            char name[32];
            char exe_buffer[FILELEN];
            int len;
            
            pid = atoi(entry->d_name);
            if (pid == selfpid)
                continue;
            
            g_snprintf(name, sizeof(name) - 1, "/proc/%s/exe", entry->d_name);
            
            len = readlink(name, exe_buffer, sizeof(exe_buffer));
            
            if (len <= 0 /* error occured */
                || len == sizeof(exe_buffer) /* name didn't fit completely */)
                continue;
            
            exe_buffer[len] = '\0';
            
            if (!sanitize_file(exe_buffer) || !accept_file(exe_buffer, kp_conf->system.exeprefix))
                continue;
            
            func(GUINT_TO_POINTER(pid), exe_buffer, user_data);
        }
    }
    
    closedir(proc);
}

/* Macros for reading /proc files (VERBATIM from upstream) */
#define open_file(filename) G_STMT_START {              \
    int fd, len;                                        \
    len = 0;                                            \
    if ((fd = open(filename, O_RDONLY)) != -1) {        \
        if ((len = read(fd, buf, sizeof (buf) - 1)) < 0)\
            len = 0;                                    \
        close (fd);                                     \
    }                                                   \
    buf[len] = '\0';                                    \
} G_STMT_END

#define read_tag(tag, v) G_STMT_START {                 \
    const char *b;                                      \
    b = strstr(buf, tag" ");                            \
    if (b) sscanf(b, tag" %d", &(v));                   \
} G_STMT_END

#define read_tag2(tag, v1, v2) G_STMT_START {           \
    const char *b;                                      \
    b = strstr(buf, tag" ");                            \
    if (b) sscanf(b, tag" %d %d", &(v1), &(v2));        \
} G_STMT_END

/**
 * Read system memory information
 * (VERBATIM from upstream proc_get_memstat)
 */
void
kp_proc_get_memstat(kp_memory_t *mem)
{
    static int pagesize = 0;
    char buf[4096];
    
    memset(mem, 0, sizeof(*mem));
    
    if (!pagesize)
        pagesize = getpagesize();
    
    open_file("/proc/meminfo");
    read_tag("MemTotal:", mem->total);
    read_tag("MemFree:", mem->free);
    read_tag("Buffers:", mem->buffers);
    read_tag("Cached:", mem->cached);
    
    open_file("/proc/vmstat");
    read_tag("pgpgin", mem->pagein);
    read_tag("pgpgout", mem->pageout);
    
    if (!mem->pagein) {
        open_file("/proc/stat");
        read_tag2("page", mem->pagein, mem->pageout);
    }
    
    mem->pagein *= pagesize / 1024;
    mem->pageout *= pagesize / 1024;
    
    if (!mem->total || !mem->pagein)
        g_warning("failed to read memory stat, is /proc mounted?");
}
