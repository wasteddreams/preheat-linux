/* state_exe.c - Executable management for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * =============================================================================
 * MODULE: Executable Management
 * =============================================================================
 *
 * Executables (kp_exe_t) represent tracked applications:
 *
 *   exe.path     = "/usr/bin/firefox"
 *   exe.time     = total seconds ever running (for frequency weighting)
 *   exe.exemaps  = set of memory maps this exe uses
 *   exe.markovs  = set of correlations with other exes
 *
 * EXE LIFECYCLE:
 *   1. Discovered via /proc scan → kp_exe_new()
 *   2. Registered in global state → kp_state_register_exe()
 *   3. Markov chains created to all existing exes
 *   4. Time/prob updated each scan cycle
 *   5. Serialized to state file on save
 *
 * =============================================================================
 */

#include "common.h"
#include "state.h"
#include "state_exe.h"

/**
 * Add map size to exe's total size
 * Helper callback for calculating total memory footprint of an executable.
 */
static void
exe_add_map_size(kp_exemap_t *exemap, kp_exe_t *exe)
{
    exe->size += kp_map_get_size(exemap->map);
}

/* Wrapper with correct GFunc signature for exe_add_map_size */
static void
exe_add_map_size_wrapper(gpointer data, gpointer user_data)
{
    exe_add_map_size((kp_exemap_t *)data, (kp_exe_t *)user_data);
}

/**
 * Create new executable object
 *
 * @param path     Absolute path to the executable
 * @param running  TRUE if executable is currently running
 * @param exemaps  Pre-populated set of exemaps, or NULL to create empty
 * @return         New exe object (not yet registered in global state)
 */
kp_exe_t *
kp_exe_new(const char *path, gboolean running, GSet *exemaps)
{
    kp_exe_t *exe;

    g_return_val_if_fail(path, NULL);

    exe = g_slice_new(kp_exe_t);
    exe->path = g_strdup(path);
    exe->size = 0;
    exe->time = 0;
    exe->change_timestamp = kp_state->time;

    /* Initialize weighted launch counting fields */
    exe->weighted_launches = 0.0;
    exe->raw_launches = 0;
    exe->total_duration_sec = 0;
    exe->running_pids = g_hash_table_new_full(
        g_direct_hash, g_direct_equal,
        NULL,                /* pid is stored as GINT_TO_POINTER, no need to free */
        g_free               /* process_info_t* allocated with g_new, free with g_free */
    );

    if (running) {
        exe->update_time = exe->running_timestamp = kp_state->last_running_timestamp;
    } else {
        exe->update_time = exe->running_timestamp = -1;
    }

    if (!exemaps)
        exe->exemaps = g_set_new();
    else
        exe->exemaps = exemaps;

    g_set_foreach(exe->exemaps, exe_add_map_size_wrapper, exe);
    exe->markovs = g_set_new();
    return exe;
}

/* Wrapper with correct GFunc signature for kp_exemap_free */
static void
kp_exemap_free_wrapper(gpointer data, gpointer user_data)
{
    (void)user_data;
    kp_exemap_free((kp_exemap_t *)data);
}

/* Wrapper with correct GFunc signature for kp_markov_free */
static void
kp_markov_free_from_exe_wrapper(gpointer data, gpointer user_data)
{
    kp_markov_free((kp_markov_t *)data, (kp_exe_t *)user_data);
}

/**
 * Free exe
 * (VERBATIM from upstream preload_exe_free, with running_pids cleanup)
 */
void
kp_exe_free(kp_exe_t *exe)
{
    g_return_if_fail(exe);
    g_return_if_fail(exe->path);

    g_set_foreach(exe->exemaps, kp_exemap_free_wrapper, NULL);
    g_set_free(exe->exemaps);
    exe->exemaps = NULL;

    g_set_foreach(exe->markovs, kp_markov_free_from_exe_wrapper, exe);
    g_set_free(exe->markovs);
    exe->markovs = NULL;

    /* Free running PIDs hash table */
    if (exe->running_pids) {
        g_hash_table_destroy(exe->running_pids);
        exe->running_pids = NULL;
    }

    g_free(exe->path);
    exe->path = NULL;
    g_slice_free(kp_exe_t, exe);
}

/**
 * Create exemap and add to exe
 * (VERBATIM from upstream preload_exe_map_new)
 */
kp_exemap_t *
kp_exe_map_new(kp_exe_t *exe, kp_map_t *map)
{
    kp_exemap_t *exemap;

    g_return_val_if_fail(exe, NULL);
    g_return_val_if_fail(map, NULL);

    exemap = kp_exemap_new(map);
    g_set_add(exe->exemaps, exemap);
    exe_add_map_size(exemap, exe);
    return exemap;
}

/**
 * Helper for creating markov with existing exe
 * (VERBATIM from upstream shift_preload_markov_new)
 */
static void
shift_kp_markov_new(gpointer G_GNUC_UNUSED key, kp_exe_t *a, kp_exe_t *b)
{
    if (a != b)
        kp_markov_new(a, b, TRUE);
}

/* Wrapper with correct GHFunc signature for shift_kp_markov_new */
static void
shift_kp_markov_new_wrapper(gpointer key, gpointer value, gpointer user_data)
{
    shift_kp_markov_new(key, (kp_exe_t *)value, (kp_exe_t *)user_data);
}

/**
 * Register exe in state
 * (Modified from upstream preload_state_register_exe)
 * 
 * B012 FIX: Limit Markov chain creation to prevent O(n²) memory growth.
 * With N executables, creating chains to all others requires N*(N-1)/2 chains.
 * We limit chains to when total exes < MAX_MARKOV_EXES.
 */
#define MAX_MARKOV_EXES 100  /* Only create full Markov mesh below this count */

void
kp_state_register_exe(kp_exe_t *exe, gboolean create_markovs)
{
    g_return_if_fail(!g_hash_table_lookup(kp_state->exes, exe));

    exe->seq = ++(kp_state->exe_seq);
    
    /* B012 REVISED: Only create Markov chains for PRIORITY pool apps.
     * Observation pool apps (grep, find, etc.) don't need prediction.
     * This limits memory to ~(priority_count)² chains instead of O(n²). */
    if (create_markovs && exe->pool == POOL_PRIORITY) {
        g_hash_table_foreach(kp_state->exes, shift_kp_markov_new_wrapper, exe);
    }
    g_hash_table_insert(kp_state->exes, exe->path, exe);
}

/**
 * Unregister exe from state
 * (VERBATIM from upstream preload_state_unregister_exe)
 */
void
kp_state_unregister_exe(kp_exe_t *exe)
{
    g_return_if_fail(g_hash_table_lookup(kp_state->exes, exe));

    g_set_foreach(exe->markovs, kp_markov_free_from_exe_wrapper, exe);
    g_set_free(exe->markovs);
    exe->markovs = NULL;
    g_hash_table_remove(kp_state->exes, exe);
}
