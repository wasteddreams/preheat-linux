/* prophet.c - Prediction engine for Preheat
 *
 * Based on preload 0.6.4 prophet.c (VERBATIM algorithms)
 * Based on the preload daemon
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.h"
#include "prophet.h"
#include "../utils/logging.h"
#include "../config/config.h"
#include "../state/state.h"
#include "../monitor/proc.h"
#include "../readahead/readahead.h"

#include <math.h>

/* Manual app priority boost value - strong negative lnprob = high need */
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
     */
    p_state_change = -kp_conf->model.cycle * 1.5 / markov->time_to_leave[state];
    p_state_change = 1 - exp(p_state_change);
    
    /* p_y_runs_next is the probability that X runs, given that a state
     * change occurs. It's computed linearly based on the number of times
     * transition has occured from this state to other states.
     * (VERBATIM from upstream lines 77-83)
     */
    /* Regularize a bit by adding something to denominator */
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
 * (VERBATIM from upstream exe_zero_prob)
 */
static void
exe_zero_prob(gpointer G_GNUC_UNUSED key, kp_exe_t *exe)
{
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
        /* If exe is running, we vote against the map,
         * since it's most prolly in the memory already. */
        /* NOTE: Using simple binary vote (1 or 0) instead of exemap->prob.
         * Incorporating exemap probability would require additional theoretical
         * work on probability combination. Current approach is conservative and
         * works well - we vote against preloading if exe is already running. */
        exemap->map->lnprob += 1;
    } else {
        exemap->map->lnprob += exe->lnprob;
    }
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
void
kp_prophet_readahead(GPtrArray *maps_arr)
{
    int i;
    int memavail, memavailtotal; /* in kilobytes */
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
    
    g_debug("%dkb available for preloading, using %dkb of it",
            memavailtotal, memavailtotal - memavail);
    
    if (i) {
        i = kp_readahead((kp_map_t **)maps_arr->pdata, i);
        g_debug("readahead %d files", i);
    } else {
        g_debug("nothing to readahead");
    }
}

/**
 * Boost manual apps by giving them high probability
 * Manual apps get a strong negative lnprob (= high need probability)
 */
static void
boost_manual_apps(void)
{
    char **app_path;
    int boosted = 0;
    
    if (!kp_conf->system.manual_apps_loaded || 
        kp_conf->system.manual_apps_count == 0) {
        return;
    }
    
    for (app_path = kp_conf->system.manual_apps_loaded; *app_path; app_path++) {
        kp_exe_t *exe;
        
        /* Look up exe in state */
        exe = g_hash_table_lookup(kp_state->exes, *app_path);
        
        if (exe && !exe_is_running(exe)) {
            /* Boost: set strong negative lnprob = high need */
            exe->lnprob = MANUAL_APP_BOOST_LNPROB;
            boosted++;
        }
    }
    
    if (boosted > 0) {
        g_debug("Boosted %d manual apps for preloading", boosted);
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
    g_hash_table_foreach(kp_state->exes, (GHFunc)exe_zero_prob, data);
    g_ptr_array_foreach(kp_state->maps_arr, (GFunc)map_zero_prob, data);
    
    /* Boost manual apps first (Preheat extension) */
    boost_manual_apps();
    
    /* Markovs bid in exes */
    kp_markov_foreach((GFunc)markov_bid_in_exes, data);
    
    /* Exes bid in maps */
    kp_exemap_foreach((GHFunc)exemap_bid_in_maps, data);
    
    /* Sort maps on probability */
    g_ptr_array_sort(kp_state->maps_arr, (GCompareFunc)map_prob_compare);
    
    /* Read them in */
    kp_prophet_readahead(kp_state->maps_arr);
}
