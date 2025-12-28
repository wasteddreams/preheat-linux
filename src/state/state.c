/* state.c - State management implementation for Preheat
 *
 * Based on preload 0.6.4 state.c (VERBATIM helper functions)
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: State Management
 * =============================================================================
 *
 * This module orchestrates Preheat's state management. The actual implementation
 * is split across several submodules:
 *
 * - state_map.c:    Map and exemap management
 * - state_exe.c:    Executable management  
 * - state_markov.c: Markov chain management
 * - state_family.c: Application family management
 * - state_io.c:     State file read/write operations
 *
 * This file contains:
 * - Global state singleton
 * - State lifecycle functions (load, save, free, run)
 * - Daemon tick loop
 *
 * =============================================================================
 */

#include "common.h"
#include "../utils/logging.h"
#include "../config/config.h"
#include "../daemon/pause.h"
#include "../daemon/session.h"
#include "state.h"
#include "state_io.h"
#include "../monitor/proc.h"
#include "../monitor/spy.h"
#include "../predict/prophet.h"
#include "../utils/seeding.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/*
 * Global state singleton.
 * Allocated as array of 1 element (same trick as kp_conf) so it can
 * be used like a pointer: kp_state->exes instead of (&kp_state)->exes
 */
kp_state_t kp_state[1];

/* ========================================================================
 * MODULE EXTRACTION NOTES
 * ========================================================================
 *
 * Map functions -> state_map.c:
 *   kp_map_new, kp_map_free, kp_map_ref, kp_map_unref,
 *   kp_map_get_size, kp_map_hash, kp_map_equal,
 *   kp_exemap_new, kp_exemap_free, kp_exemap_foreach
 *
 * Exe functions -> state_exe.c:
 *   kp_exe_new, kp_exe_free, kp_exe_map_new,
 *   kp_state_register_exe, kp_state_unregister_exe
 *
 * Markov functions -> state_markov.c:
 *   kp_markov_new, kp_markov_state_changed, kp_markov_free,
 *   kp_markov_foreach, kp_markov_correlation
 *
 * Family functions -> state_family.c:
 *   kp_family_new, kp_family_free, kp_family_add_member,
 *   kp_family_update_stats, kp_family_lookup, kp_family_lookup_by_exe
 *
 * I/O functions -> state_io.c:
 *   All read_*, write_*, handle_corrupt_statefile
 *
 * ======================================================================== */

/* ========================================================================
 * STATE LIFECYCLE FUNCTIONS
 * ======================================================================== */

/**
 * Load state from file
 * Modified from upstream to handle corruption gracefully and seed on first run
 */
void kp_state_load(const char *statefile)
{
    gboolean state_was_empty = FALSE;
    
    memset(kp_state, 0, sizeof(*kp_state));
    kp_state->exes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)kp_exe_free);
    kp_state->bad_exes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    kp_state->maps = g_hash_table_new((GHashFunc)kp_map_hash, (GEqualFunc)kp_map_equal);
    kp_state->maps_arr = g_ptr_array_new();

    /* Initialize family hash tables (Enhancement #3) */
    kp_state->app_families = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, (GDestroyNotify)kp_family_free);
    kp_state->exe_to_family = g_hash_table_new_full(g_str_hash, g_str_equal, 
                                                      g_free, g_free);

    if (statefile && *statefile) {
        GIOChannel *f;
        GError *err = NULL;

        g_message("loading state from %s", statefile);

        f = g_io_channel_new_file(statefile, "r", &err);
        if (!f) {
            if (err->code == G_FILE_ERROR_ACCES) {
                g_critical("cannot open %s for reading: %s - continuing without saved state",
                           statefile, err->message);
            } else if (err->code == G_FILE_ERROR_NOENT) {
                g_message("State file not found - first run detected");
                state_was_empty = TRUE;
            } else {
                g_warning("cannot open %s for reading, ignoring: %s", statefile, err->message);
            }
            g_error_free(err);
        } else {
            char *errmsg;

            errmsg = kp_state_read_from_channel(f);
            g_io_channel_unref(f);
            if (errmsg) {
                kp_state_handle_corrupt_file(statefile, errmsg);
                g_free(errmsg);
                state_was_empty = TRUE;
            }
        }

        g_debug("loading state done");
    }

    /* Enhancement #4: Smart first-run seeding */
    if (state_was_empty || (kp_state->exes && g_hash_table_size(kp_state->exes) == 0)) {
        kp_seed_from_sources();
    }

    kp_proc_get_memstat(&(kp_state->memstat));
    kp_state->memstat_timestamp = kp_state->time;
}

/**
 * Register manual apps that aren't already tracked
 */
void
kp_state_register_manual_apps(void)
{
    char **app_path;
    int registered = 0;
    int already_tracked = 0;
    int total = 0;
    
    if (!kp_conf->system.manual_apps_loaded ||
        kp_conf->system.manual_apps_count == 0) {
        g_debug("No manual apps configured");
        return;
    }
    
    g_message("=== Registering manual apps ===");
    
    for (app_path = kp_conf->system.manual_apps_loaded; *app_path; app_path++) {
        kp_exe_t *exe;
        total++;
        
        exe = g_hash_table_lookup(kp_state->exes, *app_path);
        if (exe) {
            g_debug("Manual app already tracked: %s", *app_path);
            already_tracked++;
            continue;
        }
        
        exe = kp_exe_new(*app_path, FALSE, NULL);
        if (!exe) {
            g_warning("Failed to create exe for manual app: %s", *app_path);
            continue;
        }
        
        kp_state_register_exe(exe, FALSE);
        registered++;
        
        g_message("Registered manual app: %s", *app_path);
    }
    
    if (registered > 0 || already_tracked > 0) {
        g_message("Manual apps: %d registered, %d already tracked (of %d total)",
                  registered, already_tracked, total);
    }
    
    if (registered > 0) {
        kp_state->dirty = TRUE;
    }
}

/* Helper for removing all bad_exes */
static gboolean
true_func(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)value;
    (void)user_data;
    return TRUE;
}

/**
 * Save state to file
 */
void kp_state_save(const char *statefile)
{
    if (kp_state->dirty && statefile && *statefile) {
        int fd = -1;
        GIOChannel *f;
        char *tmpfile;

        g_message("saving state to %s", statefile);

        tmpfile = g_strconcat(statefile, ".tmp", NULL);
        g_debug("to be honest, saving state to %s", tmpfile);

        fd = open(tmpfile, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            g_critical("cannot open %s for writing, ignoring: %s", tmpfile, strerror(errno));
        } else {
            char *errmsg;

            f = g_io_channel_unix_new(fd);

            errmsg = kp_state_write_to_channel(f, fd);
            g_io_channel_flush(f, NULL);
            g_io_channel_unref(f);

            if (errmsg) {
                g_critical("failed writing state to %s, ignoring: %s", tmpfile, errmsg);
                g_free(errmsg);
                close(fd);
                unlink(tmpfile);
            } else {
                if (fsync(fd) < 0) {
                    g_critical("fsync failed for %s: %s - state may be lost on crash",
                               tmpfile, strerror(errno));
                }
                close(fd);

                if (rename(tmpfile, statefile) < 0) {
                    g_critical("failed to rename %s to %s: %s",
                               tmpfile, statefile, strerror(errno));
                    unlink(tmpfile);
                } else {
                    g_debug("successfully renamed %s to %s", tmpfile, statefile);
                }
            }
        }

        g_free(tmpfile);

        kp_state->dirty = FALSE;

        g_debug("saving state done");
    }

    /* B009: Clear bad_exes after save. This is intentional - bad_exes
     * contains paths that failed to stat/open during this cycle. Clearing
     * after save ensures we don't accumulate stale entries, and transient
     * failures (like unmounted filesystems) get re-tried on next cycle. */
    g_hash_table_foreach_remove(kp_state->bad_exes, true_func, NULL);
}

/**
 * Free state memory
 */
void kp_state_free(void)
{
    g_message("freeing state memory begin");
    g_hash_table_destroy(kp_state->bad_exes);
    kp_state->bad_exes = NULL;
    g_hash_table_destroy(kp_state->exes);
    kp_state->exes = NULL;

    if (kp_state->app_families) {
        g_hash_table_destroy(kp_state->app_families);
        kp_state->app_families = NULL;
    }
    if (kp_state->exe_to_family) {
        g_hash_table_destroy(kp_state->exe_to_family);
        kp_state->exe_to_family = NULL;
    }

    g_assert(g_hash_table_size(kp_state->maps) == 0);
    g_assert(kp_state->maps_arr->len == 0);
    g_hash_table_destroy(kp_state->maps);
    kp_state->maps = NULL;
    g_slist_free(kp_state->running_exes);
    kp_state->running_exes = NULL;
    g_ptr_array_free(kp_state->maps_arr, TRUE);
    g_debug("freeing state memory done");
}

/**
 * Dump state to log
 */
void kp_state_dump_log(void)
{
    g_message("state log dump requested");
    fprintf(stderr, "persistent state stats:\n");
    fprintf(stderr, "preload time = %d\n", kp_state->time);
    fprintf(stderr, "num exes = %d\n", g_hash_table_size(kp_state->exes));
    fprintf(stderr, "num bad exes = %d\n", g_hash_table_size(kp_state->bad_exes));
    fprintf(stderr, "num maps = %d\n", g_hash_table_size(kp_state->maps));
    fprintf(stderr, "runtime state stats:\n");
    fprintf(stderr, "num running exes = %d\n", g_slist_length(kp_state->running_exes));
    g_debug("state log dump done");
}

/* ========================================================================
 * STATE PERIODIC TASKS - The Daemon's Heartbeat
 * ======================================================================== */

static gboolean kp_state_tick(gpointer data);

static gboolean
kp_state_tick2(gpointer data)
{
    if (kp_state->model_dirty) {
        g_debug("state updating begin");
        kp_spy_update_model(data);
        kp_state->model_dirty = FALSE;
        g_debug("state updating end");
    }

    kp_state->time += (kp_conf->model.cycle + 1) / 2;
    g_timeout_add_seconds((kp_conf->model.cycle + 1) / 2, kp_state_tick, data);
    return FALSE;
}

static gboolean
kp_state_tick(gpointer data)
{
    if (kp_conf->system.doscan) {
        g_debug("state scanning begin");
        kp_spy_scan(data);
        kp_state->dirty = kp_state->model_dirty = TRUE;
        g_debug("state scanning end");
    }
    if (kp_conf->system.dopredict) {
        if (kp_pause_is_active()) {
            g_debug("preloading paused - skipping prediction");
        } else {
            kp_session_check();
            if (kp_session_in_boot_window()) {
                g_debug("session boot window active (%d sec remaining)",
                        kp_session_window_remaining());
                kp_session_preload_top_apps(5);
            }

            g_debug("state predicting begin");
            kp_prophet_predict(data);
            g_debug("state predicting end");
        }
    }

    kp_state->time += kp_conf->model.cycle / 2;
    g_timeout_add_seconds(kp_conf->model.cycle / 2, kp_state_tick2, data);
    return FALSE;
}

static const char *autosave_statefile;

/* B008 FIX: Eviction thresholds */
#define EXE_EVICTION_THRESHOLD 1500  /* Start evicting when above this count */
#define EXE_EVICTION_MAX_AGE (30 * 24 * 3600)  /* 30 days in seconds */

/**
 * B008 FIX: Check if an exe should be evicted
 * Returns TRUE if exe is old and unused (not seen in 30+ days, zero weight)
 */
static gboolean
should_evict_exe(gpointer key, gpointer value, gpointer user_data)
{
    kp_exe_t *exe = (kp_exe_t *)value;
    int current_time = *(int *)user_data;
    
    (void)key;
    
    /* Keep if has any weighted launches */
    if (exe->weighted_launches > 0.1)
        return FALSE;
    
    /* Keep if running recently */
    if (exe->running_timestamp > current_time - EXE_EVICTION_MAX_AGE)
        return FALSE;
    
    /* Evict: old and unused */
    return TRUE;
}

static gboolean
kp_state_autosave(gpointer user_data)
{
    (void)user_data;
    
    /* B008 FIX: Evict old unused exes if table is too large */
    guint exe_count = g_hash_table_size(kp_state->exes);
    if (exe_count > EXE_EVICTION_THRESHOLD) {
        int current_time = kp_state->time;
        guint before = exe_count;
        g_hash_table_foreach_remove(kp_state->exes, should_evict_exe, &current_time);
        guint after = g_hash_table_size(kp_state->exes);
        if (after < before) {
            g_message("B008: Evicted %u old unused exes (%u -> %u)", 
                      before - after, before, after);
        }
    }
    
    kp_state_save(autosave_statefile);

    g_timeout_add_seconds(kp_conf->system.autosave, kp_state_autosave, NULL);
    return FALSE;
}

/**
 * Start state periodic tasks
 */
void kp_state_run(const char *statefile)
{
    g_timeout_add(0, kp_state_tick, NULL);
    if (statefile) {
        autosave_statefile = statefile;
        g_timeout_add_seconds(kp_conf->system.autosave, kp_state_autosave, NULL);
    }
}
