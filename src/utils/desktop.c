/* desktop.c - Desktop file scanning implementation
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.h"
#include "desktop.h"
#include "logging.h"
#include <sys/stat.h>

/**
 * Desktop application entry
 */
typedef struct {
    char *app_name;        /* Display name (e.g., "Firefox") */
    char *exec_path;       /* Resolved executable path */
    char *desktop_file;    /* Path to .desktop file */
} desktop_app_t;

/* Global registry: exe_path → desktop_app_t */
static GHashTable *desktop_apps = NULL;

/**
 * Free desktop app entry
 */
static void
desktop_app_free(gpointer data)
{
    desktop_app_t *app = data;
    if (app) {
        g_free(app->app_name);
        g_free(app->exec_path);
        g_free(app->desktop_file);
        g_free(app);
    }
}

/**
 * Try to resolve snap wrapper to actual binary
 *
 * Snap installs wrappers in /snap/bin/<name> that are scripts, not symlinks.
 * This function finds the actual binary inside the snap squashfs mount.
 *
 * Common patterns:
 *   /snap/<name>/current/usr/bin/<name>
 *   /snap/<name>/current/usr/lib/<name>/<name>
 *   /snap/<name>/current/bin/<name>
 *
 * @param wrapper_path  Path like /snap/bin/firefox
 * @return              Resolved binary path (caller frees), or NULL if not found
 */
static char *
resolve_snap_binary(const char *wrapper_path)
{
    char snap_path[PATH_MAX];
    char resolved[PATH_MAX];
    const char *snap_name;
    
    /* Only handle /snap/bin/ wrappers */
    if (!wrapper_path || !g_str_has_prefix(wrapper_path, "/snap/bin/")) {
        return NULL;
    }
    
    /* Extract snap name from /snap/bin/<name> */
    snap_name = wrapper_path + strlen("/snap/bin/");
    if (!*snap_name) {
        return NULL;
    }
    
    /* Try common snap binary locations (use /current for version-independent path) */
    
    /* Pattern 1: /snap/<name>/current/usr/lib/<name>/<name> (Firefox, etc.) */
    g_snprintf(snap_path, sizeof(snap_path),
               "/snap/%s/current/usr/lib/%s/%s", snap_name, snap_name, snap_name);
    if (access(snap_path, X_OK) == 0 && realpath(snap_path, resolved)) {
        g_debug("Snap resolution: %s → %s", wrapper_path, resolved);
        return g_strdup(resolved);
    }
    
    /* Pattern 2: /snap/<name>/current/usr/bin/<name> */
    g_snprintf(snap_path, sizeof(snap_path),
               "/snap/%s/current/usr/bin/%s", snap_name, snap_name);
    if (access(snap_path, X_OK) == 0 && realpath(snap_path, resolved)) {
        g_debug("Snap resolution: %s → %s", wrapper_path, resolved);
        return g_strdup(resolved);
    }
    
    /* Pattern 3: /snap/<name>/current/bin/<name> */
    g_snprintf(snap_path, sizeof(snap_path),
               "/snap/%s/current/bin/%s", snap_name, snap_name);
    if (access(snap_path, X_OK) == 0 && realpath(snap_path, resolved)) {
        g_debug("Snap resolution: %s → %s", wrapper_path, resolved);
        return g_strdup(resolved);
    }
    
    g_debug("Snap resolution failed for: %s", wrapper_path);
    return NULL;
}

/**
 * Resolve Exec= line to actual executable path
 *
 * Handles:
 * - Absolute paths: /usr/bin/firefox
 * - Relative commands: firefox → /usr/bin/firefox (via PATH)
 * - Arguments: "firefox %u" → /usr/bin/firefox
 * - Field codes: %u, %U, %f, %F (removed)
 * - Snap wrappers: /snap/bin/firefox → /snap/firefox/current/usr/lib/firefox/firefox
 */
static char *
resolve_exec_path(const char *exec_line)
{
    char **argv = NULL;
    char *resolved = NULL;
    char *binary;

    if (!exec_line || !*exec_line) {
        return NULL;
    }

    /* Parse Exec= line using shell parsing (handles quotes, spaces) */
    if (!g_shell_parse_argv(exec_line, NULL, &argv, NULL)) {
        return NULL;
    }

    if (!argv || !argv[0]) {
        g_strfreev(argv);
        return NULL;
    }

    binary = argv[0];

    /* Resolve to absolute path */
    if (binary[0] == '/') {
        /* Already absolute */
        resolved = g_strdup(binary);
    } else {
        /* Search in PATH */
        resolved = g_find_program_in_path(binary);
    }

    g_strfreev(argv);

    /* Canonicalize path */
    if (resolved) {
        char canonical[PATH_MAX];
        if (realpath(resolved, canonical)) {
            g_free(resolved);
            resolved = g_strdup(canonical);
        }
    }

    /* Handle snap wrappers: /snap/bin/X are scripts, not symlinks to binaries */
    if (resolved && g_str_has_prefix(resolved, "/snap/bin/")) {
        char *snap_resolved = resolve_snap_binary(resolved);
        if (snap_resolved) {
            g_free(resolved);
            resolved = snap_resolved;
        }
    }

    return resolved;
}

/**
 * Parse a single .desktop file
 */
static void
parse_desktop_file(const char *path)
{
    GKeyFile *kf;
    GError *error = NULL;
    char *exec = NULL;
    char *name = NULL;
    char *resolved_path = NULL;
    desktop_app_t *app;
    gboolean is_hidden;

    kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
        g_debug("Cannot load desktop file %s: %s", path, error->message);
        g_error_free(error);
        g_key_file_free(kf);
        return;
    }

    /* Skip hidden applications (NoDisplay=true or Hidden=true) */
    is_hidden = g_key_file_get_boolean(kf, "Desktop Entry", "NoDisplay", NULL);
    if (!is_hidden) {
        is_hidden = g_key_file_get_boolean(kf, "Desktop Entry", "Hidden", NULL);
    }
    if (is_hidden) {
        g_key_file_free(kf);
        return;
    }

    /* Get Exec= and Name= */
    exec = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
    name = g_key_file_get_string(kf, "Desktop Entry", "Name", NULL);

    if (!exec) {
        g_debug("Desktop file %s has no Exec= line", path);
        goto cleanup;
    }

    /* Resolve Exec= to actual binary path */
    resolved_path = resolve_exec_path(exec);
    if (!resolved_path) {
        g_debug("Cannot resolve Exec=%s from %s", exec, path);
        goto cleanup;
    }

    /* Check if already registered (first .desktop file wins) */
    if (g_hash_table_contains(desktop_apps, resolved_path)) {
        g_debug("Already registered: %s (from earlier .desktop)", resolved_path);
        goto cleanup;
    }

    /* Create and register app */
    app = g_new0(desktop_app_t, 1);
    app->app_name = name ? name : g_strdup("Unknown");
    app->exec_path = g_strdup(resolved_path);
    app->desktop_file = g_strdup(path);

    g_hash_table_insert(desktop_apps, g_strdup(resolved_path), app);
    g_debug("Registered desktop app: %s (%s)", app->app_name, resolved_path);

    name = NULL;  /* Ownership transferred to app */

cleanup:
    g_free(exec);
    g_free(name);
    g_free(resolved_path);
    g_key_file_free(kf);
}

/**
 * Scan a directory for .desktop files
 */
static void
scan_desktop_dir(const char *dir_path)
{
    GDir *dir;
    const char *filename;
    struct stat st;

    /* Check if directory exists */
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        g_debug("Desktop directory not found: %s", dir_path);
        return;
    }

    dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        g_debug("Cannot open desktop directory: %s", dir_path);
        return;
    }

    g_debug("Scanning desktop files in: %s", dir_path);

    while ((filename = g_dir_read_name(dir))) {
        if (g_str_has_suffix(filename, ".desktop")) {
            char *full_path = g_build_filename(dir_path, filename, NULL);
            parse_desktop_file(full_path);
            g_free(full_path);
        }
    }

    g_dir_close(dir);
}

/**
 * Initialize desktop file scanner
 */
void
kp_desktop_init(void)
{
    const char *home;
    char *user_dir;
    int count;

    if (desktop_apps) {
        g_warning("Desktop scanner already initialized");
        return;
    }

    desktop_apps = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, desktop_app_free);

    /* Scan system directories */
    scan_desktop_dir("/usr/share/applications");
    scan_desktop_dir("/usr/local/share/applications");

    /* Scan snap desktop files (Ubuntu/snapd) */
    scan_desktop_dir("/var/lib/snapd/desktop/applications");

    /* Scan user directory */
    home = g_get_home_dir();
    if (home) {
        user_dir = g_build_filename(home, ".local/share/applications", NULL);
        scan_desktop_dir(user_dir);
        g_free(user_dir);
    }

    count = g_hash_table_size(desktop_apps);
    g_message("Desktop scanner initialized: discovered %d GUI applications", count);
}

/**
 * Check if an executable has a .desktop file
 */
gboolean
kp_desktop_has_file(const char *exe_path)
{
    if (!desktop_apps || !exe_path) {
        return FALSE;
    }

    return g_hash_table_contains(desktop_apps, exe_path);
}

/**
 * Get application name from .desktop file
 */
const char *
kp_desktop_get_name(const char *exe_path)
{
    desktop_app_t *app;

    if (!desktop_apps || !exe_path) {
        return NULL;
    }

    app = g_hash_table_lookup(desktop_apps, exe_path);
    return app ? app->app_name : NULL;
}

/**
 * Free desktop scanner resources
 */
void
kp_desktop_free(void)
{
    if (desktop_apps) {
        g_hash_table_destroy(desktop_apps);
        desktop_apps = NULL;
        g_debug("Desktop scanner freed");
    }
}
