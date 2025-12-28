/* proc.c - Process monitoring for Preheat
 *
 * Based on preload 0.6.4 proc.c (VERBATIM implementation)
 * Copyright (C) 2004  Soeren Sandmann
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: /proc Filesystem Interface
 * =============================================================================
 *
 * This module provides low-level access to the Linux /proc filesystem to:
 *
 *   1. DISCOVER RUNNING PROCESSES: Scan /proc for PID directories and
 *      extract executable paths via /proc/PID/exe symlinks.
 *
 *   2. MAP MEMORY REGIONS: Parse /proc/PID/maps to discover shared libraries
 *      and memory-mapped files used by each process.
 *
 *   3. READ MEMORY STATISTICS: Parse /proc/meminfo and /proc/vmstat to
 *      determine available memory for preloading decisions.
 *
 * KEY FILES ACCESSED:
 *   /proc/           - Directory listing reveals all running PIDs
 *   /proc/PID/exe    - Symlink to the process's executable binary
 *   /proc/PID/maps   - Memory map showing all loaded files and addresses
 *   /proc/meminfo    - System memory statistics (total, free, cached)
 *   /proc/vmstat     - Virtual memory statistics (page in/out counts)
 *
 * DATA FLOW:
 *   kp_proc_foreach() → discovers processes → calls callback with (pid, exe_path)
 *   kp_proc_get_maps() → parses /proc/PID/maps → returns memory map regions
 *   kp_proc_get_memstat() → parses /proc/meminfo → returns memory stats
 *
 * PRELINK HANDLING:
 *   The prelink tool creates temporary files like /bin/bash.#prelink#.12345
 *   This module strips the prelink suffix to maintain consistent tracking
 *   even when prelink is actively relinking binaries.
 *
 * RACE CONDITIONS:
 *   Processes can exit at any time between when we see them and when we
 *   read their /proc entries. All /proc operations handle ENOENT gracefully.
 *
 * =============================================================================
 */

#include "common.h"
#include "../utils/logging.h"
#include "proc.h"
#include "../config/config.h"
#include "../state/state.h"

#include <dirent.h>
#include <ctype.h>

/*
 * Prelink Handling Note (from original preload):
 *
 * Ideally we want to ignore/get-rid-of deleted binaries and maps, BUT,
 * preLINK renames and later deletes them all the time, to replace them
 * with (supposedly better) prelinked ones. If we go by ignoring deleted
 * files, we end up losing all precious information we have gathered.
 *
 * What we do is to consider the renamed file as the original file. In
 * other words, when prelink renames /bin/bash to /bin/bash.#prelink#.12345,
 * we will behave just like it was still /bin/bash. That almost solves the
 * problem. Fortunately prelink does not change map offset/length, only
 * fills in the linking addresses.
 */

/**
 * Sanitize file path for tracking consistency
 *
 * Handles two special cases:
 *   1. PRELINK: Strips ".#prelink#.NNNNN" suffixes so prelinked files
 *      are tracked under their original names.
 *   2. DELETED: Rejects files marked "(deleted)" since they no longer exist.
 *
 * @param file  Path to sanitize (modified in place if prelink suffix found)
 * @return      TRUE if file should be tracked, FALSE if it should be skipped
 *
 * EXAMPLES:
 *   "/usr/bin/bash"                    → TRUE (unchanged)
 *   "/usr/bin/bash.#prelink#.12345"   → TRUE (path truncated at suffix)
 *   "/usr/lib/x.so (deleted)"         → FALSE (deleted files skipped)
 *   "[vdso]"                           → FALSE (not a real file)
 */
static gboolean
sanitize_file(char *file)
{
    char *p;

    /* Non-file-backed mappings start with [ like [vdso], [heap], [stack] */
    if (*file != '/')
        return FALSE;

    /* Strip prelink temporary suffix and accept the file */
    p = strstr(file, ".#prelink#.");
    if (p) {
        *p = '\0';  /* Truncate at suffix */
        return TRUE;
    }

    /* Reject deleted files - they're gone and can't be preloaded */
    if (strstr(file, "(deleted)"))
        return FALSE;

    return TRUE;
}

/**
 * Check if file matches prefix include/exclude rules
 *
 * Configuration can specify path prefixes to include or exclude:
 *   - "/usr/lib"      → Include files starting with /usr/lib
 *   - "!/usr/share"   → Exclude files starting with /usr/share (! prefix)
 *
 * First matching prefix wins. If no prefix matches, file is accepted.
 *
 * @param file    Full path to check
 * @param prefix  NULL-terminated array of prefix rules, or NULL for no filtering
 * @return        TRUE to accept file, FALSE to reject
 *
 * EXAMPLE PREFIXES:
 *   { "/usr", "!/usr/share", "/opt", NULL }
 *   → Accepts /usr/bin/foo, /opt/bar
 *   → Rejects /usr/share/icons/x.png, /home/user/app
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
 * Parse /proc/PID/maps to discover memory-mapped files
 *
 * Reads the memory map of a process and extracts information about
 * file-backed memory regions. This tells us which shared libraries
 * and other files an application uses.
 *
 * /proc/PID/maps FORMAT (each line):
 *   address          perms offset  dev   inode   pathname
 *   7f1234567000-7f1234568000 r-xp 00000000 08:01 12345   /usr/lib/libc.so.6
 *
 * @param pid      Process ID to examine
 * @param maps     Hash table to add map objects to (can be NULL)
 * @param exemaps  Output set of exemap objects (pointer to set, can be NULL)
 * @return         Total size of all file-backed mappings in bytes, 0 on failure
 *
 * FAILURE CASES:
 *   - Process exited between /proc scan and this call
 *   - Permission denied (process owned by different user)
 *   - /proc not mounted (unusual configuration)
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
        unsigned long start, end, offset;
        long length;
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
 * Iterate over all running processes on the system
 *
 * Scans /proc for numeric directories (PIDs), reads /proc/PID/exe to get
 * the executable path, and calls the callback for each valid process.
 *
 * @param func       GHFunc callback: func(GINT_TO_POINTER(pid), exe_path, user_data)
 * @param user_data  Passed through to callback
 *
 * SKIPPED PROCESSES:
 *   - Our own PID (self-preloading is pointless)
 *   - Kernel threads (no /proc/PID/exe symlink)
 *   - Processes that exit between scan and read
 *   - Files filtered by exeprefix configuration
 *
 * GRACEFUL DEGRADATION:
 *   If /proc cannot be opened (very unusual), logs a warning and returns.
 *   The daemon continues, hoping /proc becomes available next cycle.
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
        if (all_digits(entry->d_name)) {
            pid_t pid;
            char name[32];
            char exe_buffer[FILELEN];
            int len;

            pid = atoi(entry->d_name);
            if (pid == selfpid)
                continue;

            g_snprintf(name, sizeof(name) - 1, "/proc/%s/exe", entry->d_name);

            len = readlink(name, exe_buffer, sizeof(exe_buffer));

            if (len <= 0) {
                /* Error occurred - check if it's a permission issue (snap sandbox) */
                int err = errno;
                if (err == EACCES || err == EPERM) {
                    /* Try fallback: read /proc/PID/cmdline for snap apps */
                    char cmdline_path[64];
                    g_snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%s/cmdline", entry->d_name);
                    
                    FILE *cmdline_file = fopen(cmdline_path, "r");
                    if (cmdline_file) {
                        /* cmdline contains null-separated args, first is the executable */
                        len = fread(exe_buffer, 1, sizeof(exe_buffer) - 1, cmdline_file);
                        fclose(cmdline_file);
                        
                        if (len > 0) {
                            exe_buffer[len] = '\0';
                            
                            /* Find first null OR space - cmdline uses null but
                             * some systems/wrappers may use spaces */
                            char *end = exe_buffer;
                            while (*end && *end != ' ' && *end != '\t' && (end - exe_buffer) < len) {
                                end++;
                            }
                            *end = '\0';  /* Truncate at first delimiter */
                            
                            /* Only proceed if we got a valid path starting with / */
                            if (exe_buffer[0] == '/') {
                                g_message("SNAP WORKAROUND: Using cmdline for pid=%d: %s", pid, exe_buffer);
                                goto process_exe;  /* Skip to processing */
                            }
                        }
                    }
                }
                continue;
            }
            if (len == sizeof(exe_buffer)) {
                /* Buffer overflow - path too long */
                g_debug("exe path too long for pid %d", pid);
                continue;
            }

            exe_buffer[len] = '\0';

process_exe:
            /* Log every snap path to trace the flow */
            if (g_str_has_prefix(exe_buffer, "/snap/")) {
                g_message("SNAP DEBUG: At process_exe pid=%d path=%s", pid, exe_buffer);
            }
            
            /* Log first 20 chars of every path to verify we're scanning */
            static int scan_count = 0;
            if (scan_count++ % 50 == 0) {
                g_message("PROC DEBUG: Scanning pid=%d path=%.40s...", pid, exe_buffer);
            }

            /* Debug: Log snap paths being processed - BEFORE filters */
            if (g_str_has_prefix(exe_buffer, "/snap/")) {
                g_message("SNAP DEBUG: Checking filters for pid=%d path=%s", pid, exe_buffer);
            }

            if (!sanitize_file(exe_buffer)) {
                if (g_str_has_prefix(exe_buffer, "/snap/")) {
                    g_message("SNAP DEBUG: Process %d rejected by sanitize_file", pid);
                }
                continue;
            }
            
            if (!accept_file(exe_buffer, kp_conf->system.exeprefix)) {
                if (g_str_has_prefix(exe_buffer, "/snap/")) {
                    g_message("SNAP DEBUG: Process %d rejected by exeprefix filter: %s", pid, exe_buffer);
                }
                continue;
            }

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
 * Read system memory statistics from /proc
 *
 * Parses /proc/meminfo and /proc/vmstat to determine:
 *   - How much memory is available for preloading
 *   - Current I/O activity (page in/out rates)
 *
 * The prediction engine uses this to avoid preloading when:
 *   - System is low on memory
 *   - Heavy I/O activity suggests disk is already busy
 *
 * @param mem  Structure to populate with memory statistics (in KB)
 *
 * VALUES COLLECTED:
 *   total    - Total physical RAM
 *   free     - Completely unused memory
 *   buffers  - File metadata cache
 *   cached   - Page cache (file contents)
 *   pagein   - Cumulative pages read from disk since boot
 *   pageout  - Cumulative pages written to disk since boot
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
