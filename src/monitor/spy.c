/* spy.c - Process tracking for Preheat
 *
 * Based on preload 0.6.4 spy.c (VERBATIM implementation)
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: Process Tracking ("Spy")
 * =============================================================================
 *
 * This module observes which processes are running and tracks state changes
 * to build the prediction model. It's called "spy" because it watches what
 * the user is doing without modifying behavior.
 *
 * TWO-PHASE SCANNING:
 *   The daemon calls these functions in sequence each cycle:
 *
 *   PHASE 1 - kp_spy_scan():
 *     Called at start of cycle. Scans /proc for running processes and:
 *     - Updates timestamps for already-known executables
 *     - Queues newly-discovered executables for evaluation
 *     - Identifies executables that stopped running
 *
 *   PHASE 2 - kp_spy_update_model():
 *     Called half-cycle later (10 seconds by default). Updates model:
 *     - Evaluates queued new executables (size check, map extraction)
 *     - Registers worthy apps in the state, blacklists small ones
 *     - Triggers state change callbacks for Markov chain updates
 *     - Increments running time counters for probability calculation
 *
 * WHY TWO PHASES?
 *   Splitting scan and model-update allows the daemon to learn about
 *   short-lived processes. A process must survive half a cycle to be
 *   examined, filtering out transient scripts and compilation steps.
 *
 * STATE TRACKING:
 *   For each executable (kp_exe_t), we track:
 *   - running_timestamp: When it was last seen running
 *   - time: Total time spent running (for frequency weighting)
 *   - change_timestamp: Last state transition (running ↔ not running)
 *
 * =============================================================================
 */

#include "common.h"
#include "spy.h"
#include "../config/config.h"
#include "../state/state.h"
#include "../daemon/stats.h"
#include "../utils/desktop.h"
#include "proc.h"
#include <math.h>

/*
 * Module-level state for tracking changes between phases.
 * These lists accumulate data in kp_spy_scan() then are processed
 * in kp_spy_update_model().
 */
static GSList *state_changed_exes;  /* Exes that started or stopped running */
static GSList *new_running_exes;    /* Currently running exe list (rebuilt each scan) */
static GHashTable *new_exes;        /* Newly discovered exe paths → PIDs */

/*
 * =============================================================================
 * WEIGHTED LAUNCH COUNTING
 * =============================================================================
 */

/**
 * Get parent PID of a process
 *
 * @param pid  Process ID
 * @return Parent PID, or 0 if couldn't be determined
 *
 * Reads field 4 from /proc/{pid}/stat which contains the parent PID.
 */ 
pid_t
get_parent_pid(pid_t pid)
{
    char stat_path[64];
    FILE *fp;
    pid_t ppid = 0;
    
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    fp = fopen(stat_path, "r");
    if (!fp)
        return 0;
    
    /* Format: pid (comm) state ppid ...
     * SECURITY FIX (B013/B016): comm is in parentheses and can contain
     * spaces, newlines, or any character except ')'. Use %*s to skip
     * to the last ')' then parse remaining fields.
     */
    char line[1024];
    if (fgets(line, sizeof(line), fp)) {
        /* Find last ')' - comm field ends there */
        char *close_paren = strrchr(line, ')');
        if (close_paren && close_paren[1] == ' ') {
            /* Parse: " state ppid ..." after the closing paren */
            char state;
            if (sscanf(close_paren + 2, "%c %d", &state, &ppid) != 2)
                ppid = 0;
        }
    }
    
    fclose(fp);
    return ppid;
}

/**
 * Detect if process was initiated by user (not automated/script)
 *
 * @param parent_pid  Parent process ID
 * @return TRUE if user-initiated (shell, terminal, desktop launcher)
 *
 * HEURISTICS:
 *   1. Parent is shell (bash, zsh, fish, sh)
 *   2. Parent is terminal emulator (gnome-terminal, konsole, xterm, alacritty)
 *   3. Parent is desktop launcher (gnome-shell, plasmashell, xfce4-panel)
 *
 * Graceful fallback: If parent cannot be determined, assume automated.
 */
static gboolean
is_user_initiated(pid_t parent_pid)
{
    char parent_exe_path[PATH_MAX];
    char *parent_basename;
    gboolean result = FALSE;

    /* Read /proc/{parent_pid}/exe */
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", parent_pid);
    
    ssize_t len = readlink(proc_path, parent_exe_path, sizeof(parent_exe_path) - 1);
    if (len < 0) {
        /* Parent process may have exited, assume automated */
        return FALSE;
    }
    parent_exe_path[len] = '\0';

    parent_basename = g_path_get_basename(parent_exe_path);
    
    /* Check for shells */
    if (g_str_has_prefix(parent_basename, "bash") ||
        g_str_has_prefix(parent_basename, "zsh") ||
        g_str_has_prefix(parent_basename, "fish") ||
        strcmp(parent_basename, "sh") == 0) {
        result = TRUE;
        goto cleanup;
    }
    
    /* Check for terminal emulators */
    if (strstr(parent_exe_path, "gnome-terminal") ||
        strstr(parent_exe_path, "konsole") ||
        strstr(parent_exe_path, "xterm") ||
        strstr(parent_exe_path, "alacritty") ||
        strstr(parent_exe_path, "qterminal") ||
        strstr(parent_exe_path, "terminator")) {
        result = TRUE;
        goto cleanup;
    }
    
    /* Check common automated task runners (non-user-initiated) */
    if (strstr(parent_exe_path, "cron") != NULL ||
        strstr(parent_exe_path, "systemd") != NULL ||
        strstr(parent_exe_path, "anacron") != NULL) {
        /* Automated task - not user-initiated */
        result = FALSE;
        goto cleanup;
    }

    /* Check for desktop launchers */
    if (strstr(parent_exe_path, "gnome-shell") ||
        strstr(parent_exe_path, "plasmashell") ||
        strstr(parent_exe_path, "xfce4-panel") ||
        strstr(parent_exe_path, "mate-panel")) {
        result = TRUE;
        goto cleanup;
    }

cleanup:
    g_free(parent_basename);
    return result;
}

/**
 * Calculate launch weight based on duration and user-initiated status
 *
 * @param duration_sec   Process runtime in seconds
 * @param user_initiated TRUE if user-initiated
 * @return Weight value (logarithmic duration × user multiplier)
 *
 * FORMULA: weight = 1.0 × log(1 + duration/divisor) × user_multiplier
 *   where divisor and user_multiplier are configurable
 *
 * CONFIG:
 *   preheat.weight_duration_divisor (default 60)
 *   preheat.weight_user_multiplier_x100 (default 200 = 2.0x)
 *
 * EXAMPLES (with defaults):
 *   - grep 0.1s (automated):  1.0 × log(1.002) × 1.0 ≈ 0.002
 *   - updatedb 10m (cron):    1.0 × log(11) × 1.0 ≈ 2.4
 *   - firefox 2h (user):      1.0 × log(121) × 2.0 ≈ 9.8
 */
static double
calculate_launch_weight(time_t duration_sec, gboolean user_initiated)
{
    double base = 1.0;
    
    /* Get config values (fallback to defaults if not available) */
    int divisor = 60;  /* Default */
    double user_mult = 2.0;  /* Default */
    
#ifdef ENABLE_PREHEAT_EXTENSIONS
    if (kp_conf) {
        divisor = kp_conf->preheat.weight_duration_divisor;
        user_mult = kp_conf->preheat.weight_user_multiplier_x100 / 100.0;
    }
#endif
    
    double duration_multiplier = log(1.0 + (double)duration_sec / (double)divisor);
    double user_multiplier = user_initiated ? user_mult : 1.0;
    
    /* Short-lived penalty: processes that exit quickly likely crashed or are transient */
    double short_lived_penalty = 1.0;
    if (duration_sec < 5) {
        short_lived_penalty = 0.3;  /* Heavily penalize very short runs */
    }
    
    return base * duration_multiplier * user_multiplier * short_lived_penalty;
}

/**
 * Track process start for weighted counting
 *
 * @param exe         Executable structure
 * @param pid         Process ID
 * @param parent_pid  Parent process ID
 */
static void
track_process_start(kp_exe_t *exe, pid_t pid, pid_t parent_pid)
{
    process_info_t *proc_info;
    time_t now = time(NULL);
    
    g_return_if_fail(exe);
    g_return_if_fail(exe->running_pids);
    
    /* Check if already tracking this PID (shouldn't happen, but be safe) */
    if (g_hash_table_lookup(exe->running_pids, GINT_TO_POINTER(pid)))
        return;
    
    proc_info = g_new0(process_info_t, 1);
    proc_info->pid = pid;
    proc_info->parent_pid = parent_pid;
    proc_info->start_time = now;
    proc_info->last_weight_update = now;
    proc_info->user_initiated = is_user_initiated(parent_pid);
    
    /* FALLBACK for snap/flatpak/container apps:
     * Only triggers when is_user_initiated() returned FALSE.
     * If exe has a .desktop file, it's a real GUI app launched by user.
     * This handles snap-confine, bwrap, and other container parent processes
     * that aren't in our shell/terminal/desktop whitelist.
     * 
     * For apt-installed apps, is_user_initiated() already returns TRUE
     * when launched from terminal/desktop, so this fallback never runs.
     */
    if (!proc_info->user_initiated && kp_desktop_has_file(exe->path)) {
        proc_info->user_initiated = TRUE;
        g_debug("Desktop app fallback: %s (pid %d, parent was container)",
                exe->path, pid);
    }
    
    /* Only increment raw launch count for user-initiated processes.
     * This avoids counting child processes (e.g., Firefox content processes)
     * as separate launches. Child processes inherit the parent's session and
     * should not inflate the launch count. */
    if (proc_info->user_initiated) {
        exe->raw_launches++;
        g_debug("Launch detected: %s (pid %d, user-initiated)",
                exe->path, pid);
        
        /* Record hit or miss for stats tracking */
        if (kp_stats_is_app_preloaded(exe->path)) {
            kp_stats_record_hit(exe->path);
        } else {
            kp_stats_record_miss(exe->path);
        }
    } else {
        g_debug("Child process detected: %s (pid %d, parent %d)",
                exe->path, pid, parent_pid);
    }
    
    g_hash_table_insert(exe->running_pids, GINT_TO_POINTER(pid), proc_info);
}

/**
 * Track process exit and update weighted counters
 *
 * @param exe  Executable structure
 * @param pid  Process ID that exited
 *
 * NOTE: This is called from clean_exited_pids_callback() which is used with
 * g_hash_table_foreach_remove(). The hash table entry will be automatically
 * removed when the callback returns TRUE, so we must NOT call g_hash_table_remove()
 * here to avoid double-removal.
 */
static void
track_process_exit(kp_exe_t *exe, pid_t pid)
{
    process_info_t *proc_info;
    time_t now, duration;
    
    g_return_if_fail(exe);
    g_return_if_fail(exe->running_pids);
    
    proc_info = g_hash_table_lookup(exe->running_pids, GINT_TO_POINTER(pid));
    if (!proc_info)
        return;  /* Not tracked, this is fine */
    
    /* Weight already accumulated incrementally via update_running_weights() */
    /* Just update duration tracking */
    now = time(NULL);
    duration = now - proc_info->start_time;
    if (duration < 0)
        duration = 0;  /* Clock skew protection */
    
    exe->total_duration_sec += (unsigned long)duration;
    
    /* NOTE: Do NOT remove from hash table here - it's done automatically by
     * g_hash_table_foreach_remove() when clean_exited_pids_callback returns TRUE */
}

/**
 * Check if a PID is still running
 *
 * @param pid  Process ID to check
 * @return TRUE if process exists in /proc
 */
static gboolean
is_pid_alive(pid_t pid)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    return g_file_test(proc_path, G_FILE_TEST_EXISTS);
}

/**
 * Clean up exited PIDs from running_pids hash table
 *
 * @param key        PID as GINT_TO_POINTER
 * @param value      process_info_t*
 * @param user_data  kp_exe_t* to track exits
 * @return TRUE if PID should be removed (has exited)
 */
static gboolean
clean_exited_pids_callback(gpointer key, gpointer value, gpointer user_data)
{
    pid_t pid = GPOINTER_TO_INT(key);
    kp_exe_t *exe = (kp_exe_t *)user_data;
    (void)value;  /* Unused parameter */
    
    if (!is_pid_alive(pid)) {
        /* Process exited, track it */
        track_process_exit(exe, pid);
        return TRUE;  /* Remove from hash table */
    }
    
    return FALSE;  /* Keep in hash table */
}

/**
 * Clean exited processes from exe's running_pids table
 *
 * @param exe  Executable to clean
 */
static void
clean_exited_pids(kp_exe_t *exe)
{
    g_return_if_fail(exe);
    g_return_if_fail(exe->running_pids);
    
    g_hash_table_foreach_remove(exe->running_pids, clean_exited_pids_callback, exe);
}

/**
 * Update weighted_launches for all currently running processes
 * Called each scan cycle to provide incremental weight accumulation.
 */
static void
update_weight_for_pid(gpointer key, gpointer value, gpointer user_data)
{
    pid_t pid = GPOINTER_TO_INT(key);
    process_info_t *proc_info = (process_info_t *)value;
    kp_exe_t *exe = (kp_exe_t *)user_data;
    time_t now = time(NULL);
    time_t elapsed;
    double incremental_weight;
    
    (void)pid;  /* Unused */
    
    elapsed = now - proc_info->last_weight_update;
    if (elapsed <= 0)
        return;  /* No time passed, skip */
    
    /* Calculate weight for this interval */
    incremental_weight = calculate_launch_weight(elapsed, proc_info->user_initiated);
    
    /* Accumulate */
    exe->weighted_launches += incremental_weight;
    proc_info->last_weight_update = now;
}

static void
update_running_weights(kp_exe_t *exe)
{
    g_return_if_fail(exe);
    g_return_if_fail(exe->running_pids);
    
    g_hash_table_foreach(exe->running_pids, update_weight_for_pid, exe);
}


/**
 * Callback for every running process
 * Check whether we know what it is, and add it to appropriate list
 * (Modified with weighted launch tracking)
 */
static void
running_process_callback(pid_t pid, const char *path)
{
    kp_exe_t *exe;

    g_return_if_fail(path);

    exe = g_hash_table_lookup(kp_state->exes, path);
    if (exe) {
        /* Already existing exe */

        /* Has it been running already? */
        if (!exe_is_running(exe)) {
            new_running_exes = g_slist_prepend(new_running_exes, exe);
            state_changed_exes = g_slist_prepend(state_changed_exes, exe);
        }

        /* Update timestamp */
        exe->running_timestamp = kp_state->time;
        
        /* Track process start for weighted counting */
        if (!g_hash_table_lookup(exe->running_pids, GINT_TO_POINTER(pid))) {
            pid_t parent_pid = get_parent_pid(pid);
            track_process_start(exe, pid, parent_pid);
        }

    } else if (!g_hash_table_lookup(kp_state->bad_exes, path)) {
        /* An exe we have never seen before, just queue it */
        g_hash_table_insert(new_exes, g_strdup(path), GUINT_TO_POINTER(pid));
    }
}

/**
 * For every exe that has been running, check whether it's still running
 * (VERBATIM from upstream already_running_exe_callback)
 */
static void
already_running_exe_callback(kp_exe_t *exe)
{
    if (exe_is_running(exe))
        new_running_exes = g_slist_prepend(new_running_exes, exe);
    else
        state_changed_exes = g_slist_prepend(state_changed_exes, exe);
}

/**
 * There is an exe we've never seen before. Check if it's a piggy one or not.
 * If yes, add it to our farm, add it to the blacklist otherwise.
 * (VERBATIM from upstream new_exe_callback)
 */
static void
new_exe_callback(char *path, pid_t pid)
{
    gboolean want_it;
    size_t size;

    size = kp_proc_get_maps(pid, NULL, NULL);

    if (!size) /* process died or something */
        return;

    want_it = size >= (size_t)kp_conf->model.minsize;

    if (want_it) {
        kp_exe_t *exe;
        GSet *exemaps;

        size = kp_proc_get_maps(pid, kp_state->maps, &exemaps);
        if (!size) {
            /* Process just died, clean up */
            g_set_foreach(exemaps, (GFunc)(void (*)(void))kp_exemap_free, NULL);
            g_set_free(exemaps);
            return;
        }

        exe = kp_exe_new(path, TRUE, exemaps);
        kp_state_register_exe(exe, TRUE);
        kp_state->running_exes = g_slist_prepend(kp_state->running_exes, exe);

    } else {
        g_hash_table_insert(kp_state->bad_exes, g_strdup(path), GINT_TO_POINTER(size));
    }
}

/**
 * Increment time for running markov (state 3 = both exes running)
 * (VERBATIM from upstream running_markov_inc_time)
 */
static void
running_markov_inc_time(kp_markov_t *markov, int time)
{
    if (markov->state == 3)
        markov->time += time;
}

/**
 * Increment time for running exe
 * (VERBATIM from upstream running_exe_inc_time)
 */
static void
running_exe_inc_time(gpointer G_GNUC_UNUSED key, kp_exe_t *exe, int time)
{
    if (exe_is_running(exe))
        exe->time += time;
}

/**
 * Adjust states on exes that change state (running/not-running)
 * (VERBATIM from upstream exe_changed_callback)
 */
static void
exe_changed_callback(kp_exe_t *exe)
{
    exe->change_timestamp = kp_state->time;
    g_set_foreach(exe->markovs, (GFunc)(void (*)(void))kp_markov_state_changed, NULL);
}

/**
 * Scan processes, see which exes started running, which are not running
 * anymore, and what new exes are around.
 * (VERBATIM from upstream preload_spy_scan)
 */
/* Wrapper with correct GHFunc signature for running_process_callback */
static void
running_process_callback_wrapper(gpointer key, gpointer value, gpointer user_data)
{
    (void)user_data;
    running_process_callback(GPOINTER_TO_INT(key), (const char *)value);
}

/* Wrapper with correct GFunc signature for already_running_exe_callback */
static void
already_running_exe_callback_wrapper(gpointer data, gpointer user_data)
{
    (void)user_data;
    already_running_exe_callback((kp_exe_t *)data);
}

void
kp_spy_scan(gpointer data)
{
    /* Scan processes */
    state_changed_exes = new_running_exes = NULL;
    new_exes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Mark each running exe with fresh timestamp */
    kp_proc_foreach(running_process_callback_wrapper, data);
    kp_state->last_running_timestamp = kp_state->time;

    /* Figure out who's not running by checking their timestamp */
    g_slist_foreach(kp_state->running_exes, already_running_exe_callback_wrapper, data);

    /* Update weights for running processes, then clean up exited ones */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, kp_state->exes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        kp_exe_t *exe = (kp_exe_t *)value;
        update_running_weights(exe);  /* Incremental weight update */
        clean_exited_pids(exe);
    }

    g_slist_free(kp_state->running_exes);
    kp_state->running_exes = new_running_exes;
}

/* Wrapper with correct GHFunc signature for new_exe_callback */
static void
new_exe_callback_wrapper(gpointer key, gpointer value, gpointer user_data)
{
    (void)user_data;
    new_exe_callback((char *)key, GPOINTER_TO_INT(value));
}

/* Wrapper with correct GFunc signature for exe_changed_callback */
static void
exe_changed_callback_wrapper(gpointer data, gpointer user_data)
{
    (void)user_data;
    exe_changed_callback((kp_exe_t *)data);
}

/* Wrapper with correct GHFunc signature for running_exe_inc_time */
static void
running_exe_inc_time_wrapper(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    running_exe_inc_time(NULL, (kp_exe_t *)value, GPOINTER_TO_INT(user_data));
}

/* Wrapper with correct GFunc signature for running_markov_inc_time */
static void
running_markov_inc_time_wrapper(gpointer data, gpointer user_data)
{
    running_markov_inc_time((kp_markov_t *)data, GPOINTER_TO_INT(user_data));
}

/**
 * Update model - run after scan, after some delay (half a cycle)
 * (VERBATIM from upstream preload_spy_update_model)
 */
void
kp_spy_update_model(gpointer data)
{
    int period;

    /* Register newly discovered exes */
    g_hash_table_foreach(new_exes, new_exe_callback_wrapper, data);
    g_hash_table_destroy(new_exes);

    /* And adjust states for those changing */
    g_slist_foreach(state_changed_exes, exe_changed_callback_wrapper, data);
    g_slist_free(state_changed_exes);

    /* Do some accounting */
    period = kp_state->time - kp_state->last_accounting_timestamp;
    g_hash_table_foreach(kp_state->exes, running_exe_inc_time_wrapper, GINT_TO_POINTER(period));
    kp_markov_foreach(running_markov_inc_time_wrapper, GINT_TO_POINTER(period));
    kp_state->last_accounting_timestamp = kp_state->time;
}

/**
 * Get foreground time ratio for a process (STUB)
 * 
 * TODO: Integrate with window manager to track actual foreground time.
 * 
 * FUTURE WM INTEGRATION:
 *   - X11: Monitor _NET_ACTIVE_WINDOW property changes
 *   - Wayland: Use compositor-specific protocols
 *   - Match window PID to our tracked processes
 *   - Maintain per-PID foreground time counter
 * 
 * CURRENT BEHAVIOR:
 *   - Returns 0.5 (50%) for GUI apps (heuristic)
 *   - Returns 0.0 for non-GUI apps
 *   - Allows basic weighting until WM integration ready
 * 
 * @param proc_info Process information structure
 * @return Foreground time ratio (0.0 to 1.0)
 */
static double __attribute__((unused))
get_foreground_time_ratio(process_info_t *proc_info)
{
    (void)proc_info;  /* Unused until WM integration */
    
    /* STUB: Assume GUI apps spend 50% time in foreground
     * This is a reasonable heuristic for typical desktop usage.
     * Real implementation would track via WM events. */
    
    /* TODO: Detect if process has X11/Wayland windows
     * For now, return neutral value that doesn't bias results */
    
    return 0.0;  /* Disabled until WM integration complete */
}

/**
 * Handle edge case: process still running when calculating weight
 * Returns true if we should defer weight calculation
 */
static gboolean __attribute__((unused))
is_process_still_running(pid_t pid)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    return (access(proc_path, F_OK) == 0);
}

/* Note: Integrate into clean_exited_pids_callback for proper handling */
