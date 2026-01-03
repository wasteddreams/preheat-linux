/* lib_scanner.c - Shared library discovery for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Discovers shared libraries via:
 * 1. ldd (ELF-linked dependencies)
 * 2. Directory scan (for dlopen'd libraries like Firefox's libxul.so)
 */

#include "lib_scanner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <glib.h>

#define MAX_LIBS 256
#define LINE_SIZE 1024
#define MIN_LIB_SIZE (64 * 1024)  /* Only scan libs > 64 KB */

/**
 * Scan directory for .so files (catches dlopen'd libs like libxul.so)
 */
static void
scan_dir_for_libs(const char *dir_path, GPtrArray *libs)
{
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char full_path[PATH_MAX];
    
    dir = opendir(dir_path);
    if (!dir)
        return;
    
    while ((entry = readdir(dir)) != NULL && libs->len < MAX_LIBS) {
        /* Look for .so files */
        const char *name = entry->d_name;
        size_t len = strlen(name);
        
        if (len < 4)
            continue;
        
        /* Check for .so extension or .so.N pattern */
        if (!strstr(name, ".so"))
            continue;
        
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
        
        /* Skip if not a regular file or too small */
        if (stat(full_path, &st) < 0 || !S_ISREG(st.st_mode))
            continue;
        
        if (st.st_size < MIN_LIB_SIZE)
            continue;
        
        g_ptr_array_add(libs, g_strdup(full_path));
    }
    
    closedir(dir);
}

/**
 * Scan executable for shared library dependencies
 * Uses ldd for ELF deps + directory scan for dlopen'd libs
 */
char **
kp_scan_libraries(const char *exe_path)
{
    char cmd[PATH_MAX + 32];
    char line[LINE_SIZE];
    FILE *fp;
    GPtrArray *libs;
    char *exe_dir;
    
    if (!exe_path)
        return NULL;
    
    libs = g_ptr_array_new();
    
    /* Phase 1: Use ldd for ELF-linked dependencies */
    /* BUG 1 FIX: Use g_shell_quote to prevent command injection */
    gchar *quoted_path = g_shell_quote(exe_path);
    snprintf(cmd, sizeof(cmd), "/usr/bin/ldd %s 2>/dev/null", quoted_path);
    g_free(quoted_path);
    
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp) && libs->len < MAX_LIBS) {
            char *arrow, *path_start, *path_end;
            char *lib_path = NULL;
            
            /* Skip virtual libraries */
            if (strstr(line, "linux-vdso") || 
                strstr(line, "linux-gate") ||
                strstr(line, "not found")) {
                continue;
            }
            
            /* Parse " => /path" pattern */
            arrow = strstr(line, " => ");
            if (arrow) {
                path_start = arrow + 4;
                path_end = strchr(path_start, ' ');
                if (!path_end) path_end = strchr(path_start, '\n');
                
                if (path_end && path_end > path_start) {
                    lib_path = g_strndup(path_start, path_end - path_start);
                }
            } else {
                /* Direct path: "    /lib64/ld-linux.so" */
                char *start = line;
                while (*start == ' ' || *start == '\t') start++;
                
                if (*start == '/') {
                    path_end = strchr(start, ' ');
                    if (!path_end) path_end = strchr(start, '\n');
                    
                    if (path_end && path_end > start) {
                        lib_path = g_strndup(start, path_end - start);
                    }
                }
            }
            
            /* Add valid paths, skip ld-linux */
            if (lib_path && lib_path[0] == '/' && !strstr(lib_path, "ld-linux")) {
                g_ptr_array_add(libs, lib_path);
            } else {
                g_free(lib_path);
            }
        }
        pclose(fp);
    }
    
    /* Phase 2: Scan executable's directory for .so files (dlopen'd libs) */
    exe_dir = g_path_get_dirname(exe_path);
    if (exe_dir && strcmp(exe_dir, ".") != 0 && strcmp(exe_dir, "/usr/bin") != 0) {
        /* Only scan app-specific dirs like /usr/lib/firefox-esr/, not /usr/bin */
        scan_dir_for_libs(exe_dir, libs);
    }
    g_free(exe_dir);
    
    if (libs->len == 0) {
        g_ptr_array_free(libs, TRUE);
        return NULL;
    }
    
    g_ptr_array_add(libs, NULL);  /* NULL terminator */
    
    g_debug("lib_scanner: found %u libraries for %s", libs->len - 1, exe_path);
    
    return (char **)g_ptr_array_free(libs, FALSE);
}

/**
 * Free library list returned by kp_scan_libraries
 */
void
kp_free_library_list(char **libs)
{
    if (!libs)
        return;
    
    for (int i = 0; libs[i]; i++) {
        g_free(libs[i]);
    }
    g_free(libs);
}

