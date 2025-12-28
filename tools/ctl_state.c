/* ctl_state.c - State file and path utilities for preheat-ctl
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * =============================================================================
 * MODULE: Path Matching Utilities
 * =============================================================================
 *
 * These functions handle the complexity of matching user-provided app names
 * against state file entries, which are stored as file:// URIs.
 *
 * Provides:
 *   - URI to path conversion (file:// -> /path/to/file)
 *   - Multi-layer path matching (exact, substring, basename)
 *   - App name resolution (/usr/bin, /bin, /usr/local/bin search)
 *
 * =============================================================================
 */

#define _DEFAULT_SOURCE  /* For realpath() */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <linux/limits.h>
#include <libgen.h>
#include <glib.h>

#include "ctl_state.h"

/**
 * Convert file:// URI to plain filesystem path
 * Returns newly allocated string (caller must free) or NULL on error
 */
char *
uri_to_path(const char *uri)
{
    if (!uri) return NULL;
    
    /* GLib's g_filename_from_uri handles URL decoding and validation */
    return g_filename_from_uri(uri, NULL, NULL);
}

/**
 * Check if string is a file:// URI
 */
gboolean
is_uri(const char *str)
{
    return str && g_str_has_prefix(str, "file://");
}

/**
 * Check if two paths match, handling URIs, basenames, and partial paths
 */
gboolean
paths_match(const char *search_path, const char *state_path)
{
    char *state_plain = NULL;
    char *search_basename = NULL;
    char *state_basename = NULL;
    gboolean match = FALSE;
    
    if (!search_path || !state_path) {
        return FALSE;
    }
    
    /* Convert URI to plain path if needed */
    if (is_uri(state_path)) {
        state_plain = uri_to_path(state_path);
        if (!state_plain) return FALSE;
    } else {
        state_plain = g_strdup(state_path);
    }
    
    /* Layer 1: Exact match (fastest) */
    if (strcmp(search_path, state_plain) == 0) {
        match = TRUE;
        goto cleanup;
    }
    
    /* Layer 2: Substring match (for partial paths like "bin/antigravity") */
    if (strstr(state_plain, search_path) || strstr(search_path, state_plain)) {
        match = TRUE;
        goto cleanup;
    }
    
    /* Layer 3: Basename match (handles "firefox" vs "/usr/lib/firefox") */
    {
        char *search_copy = g_strdup(search_path);
        char *state_copy = g_strdup(state_plain);
        
        search_basename = g_strdup(basename(search_copy));
        state_basename = g_strdup(basename(state_copy));
        
        g_free(search_copy);
        g_free(state_copy);
    }
    
    if (strcmp(search_basename, state_basename) == 0) {
        match = TRUE;
        goto cleanup;
    }
    
cleanup:
    g_free(state_plain);
    g_free(search_basename);
    g_free(state_basename);
    return match;
}

/**
 * Try to resolve app name to full path
 * Returns resolved path, or original name if not found
 * Now follows symlinks to find the actual binary
 */
const char *
resolve_app_name(const char *name, char *buffer, size_t bufsize)
{
    char temp_path[PATH_MAX];
    char *resolved = NULL;
    
    if (!name) {
        return name;
    }
    
    /* If already absolute, try to resolve it */
    if (name[0] == '/') {
        resolved = realpath(name, buffer);
        return resolved ? resolved : name;
    }
    
    /* Try common paths */
    snprintf(temp_path, sizeof(temp_path), "/usr/bin/%s", name);
    if (access(temp_path, F_OK) == 0) {
        resolved = realpath(temp_path, buffer);
        if (resolved) return resolved;
        /* realpath failed - copy temp_path to buffer to avoid returning local addr */
        strncpy(buffer, temp_path, bufsize - 1);
        buffer[bufsize - 1] = '\0';
        return buffer;
    }
    
    snprintf(temp_path, sizeof(temp_path), "/bin/%s", name);
    if (access(temp_path, F_OK) == 0) {
        resolved = realpath(temp_path, buffer);
        if (resolved) return resolved;
        strncpy(buffer, temp_path, bufsize - 1);
        buffer[bufsize - 1] = '\0';
        return buffer;
    }
    
    snprintf(temp_path, sizeof(temp_path), "/usr/local/bin/%s", name);
    if (access(temp_path, F_OK) == 0) {
        resolved = realpath(temp_path, buffer);
        if (resolved) return resolved;
        strncpy(buffer, temp_path, bufsize - 1);
        buffer[bufsize - 1] = '\0';
        return buffer;
    }
    
    return name;  /* Not found, use original */
}
