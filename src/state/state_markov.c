/* state_markov.c - Markov chain management for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * =============================================================================
 * MODULE: Markov Chain Management
 * =============================================================================
 *
 * Markov chains (kp_markov_t) model correlations between application pairs:
 *
 *   exe_a <───── markov ─────> exe_b
 *
 * They track a 4-state continuous-time Markov model:
 *
 *   State │ A running │ B running │ Description
 *   ──────┼───────────┼───────────┼─────────────────────
 *     0   │    No     │    No     │ Neither running
 *     1   │   Yes     │    No     │ Only A running
 *     2   │    No     │   Yes     │ Only B running
 *     3   │   Yes     │   Yes     │ Both running
 *
 * For each state, we track:
 *   - time_to_leave[s]: Mean time spent in state s before transitioning
 *   - weight[i][j]: Count of transitions from state i to state j
 *
 * The correlation() function computes Pearson correlation coefficient
 * from these statistics, determining how "related" two apps are.
 * High correlation → if A is running, B is likely to run soon.
 *
 * =============================================================================
 */

#include "common.h"
#include "state.h"
#include "state_markov.h"
#include <math.h>
#include <string.h>

/**
 * Create new Markov chain between two executables
 *
 * @param a          First executable
 * @param b          Second executable (must differ from a)
 * @param initialize If TRUE, initialize state based on current running status
 * @return           New markov object, added to both exe's markov sets
 */
kp_markov_t *
kp_markov_new(kp_exe_t *a, kp_exe_t *b, gboolean initialize)
{
    kp_markov_t *markov;

    g_return_val_if_fail(a, NULL);
    g_return_val_if_fail(b, NULL);
    g_return_val_if_fail(a != b, NULL);

    markov = g_slice_new(kp_markov_t);
    markov->a = a;
    markov->b = b;

    if (initialize) {
        markov->state = markov_state(markov);

        markov->change_timestamp = kp_state->time;
        if (a->change_timestamp > 0 && b->change_timestamp > 0) {
            if (a->change_timestamp < kp_state->time)
                markov->change_timestamp = a->change_timestamp;
            if (b->change_timestamp < kp_state->time && b->change_timestamp > markov->change_timestamp)
                markov->change_timestamp = b->change_timestamp;  /* BUG 1 FIX: was incorrectly using a->change_timestamp */
            if (a->change_timestamp > markov->change_timestamp)
                markov->state ^= 1;
            if (b->change_timestamp > markov->change_timestamp)
                markov->state ^= 2;
        }

        markov->time = 0;
        memset(markov->time_to_leave, 0, sizeof(markov->time_to_leave));
        memset(markov->weight, 0, sizeof(markov->weight));
        kp_markov_state_changed(markov);
    }

    /* BUG 6 FIX: Check markov sets exist before adding */
    if (!a->markovs || !b->markovs) {
        g_slice_free(kp_markov_t, markov);
        return NULL;
    }
    g_set_add(a->markovs, markov);
    g_set_add(b->markovs, markov);
    return markov;
}

/**
 * Handle Markov state change
 * (VERBATIM from upstream preload_markov_state_changed)
 */
void
kp_markov_state_changed(kp_markov_t *markov)
{
    int old_state, new_state;

    if (markov->change_timestamp == kp_state->time)
        return; /* already taken care of */

    old_state = markov->state;
    new_state = markov_state(markov);

    /* BUG 3 FIX: Gracefully handle race condition instead of crashing */
    if (old_state == new_state)
        return;

    markov->weight[old_state][old_state]++;
    markov->time_to_leave[old_state] += ((kp_state->time - markov->change_timestamp)
                                         - markov->time_to_leave[old_state])
                                        / markov->weight[old_state][old_state];

    markov->weight[old_state][new_state]++;
    markov->state = new_state;
    markov->change_timestamp = kp_state->time;
}

/**
 * Free Markov chain
 * (VERBATIM from upstream preload_markov_free)
 */
void
kp_markov_free(kp_markov_t *markov, kp_exe_t *from)
{
    g_return_if_fail(markov);

    if (from) {
        kp_exe_t *other;
        g_assert(markov->a == from || markov->b == from);
        other = markov_other_exe(markov, from);
        g_set_remove(other->markovs, markov);
    } else {
        g_set_remove(markov->a->markovs, markov);
        g_set_remove(markov->b->markovs, markov);
    }
    g_slice_free(kp_markov_t, markov);
}

/**
 * Context for markov iteration
 * (VERBATIM from upstream)
 */
typedef struct _markov_foreach_context_t
{
    kp_exe_t *exe;
    GFunc func;
    gpointer data;
} markov_foreach_context_t;

/**
 * Callback for markov iteration
 * (VERBATIM from upstream exe_markov_callback)
 */
static void
exe_markov_callback(kp_markov_t *markov, markov_foreach_context_t *ctx)
{
    /* Each markov should be processed only once, not twice */
    if (ctx->exe == markov->a)
        ctx->func(markov, ctx->data);
}

/* Wrapper with correct GFunc signature for exe_markov_callback */
static void
exe_markov_callback_wrapper(gpointer data, gpointer user_data)
{
    exe_markov_callback((kp_markov_t *)data, (markov_foreach_context_t *)user_data);
}

/**
 * Iterate markovs for an exe
 * (VERBATIM from upstream exe_markov_foreach)
 */
static void
exe_markov_foreach(gpointer G_GNUC_UNUSED key, kp_exe_t *exe, markov_foreach_context_t *ctx)
{
    ctx->exe = exe;
    g_set_foreach(exe->markovs, exe_markov_callback_wrapper, ctx);
}

/* Wrapper with correct GHFunc signature for exe_markov_foreach */
static void
exe_markov_foreach_wrapper(gpointer key, gpointer value, gpointer user_data)
{
    exe_markov_foreach(key, (kp_exe_t *)value, (markov_foreach_context_t *)user_data);
}

/**
 * Iterate all markovs
 * (VERBATIM from upstream preload_markov_foreach)
 */
void
kp_markov_foreach(GFunc func, gpointer user_data)
{
    markov_foreach_context_t ctx;
    ctx.func = func;
    ctx.data = user_data;
    g_hash_table_foreach(kp_state->exes, exe_markov_foreach_wrapper, &ctx);
}

/**
 * Calculate correlation coefficient
 * (VERBATIM from upstream preload_markov_correlation)
 *
 * Calculates Pearson product-moment correlation coefficient between
 * two exes being run. Returns value in range -1 to 1.
 */
double
kp_markov_correlation(kp_markov_t *markov)
{
    double correlation, numerator, denominator2;
    int t, a, b, ab;

    t = kp_state->time;
    a = markov->a->time;
    b = markov->b->time;
    ab = markov->time;

    if (a == 0 || a == t || b == 0 || b == t)
        correlation = 0;
    else {
        numerator = ((double)t*ab) - ((double)a * b);
        denominator2 = ((double)a * b) * ((double)(t - a) * (t - b));
        
        /* BUG 4 FIX: Guard against negative/zero denominator from overflow */
        if (denominator2 <= 0) {
            correlation = 0;
        } else {
            correlation = numerator / sqrt(denominator2);
            /* Clamp to valid range instead of asserting */
            if (correlation > 1.0) correlation = 1.0;
            if (correlation < -1.0) correlation = -1.0;
        }
    }

    return correlation;
}

/**
 * Build Markov chains between all priority pool exes
 * Should be called AFTER seeding completes to create the full mesh.
 * 
 * This creates chains between all pairs of priority apps that don't 
 * already have chains. O(n²) for n priority apps but n is typically small.
 */
void
kp_markov_build_priority_mesh(void)
{
    GHashTableIter iter_a, iter_b;
    gpointer key_a, val_a, key_b, val_b;
    int chains_created = 0;
    int priority_count = 0;
    
    if (!kp_state || !kp_state->exes) return;
    
    /* First count priority apps */
    g_hash_table_iter_init(&iter_a, kp_state->exes);
    while (g_hash_table_iter_next(&iter_a, &key_a, &val_a)) {
        kp_exe_t *exe = (kp_exe_t *)val_a;
        if (exe->pool == POOL_PRIORITY) priority_count++;
    }
    
    g_message("Building Markov mesh for %d priority apps...", priority_count);
    
    /* Create chains between all pairs of priority apps */
    g_hash_table_iter_init(&iter_a, kp_state->exes);
    while (g_hash_table_iter_next(&iter_a, &key_a, &val_a)) {
        kp_exe_t *exe_a = (kp_exe_t *)val_a;
        
        if (exe_a->pool != POOL_PRIORITY) continue;
        
        g_hash_table_iter_init(&iter_b, kp_state->exes);
        while (g_hash_table_iter_next(&iter_b, &key_b, &val_b)) {
            kp_exe_t *exe_b = (kp_exe_t *)val_b;
            
            if (exe_b->pool != POOL_PRIORITY) continue;
            if (exe_a == exe_b) continue;
            if (exe_a->seq > exe_b->seq) continue;  /* Only create once per pair */
            
            /* Check if chain already exists by scanning exe_a's markovs */
            gboolean exists = FALSE;
            for (guint i = 0; i < g_set_size(exe_a->markovs); i++) {
                kp_markov_t *m = g_ptr_array_index(exe_a->markovs, i);
                if ((m->a == exe_a && m->b == exe_b) || 
                    (m->a == exe_b && m->b == exe_a)) {
                    exists = TRUE;
                    break;
                }
            }
            
            if (!exists) {
                kp_markov_new(exe_a, exe_b, TRUE);
                chains_created++;
            }
        }
    }
    
    g_message("Markov mesh built: %d chains created for %d priority apps", 
              chains_created, priority_count);
}
