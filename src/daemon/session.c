/* session.c - Session-aware preloading implementation for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: Session-Aware Preloading
 * =============================================================================
 *
 * Detects user login and aggressively preloads top applications during
 * the "boot window" (first 3 minutes after login).
 *
 * SESSION DETECTION:
 *   Monitors /run/user/$UID directory creation, which indicates:
 *   - User has logged in via display manager (GDM, SDDM, LightDM)
 *   - systemd-logind has created user session
 *
 * BOOT WINDOW BEHAVIOR:
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │ Time 0 (login)                                              │
 *   │   ↓ Session detected, window opens                          │
 *   │   ↓ kp_session_preload_top_apps(5) boosts top 5 apps        │
 *   │   ↓ Apps get lnprob = -15.0 (very high priority)            │
 *   │   ↓ Next prediction cycle immediately preloads them         │
 *   │ Time 180s (3 min)                                           │
 *   │   ↓ Window closes, normal prediction resumes                │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * TOP APP SELECTION:
 *   Apps are ranked by total running time (exe->time). Applications
 *   with more usage history are assumed to be more important to the user.
 *
 * MEMORY SAFETY:
 *   Aggressive preloading only runs if ≥20% memory is available,
 *   preventing out-of-memory situations on low-RAM systems.
 *
 * =============================================================================
 */

#include "common.h"
#include "session.h"
#include "../utils/logging.h"
#include "../utils/lib_scanner.h"
#include "../config/config.h"
#include "../state/state.h"
#include "../predict/prophet.h"

#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>

/* Session detection settings */
#define SESSION_WINDOW_DEFAULT 180    /* 3 minutes */
#define SESSION_MAX_APPS_DEFAULT 5
#define SESSION_MEMORY_THRESHOLD 20   /* 20% minimum free */

/**
 * Load a single file as a map for an exe
 */
static gboolean
load_single_map(kp_exe_t *exe, const char *path)
{
    struct stat st;
    kp_map_t *map;
    kp_exemap_t *exemap;
    
    if (stat(path, &st) < 0)
        return FALSE;
    
    if ((size_t)st.st_size < (size_t)kp_conf->model.minsize)
        return FALSE;
    
    map = kp_map_new(path, 0, st.st_size);
    if (!map)
        return FALSE;
    
    exemap = kp_exe_map_new(exe, map);
    if (!exemap) {
        kp_map_free(map);
        return FALSE;
    }
    
    exemap->prob = 1.0;
    exe->size += st.st_size;
    
    return TRUE;
}

/**
 * Load memory maps for a session app including shared libraries
 */
static gboolean
load_maps_for_session_app(kp_exe_t *exe)
{
    char **libs;
    int loaded = 0;
    
    if (!exe || !exe->path)
        return FALSE;
    
    exe->size = 0;  /* Reset size counter */
    
    /* Load main binary */
    if (load_single_map(exe, exe->path)) {
        loaded++;
        g_debug("Session: loaded binary %s", exe->path);
    }
    
    /* Scan and load shared libraries */
    libs = kp_scan_libraries(exe->path);
    if (libs) {
        for (int i = 0; libs[i]; i++) {
            if (load_single_map(exe, libs[i])) {
                loaded++;
            }
        }
        kp_free_library_list(libs);
    }
    
    if (loaded > 0) {
        g_message("Session: loaded %d maps for %s (%.1f MB total)",
                  loaded, exe->path, (double)exe->size / (1024 * 1024));
        return TRUE;
    }
    
    return FALSE;
}

/* Global session state */
static struct {
    gboolean initialized;
    gboolean session_detected;
    time_t session_start;
    time_t window_end;
    int window_duration_sec;
    int max_apps;
    uid_t target_uid;
    gboolean preload_done;
} session_state = {0};

/**
 * Get the primary user UID (first non-root user with UID >= 1000)
 */
static uid_t
get_primary_user_uid(void)
{
    uid_t uid = getuid();

    /* If running as root, try to find the primary user */
    if (uid == 0) {
        /* Check SUDO_UID first */
        const char *sudo_uid = getenv("SUDO_UID");
        if (sudo_uid) {
            return (uid_t)atoi(sudo_uid);
        }

        /* Default to UID 1000 (typical first user) */
        return 1000;
    }

    return uid;
}

/**
 * Get session creation time from /run/user/$UID directory
 * Uses birth time (st_birthtime) if available, falls back to ctime
 * 
 * @return session start time, or 0 if session doesn't exist
 */
static time_t
get_session_creation_time(uid_t uid)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "/run/user/%d", uid);

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* On Linux, st_ctime is metadata change time (close to creation).
         * For /run/user/$UID specifically, this is reliable since the dir
         * is created fresh on each login and rarely modified. */
        return st.st_ctime;
    }

    return 0;
}

/**
 * Check if user session directory exists
 * Uses /run/user/$UID as session indicator
 */
static gboolean
check_user_session_exists(uid_t uid)
{
    return get_session_creation_time(uid) > 0;
}

/**
 * Check system memory availability
 * @return TRUE if enough memory available for aggressive preload
 */
static gboolean
check_memory_available(void)
{
    FILE *f;
    char line[256];
    long mem_total = 0, mem_available = 0;

    f = fopen("/proc/meminfo", "r");
    if (!f) return FALSE;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemAvailable: %ld kB", &mem_available) == 1) break;
    }
    fclose(f);

    if (mem_total == 0) return FALSE;

    int percent_available = (int)((mem_available * 100) / mem_total);

    if (percent_available < SESSION_MEMORY_THRESHOLD) {
        g_debug("Session preload: low memory (%d%% available), skipping", percent_available);
        return FALSE;
    }

    return TRUE;
}

/**
 * Initialize session detection subsystem
 */
void
kp_session_init(void)
{
    session_state.initialized = TRUE;
    session_state.session_detected = FALSE;
    session_state.preload_done = FALSE;
    session_state.window_duration_sec = SESSION_WINDOW_DEFAULT;
    session_state.max_apps = SESSION_MAX_APPS_DEFAULT;
    session_state.target_uid = get_primary_user_uid();

    /* Check if session already exists (daemon started after login) */
    time_t session_created = get_session_creation_time(session_state.target_uid);
    if (session_created > 0) {
        time_t now = time(NULL);
        time_t session_age = now - session_created;
        
        session_state.session_detected = TRUE;
        session_state.session_start = session_created;  /* Use REAL login time! */
        session_state.window_end = session_created + session_state.window_duration_sec;
        
        if (session_age >= session_state.window_duration_sec) {
            /* Window already expired - user logged in too long ago */
            g_message("Session for UID %d started %ld seconds ago, boot window expired",
                      session_state.target_uid, (long)session_age);
            session_state.preload_done = TRUE;  /* Skip aggressive preload */
        } else {
            /* Still within window */
            int remaining = session_state.window_duration_sec - (int)session_age;
            g_message("Session for UID %d started %ld sec ago, boot window active (%d sec remaining)",
                      session_state.target_uid, (long)session_age, remaining);
        }
    }

    g_debug("Session detection initialized for UID %d", session_state.target_uid);
}

/**
 * Check for user session start
 */
gboolean
kp_session_check(void)
{
    if (!session_state.initialized) {
        kp_session_init();
    }

    /* Already detected */
    if (session_state.session_detected) {
        return FALSE;
    }

    /* Check if session directory appeared */
    time_t session_created = get_session_creation_time(session_state.target_uid);
    if (session_created > 0) {
        session_state.session_detected = TRUE;
        session_state.session_start = session_created;  /* Use REAL creation time */
        session_state.window_end = session_created + session_state.window_duration_sec;

        g_message("Session detected for UID %d, starting %d second boot window",
                  session_state.target_uid, session_state.window_duration_sec);

        return TRUE;
    }

    return FALSE;
}

/**
 * Check if currently in boot/login window
 */
gboolean
kp_session_in_boot_window(void)
{
    time_t now;

    if (!session_state.session_detected) {
        return FALSE;
    }

    if (session_state.preload_done) {
        return FALSE;
    }

    now = time(NULL);

    if (now >= session_state.window_end) {
        g_message("Session boot window ended after %d seconds",
                  session_state.window_duration_sec);
        session_state.preload_done = TRUE;
        return FALSE;
    }

    return TRUE;
}

/**
 * Get remaining seconds in boot window
 */
int
kp_session_window_remaining(void)
{
    time_t now;

    if (!session_state.session_detected || session_state.preload_done) {
        return 0;
    }

    now = time(NULL);

    if (now >= session_state.window_end) {
        return 0;
    }

    return (int)(session_state.window_end - now);
}

/**
 * Compare function for sorting exes by usage time (most used first)
 */
static gint
exe_usage_compare(gconstpointer a, gconstpointer b)
{
    const kp_exe_t *exe_a = *(const kp_exe_t **)a;
    const kp_exe_t *exe_b = *(const kp_exe_t **)b;

    /* Sort by time (higher = more used = first) */
    if (exe_a->time > exe_b->time) return -1;
    if (exe_a->time < exe_b->time) return 1;
    return 0;
}

/**
 * Get top N most-used applications
 */
static GPtrArray *
get_top_apps(int max_apps)
{
    GPtrArray *apps;
    GHashTableIter iter;
    gpointer key, value;

    apps = g_ptr_array_new();

    /* Safety check: state may not be loaded yet */
    if (!kp_state->exes) {
        return apps;  /* Return empty array */
    }

    /* Collect all exes */
    g_hash_table_iter_init(&iter, kp_state->exes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        kp_exe_t *exe = (kp_exe_t *)value;

        /* Skip if currently running */
        if (exe_is_running(exe)) continue;

        /* Skip if not enough usage history */
        if (exe->time < 10) continue;  /* At least 10 seconds of use */

        g_ptr_array_add(apps, exe);
    }

    /* Sort by usage time */
    g_ptr_array_sort(apps, exe_usage_compare);

    /* Trim to max_apps */
    while (apps->len > (guint)max_apps) {
        g_ptr_array_remove_index(apps, apps->len - 1);
    }

    return apps;
}

/**
 * Trigger aggressive preload of top N apps
 */
void
kp_session_preload_top_apps(int max_apps)
{
    GPtrArray *top_apps;
    int preloaded = 0;
    int maps_loaded = 0;

    if (!check_memory_available()) {
        g_debug("Session preload: skipping due to memory constraints");
        return;
    }

    top_apps = get_top_apps(max_apps);

    g_message("Session preload: boosting top %d applications", top_apps->len);

    for (guint i = 0; i < top_apps->len; i++) {
        kp_exe_t *exe = g_ptr_array_index(top_apps, i);

        /* Load maps if not already loaded (seeded apps don't have maps) */
        if (g_set_size(exe->exemaps) == 0) {
            if (load_maps_for_session_app(exe)) {
                maps_loaded++;
            }
        }

        /* Give strong negative lnprob to trigger immediate preload */
        exe->lnprob = -15.0;  /* Very high priority */
        preloaded++;

        g_debug("Session preload: boosting %s (usage: %d sec, maps: %u)",
                exe->path, exe->time, g_set_size(exe->exemaps));
    }

    g_ptr_array_free(top_apps, TRUE);

    if (preloaded > 0) {
        g_message("Session preload: %d apps boosted (%d maps loaded)", 
                  preloaded, maps_loaded);
    }
}

/**
 * Free session resources
 */
void
kp_session_free(void)
{
    session_state.initialized = FALSE;
    session_state.session_detected = FALSE;
    session_state.preload_done = FALSE;
}
