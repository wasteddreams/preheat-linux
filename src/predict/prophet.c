/* prophet.c - Prediction engine for Preheat
 *
 * Based on preload 0.6.4 prophet.c (VERBATIM algorithms)
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: Prediction Engine ("Prophet")
 * =============================================================================
 *
 * This module decides WHAT to preload by computing probability scores for
 * each memory map based on Markov chain correlations and current system state.
 *
 * PREDICTION PIPELINE (kp_prophet_predict):
 *
 *   1. RESET: Zero all exe and map probabilities
 *
 *   2. BOOST MANUAL APPS: Apps in /etc/preheat.d/apps.list get priority
 *
 *   3. MARKOV → EXE: Each Markov chain bids on its exes
 *      ┌─────────────────────────────────────────────────────────────┐
 *      │ For each markov(A,B) where A or B is not running:           │
 *      │   P(exe runs) = correlation × P(state change) × P(next=run) │
 *      │   exe.lnprob += log(1 - P(exe runs))                        │
 *      └─────────────────────────────────────────────────────────────┘
 *
 *   4. EXE → MAP: Each exe bids on its maps
 *      ┌─────────────────────────────────────────────────────────────┐
 *      │ For each exemap linking exe → map:                          │
 *      │   If exe running: map.lnprob += 1 (already in memory)       │
 *      │   Else:           map.lnprob += exe.lnprob                  │
 *      └─────────────────────────────────────────────────────────────┘
 *
 *   5. SORT: Maps sorted by lnprob (most negative = most needed)
 *
 *   6. READAHEAD: Preload maps until memory budget exhausted
 *
 * PROBABILITY MATH:
 *   We compute log-probability of NOT needing each item:
 *     lnprob(X=0) = Σ log(P(X=0|Markov_i))
 *
 *   Negative lnprob → likely to be needed → should preload
 *   Positive lnprob → unlikely to be needed → skip
 *
 * MEMORY BUDGET (kp_prophet_readahead):
 *   Available = (memtotal% × total) + (memfree% × free) + (memcached% × cached)
 *   Preload maps in order until budget exhausted or lnprob becomes positive.
 *
 * =============================================================================
 */

#include "common.h"
#include "prophet.h"
#include "../utils/logging.h"
#include "../config/config.h"
#include "../config/blacklist.h"
#include "../state/state.h"
#include "../monitor/proc.h"
#include "../readahead/readahead.h"
#include "../daemon/stats.h"

#include <math.h>

/*
 * Priority boost for manually-configured apps.
 * Strong negative value = high probability of being needed = preload first.
 * 
 * -10.0: Empirically chosen to override all Markov predictions.
 * log(P=0) ranges from 0 (certain) to -∞ (impossible).
 * -10 ≈ P(not needed) = e^-10 ≈ 0.00005 = 99.995% likely to preload.
 */
#define MANUAL_APP_BOOST_LNPROB -10.0

/* CRITICAL ALGORITHM: Markov-based probability inference
 * (VERBATIM from upstream lines 33-49)
 *
 * Computes P(Y runs in next period | current state) and bids in for the Y.
 * Y should not be running.
 *
 * Y=1 if it's needed in next period, 0 otherwise.
 * Probability inference follows:
 *
 *   P(Y=1) = 1 - P(Y=0)
 *   P(Y=0) = Π P(Y=0|Xi)
 *   P(Y=0|Xi) = 1 - P(Y=1|Xi)
 *   P(Y=1|Xi) = P(state change of Y,X) * P(next state has Y=1) * corr(Y,X)
 *   corr(Y=X) = regularized |correlation(Y,X)|
 *
 * So:
 *
 *   lnprob(Y) = log(P(Y=0)) = Σ log(P(Y=0|Xi)) = Σ log(1 - P(Y=1|Xi))
 */
static void
markov_bid_for_exe(kp_markov_t *markov,
                   kp_exe_t *y,
                   int ystate,
                   double correlation)
{
    int state;
    double p_state_change;
    double p_y_runs_next;
    double p_runs;

    state = markov->state;

    if (!markov->weight[state][state] || !(markov->time_to_leave[state] > 1))
        return;

    /* p_state_change is the probability of the state of markov changing
     * in the next period. Period is taken as 1.5 cycles. It's computed as:
     *                                            -λ.period
     *   p(state changes in time < period) = 1 - e
     *
     * where λ is one over average time to leave the state.
     * (VERBATIM from upstream lines 66-75)
     * 
     * 1.5 multiplier: Empirically tuned from upstream preload.
     * Provides lookahead beyond current cycle to catch transitions.
     */
    p_state_change = -kp_conf->model.cycle * 1.5 / markov->time_to_leave[state];
    p_state_change = 1 - exp(p_state_change);

    /* p_y_runs_next estimates the probability that Y runs, given that a state
     * change occurs. This is a HEURISTIC, not a strict transition probability.
     *
     * Numerator: weight[state][ystate] = times we transitioned to Y-running state
     *            + weight[state][3] = times we transitioned to both-running state
     * Denominator: weight[state][state] = total time spent in current state
     *              + 0.01 for regularization (avoid division by zero)
     *
     * This ratio approximates "how often does Y start when we're in this state?"
     * (VERBATIM from upstream lines 77-83)
     */
    p_y_runs_next = markov->weight[state][ystate] + markov->weight[state][3];
    p_y_runs_next /= markov->weight[state][state] + 0.01;

    /* NOTE: Correlation handling w.r.t. state is simplified here.
     * This is an intentional algorithmic limitation from upstream preload.
     * More sophisticated state-dependent correlation would require additional
     * theoretical work. Current approach works well in practice. */
    correlation = fabs(correlation);

    p_runs = correlation * p_state_change * p_y_runs_next;

    y->lnprob += log(1 - p_runs);
}

/**
 * Bid in exes based on markov states
 * (VERBATIM from upstream markov_bid_in_exes)
 */
static void
markov_bid_in_exes(kp_markov_t *markov)
{
    double correlation;

    if (!markov->weight[markov->state][markov->state])
        return;

    correlation = kp_conf->model.usecorrelation ? kp_markov_correlation(markov) : 1.0;

    if ((markov->state & 1) == 0) /* a not running */
        markov_bid_for_exe(markov, markov->a, 1, correlation);
    if ((markov->state & 2) == 0) /* b not running */
        markov_bid_for_exe(markov, markov->b, 2, correlation);
}

/**
 * Zero map probability
 * (VERBATIM from upstream map_zero_prob)
 */
static void
map_zero_prob(kp_map_t *map)
{
    map->lnprob = 0;
}

/**
 * Compare maps by probability (for sorting)
 * (VERBATIM from upstream map_prob_compare)
 */
static int
map_prob_compare(const kp_map_t **pa, const kp_map_t **pb)
{
    const kp_map_t *a = *pa, *b = *pb;
    return a->lnprob < b->lnprob ? -1 : a->lnprob > b->lnprob ? 1 : 0;
}

/**
 * Zero exe probability
 * (VERBATIM from upstream exe_zero_prob, extended with blacklist check)
 */
static void
exe_zero_prob(gpointer G_GNUC_UNUSED key, kp_exe_t *exe)
{
    /* Skip blacklisted apps - they get no probability boost */
    if (kp_blacklist_contains(exe->path)) {
        exe->lnprob = 1;  /* Positive = low priority, won't be preloaded */
        return;
    }
    exe->lnprob = 0;
}

/* CRITICAL ALGORITHM: Map probability inference
 * (VERBATIM from upstream lines 133-159)
 *
 * Computes P(M needed in next period | current state) and bids in for the M,
 * where M is the map used by exemap.
 *
 * M=1 if it's needed in next period, 0 otherwise.
 * Probability inference follows:
 *
 *   P(M=1) = 1 - P(M=0)
 *   P(M=0) = Π P(M=0|Xi)
 *   P(M=0|Xi) = P(Xi=0)
 *
 * So:
 *
 *   lnprob(M) = log(P(M=0)) = Σ log(P(M=0|Xi)) = Σ log(P(Xi=0)) = Σ lnprob(Xi)
 */
static void
exemap_bid_in_maps(kp_exemap_t *exemap, kp_exe_t *exe)
{
    if (exe_is_running(exe)) {
        /* SPECIAL CASE: If exe is running, we vote AGAINST preloading the map.
         * Reason: The map is almost certainly already in memory (loaded by the
         * running exe), so preloading it would be wasted I/O.
         *
         * We add +1 (positive) to lnprob, which pushes the map toward
         * "unlikely to need preloading" territory. This is a HEURISTIC
         * departure from the strict formula lnprob(M) = Σ lnprob(Xi).
         *
         * Alternative considered: Using exemap->prob instead of 1, but this
         * would require additional theoretical work on probability combination.
         * The simple +1 approach works well in practice.
         */
        exemap->map->lnprob += 1;
    } else {
        /* Normal case: Accumulate exe's lnprob into map's lnprob.
         * This implements: lnprob(M) = Σ lnprob(Xi) for non-running exes. */
        exemap->map->lnprob += exe->lnprob;
    }
}

/* Wrapper with correct GHFunc signature for exe_zero_prob */
static void
exe_zero_prob_wrapper(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)user_data;
    exe_zero_prob(NULL, (kp_exe_t *)value);
}

/* Wrapper with correct GFunc signature for map_zero_prob */
static void
map_zero_prob_wrapper(gpointer data, gpointer user_data)
{
    (void)user_data;
    map_zero_prob((kp_map_t *)data);
}

/* Wrapper with correct GFunc signature for markov_bid_in_exes */
static void
markov_bid_in_exes_wrapper(gpointer data, gpointer user_data)
{
    (void)user_data;
    markov_bid_in_exes((kp_markov_t *)data);
}

/* Wrapper with correct GHFunc signature for exemap_bid_in_maps */
static void
exemap_bid_in_maps_wrapper(gpointer exemap, gpointer exe, gpointer user_data)
{
    (void)user_data;
    exemap_bid_in_maps((kp_exemap_t *)exemap, (kp_exe_t *)exe);
}

/**
 * Helper macros for memory calculations
 * (VERBATIM from upstream lines 179-181)
 */
#define clamp_percent(v) ((v)>100 ? 100 : (v) < -100 ? -100 : (v))
#define max(a,b) ((a)>(b) ? (a) : (b))
#define kb(v) ((int)(((v) + 1023) / 1024))

/**
 * Perform readahead based on memory budget
 * (VERBATIM from upstream preload_prophet_readahead)
 *
 * Input is the list of maps sorted on the need.
 * Decide a cutoff based on memory conditions and readahead.
 */
/**
 * Track which exes had maps preloaded for hit/miss statistics
 * Called after readahead to record preload times at the exe level.
 */
static void
record_preloaded_exes(kp_map_t **maps, int count)
{
    GHashTable *recorded = g_hash_table_new(g_str_hash, g_str_equal);
    
    for (int i = 0; i < count; i++) {
        const char *map_path = maps[i]->path;
        
        /* Find all exes that use this map */
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, kp_state->exes);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            kp_exe_t *exe = (kp_exe_t *)value;
            
            /* Check if this exe uses this map via exemaps (GSet = GPtrArray) */
            for (guint j = 0; j < exe->exemaps->len; j++) {
                kp_exemap_t *exemap = g_ptr_array_index(exe->exemaps, j);
                if (exemap && exemap->map && exemap->map->path && 
                    strcmp(exemap->map->path, map_path) == 0) {
                    /* Record this exe as preloaded (only once per exe) */
                    if (!g_hash_table_contains(recorded, exe->path)) {
                        kp_stats_record_preload(exe->path);
                        g_hash_table_insert(recorded, (gpointer)exe->path, 
                                          GINT_TO_POINTER(1));
                        g_debug("Recorded preload for exe: %s (via map %s)",
                                exe->path, map_path);
                    }
                    break;  /* Found match, no need to check more exemaps */
                }
            }
        }
    }
    
    g_hash_table_destroy(recorded);
}

void
kp_prophet_readahead(GPtrArray *maps_arr)
{
    int i;
    long memavail, memavailtotal; /* in kilobytes - use long for 32-bit safety */
    kp_memory_t memstat;
    kp_map_t *map;

    kp_proc_get_memstat(&memstat);

    /* Memory we are allowed to use for prefetching
     * (VERBATIM upstream formula lines 196-199)
     */
    memavail  = clamp_percent(kp_conf->model.memtotal)  * (memstat.total  / 100)
              + clamp_percent(kp_conf->model.memfree)   * (memstat.free   / 100);
    memavail  = max(0, memavail);
    memavail += clamp_percent(kp_conf->model.memcached) * (memstat.cached / 100);

    memavailtotal = memavail;

    memcpy(&(kp_state->memstat), &memstat, sizeof(memstat));
    kp_state->memstat_timestamp = kp_state->time;

    i = 0;
    while (i < (int)(maps_arr->len) &&
           (map = g_ptr_array_index(maps_arr, i)) &&
           map->lnprob < 0 && kb(map->length) <= memavail) {
        i++;

        memavail -= kb(map->length);

        /* Debug logging for individual maps (if log level high enough) */
        if (kp_is_debugging()) {
            g_debug("ln(prob(~MAP)) = %13.10lf %s", map->lnprob, map->path);
        }
    }

    g_debug("%ldkb available for preloading, using %ldkb of it",
            memavailtotal, memavailtotal - memavail);

    if (i) {
        /* Record preloaded exes BEFORE calling readahead for hit tracking */
        record_preloaded_exes((kp_map_t **)maps_arr->pdata, i);
        
        i = kp_readahead((kp_map_t **)maps_arr->pdata, i);
        g_debug("readahead %d files", i);
    } else {
        g_debug("nothing to readahead");
    }
}

/**
 * Load memory maps for an executable that has none (lazy loading)
 * 
 * Creates a single map covering the entire binary file. This is used for
 * manual apps that weren't discovered through process scanning.
 * 
 * @param exe Executable to load maps for
 * @return TRUE if successfully loaded, FALSE if file doesn't exist or is too small
 */
static gboolean
load_maps_for_exe(kp_exe_t *exe)
{
    struct stat st;
    kp_map_t *map;
    kp_exemap_t *exemap;
    
    g_return_val_if_fail(exe, FALSE);
    g_return_val_if_fail(exe->path, FALSE);
    
    /* Check if file exists and get size */
    if (stat(exe->path, &st) < 0) {
        g_warning("Cannot stat manual app: %s (%s)", exe->path, strerror(errno));
        return FALSE;
    }
    
    /* Check minimum size threshold */
    if ((size_t)st.st_size < (size_t)kp_conf->model.minsize) {
        g_debug("Manual app too small to preload: %s (%zu bytes < %d)",
                exe->path, (size_t)st.st_size, kp_conf->model.minsize);
        return FALSE;
    }
    
    /* Create single map for entire file */
    map = kp_map_new(exe->path, 0, st.st_size);
    if (!map) {
        g_warning("Failed to create map for manual app: %s", exe->path);
        return FALSE;
    }
    
    /* Create exemap and add to exe.
     * NOTE: kp_exe_map_new() takes ownership of map via kp_map_ref(),
     * but if it fails, we must free the map ourselves. */
    exemap = kp_exe_map_new(exe, map);
    if (!exemap) {
        g_warning("Failed to create exemap for manual app: %s", exe->path);
        kp_map_free(map);  /* FIX: Prevent memory leak on failure */
        return FALSE;
    }
    
    exemap->prob = 1.0;  /* Assume entire file is needed */
    
    g_debug("Loaded map for manual app: %s (%zu bytes)", exe->path, (size_t)st.st_size);
    
    return TRUE;
}

/**
 * Boost manual apps by giving them high probability
 * Manual apps get a strong negative lnprob (= high need probability)
 * 
 * LAZY MAP LOADING:
 * Manual apps are registered without memory maps (exemaps = NULL) because
 * they may not be running when first added. This function checks if a
 * manual app has no maps and loads them on-demand before boosting priority.
 */
static void
boost_manual_apps(void)
{
    char **app_path;
    int boosted = 0;
    int maps_loaded = 0;

    if (!kp_conf->system.manual_apps_loaded ||
        kp_conf->system.manual_apps_count == 0) {
        return;
    }

    for (app_path = kp_conf->system.manual_apps_loaded; *app_path; app_path++) {
        kp_exe_t *exe;

        /* Look up exe in state */
        exe = g_hash_table_lookup(kp_state->exes, *app_path);

        if (exe && !exe_is_running(exe)) {
            /* Load maps if not already loaded (lazy loading) */
            if (g_set_size(exe->exemaps) == 0) {
                if (load_maps_for_exe(exe)) {
                    maps_loaded++;
                }
            }
            
            /* Boost: set strong negative lnprob = high need */
            exe->lnprob = MANUAL_APP_BOOST_LNPROB;
            boosted++;
        }
    }

    if (boosted > 0) {
        if (maps_loaded > 0) {
            g_debug("Boosted %d manual apps (%d had maps loaded)", boosted, maps_loaded);
        } else {
            g_debug("Boosted %d manual apps for preloading", boosted);
        }
    }
}

/**
 * Main prediction function
 * (VERBATIM from upstream preload_prophet_predict)
 */
void
kp_prophet_predict(gpointer data)
{
    /* Reset probabilities that we are gonna compute */
    g_hash_table_foreach(kp_state->exes, exe_zero_prob_wrapper, data);
    g_ptr_array_foreach(kp_state->maps_arr, map_zero_prob_wrapper, data);

    /* Boost manual apps first (Preheat extension) */
    boost_manual_apps();

    /* Markovs bid in exes */
    kp_markov_foreach(markov_bid_in_exes_wrapper, data);

    /* Exes bid in maps */
    kp_exemap_foreach(exemap_bid_in_maps_wrapper, data);

    /* Sort maps on probability */
    g_ptr_array_sort(kp_state->maps_arr, (GCompareFunc)map_prob_compare);

    /* Read them in */
    kp_prophet_readahead(kp_state->maps_arr);
}
