/* stats.c - Statistics tracking implementation for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: Statistics Tracking
 * =============================================================================
 *
 * Tracks daemon performance metrics for monitoring and debugging:
 *
 * METRICS TRACKED:
 *   - preloads_total: Number of readahead operations issued
 *   - hits: Apps that were preloaded when launched (success!)
 *   - misses: Apps that were NOT preloaded when launched
 *   - hit_rate: hits / (hits + misses) × 100%
 *   - top_apps: Most frequently launched applications
 *
 * OUTPUT FORMAT (/run/preheat.stats):
 *   uptime_seconds=3600
 *   preloads_total=150
 *   hits=45
 *   misses=12
 *   hit_rate=78.9
 *   apps_tracked=234
 *   top1=firefox,23,1
 *   top2=code,18,1
 *   ...
 *
 * USAGE:
 *   kill -USR1 $(pidof preheat)   # Dump stats to file
 *   preheat-ctl stats             # View stats via CLI
 *
 * DATA STRUCTURES:
 *   - app_launches: GHashTable<app_name, launch_count>
 *   - app_preloaded: GHashTable<app_name, is_preloaded>
 *
 * =============================================================================
 */

#include "common.h"
#include "stats.h"
#include "../utils/logging.h"
#include "../state/state.h"
#include "../config/config.h"
#include "../utils/pattern.h"
#include "../utils/desktop.h"

#include <libgen.h>

/* Stats file location for CLI access */
#define STATS_FILE "/run/preheat.stats"

/* Pool classification info for an app */
typedef struct {
    pool_type_t pool;
    char *reason;  /* Why in this pool (for debugging) */
} app_pool_info_t;

/* Global statistics state */
static struct {
    gboolean initialized;
    time_t daemon_start;

    /* Counters */
    unsigned long preloads_total;
    unsigned long hits;
    unsigned long misses;
    unsigned long memory_pressure_events;  /* Enhancement #5 */

    /* Per-app tracking (simple hash) */
    GHashTable *app_launches;   /* app_name -> launch_count */
    GHashTable *app_preloaded;  /* app_name -> is_preloaded */
    GHashTable *app_pools;      /* app_name -> app_pool_info_t* */
} stats = {0};

/**
 * Initialize statistics subsystem
 */
void
kp_stats_init(void)
{
    stats.initialized = TRUE;
    stats.daemon_start = time(NULL);
    stats.preloads_total = 0;
    stats.hits = 0;
    stats.misses = 0;

    stats.app_launches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    stats.app_preloaded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    stats.app_pools = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 
                                             (GDestroyNotify)g_free);

    g_debug("Statistics subsystem initialized");
}

/* Forward declaration */
static pool_type_t classify_app_pool(const char *app_path, char **reason_out);

/**
 * Reclassify callback for g_hash_table_foreach
 */
static void
reclassify_one_exe(gpointer key, gpointer value, gpointer user_data)
{
    kp_exe_t *exe = (kp_exe_t *)value;
    char *reason = NULL;
    pool_type_t old_pool = exe->pool;
    pool_type_t new_pool;
    
    (void)key;      /* Unused */
    (void)user_data; /* Unused */
    
    /* Reclassify using current logic */
    new_pool = classify_app_pool(exe->path, &reason);
    
    /* Update if changed */
    if (new_pool != old_pool) {
        exe->pool = new_pool;
        g_message("Reclassified %s: %s → %s (reason: %s)",
                  exe->path,
                  old_pool == POOL_PRIORITY ? "priority" : "observation",
                  new_pool == POOL_PRIORITY ? "priority" : "observation",
                  reason ? reason : "unknown");
    }
    
    g_free(reason);
}

/**
 * Reclassify all loaded applications
 * 
 * Called after state load to ensure all apps benefit from updated
 * classification logic (e.g., URI handling fix).
 */
void
kp_stats_reclassify_all(void)
{
    extern kp_state_t kp_state[1];
    
    if (!kp_state->exes) {
        return;
    }
    
    g_message("Reclassifying all applications...");
    g_hash_table_foreach(kp_state->exes, reclassify_one_exe, NULL);
    g_message("Reclassification complete");
}

/**
 * Extract basename from path
 */
static const char *
get_app_name(const char *path)
{
    static char name_buf[256];
    char *path_copy;
    char *base;

    if (!path) return "unknown";

    path_copy = g_strdup(path);
    base = basename(path_copy);
    strncpy(name_buf, base, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    g_free(path_copy);

    return name_buf;
}

/**
 * Check if app is in manual apps list
 */
static gboolean
is_manual_app(const char *app_path)
{
    extern kp_conf_t kp_conf[1];
    
    if (!kp_conf->system.manual_apps_loaded) {
        return FALSE;
    }
    
    for (char **p = kp_conf->system.manual_apps_loaded; *p; p++) {
        if (strcmp(*p, app_path) == 0) {
            return TRUE;
        }
    }
    
    return FALSE;
}

/**
 * Classify application into appropriate pool
 *
 * Priority order (highest to lowest):
 * 1. Manual apps list → POOL_PRIORITY
 * 2. Has .desktop file → POOL_PRIORITY  
 * 3. Matches excluded pattern → POOL_OBSERVATION
 * 4. In user app directory → POOL_PRIORITY
 * 5. Default → POOL_OBSERVATION
 */
static pool_type_t
classify_app_pool(const char *app_path, char **reason_out)
{
    extern kp_conf_t kp_conf[1];
    char *plain_path = NULL;
    char *canonical_path = NULL;
    const char *check_path = app_path;
    pool_type_t result;
    char resolved[PATH_MAX];
    
    /* Convert file:// URI to plain path if needed */
    if (app_path && g_str_has_prefix(app_path, "file://")) {
        plain_path = g_filename_from_uri(app_path, NULL, NULL);
        if (plain_path) {
            check_path = plain_path;
        }
    }
    
    /* Resolve symlinks to canonical path for desktop matching */
    if (check_path && realpath(check_path, resolved)) {
        canonical_path = g_strdup(resolved);
        check_path = canonical_path;
    }
    
    /* Priority 1: Manual apps list (highest priority) */
    if (is_manual_app(check_path)) {
        if (reason_out) *reason_out = g_strdup("manual list");
        result = POOL_PRIORITY;
        goto cleanup;
    }
    
    /* Priority 2: Has .desktop file */
    if (kp_desktop_has_file(check_path)) {
        const char *app_name = kp_desktop_get_name(check_path);
        if (reason_out) {
            *reason_out = g_strdup_printf(".desktop (%s)", 
                                           app_name ? app_name : "unknown");
        }
        result = POOL_PRIORITY;
        goto cleanup;
    }

    
    /* Priority 3: Excluded pattern check */
    if (kp_pattern_matches_any(check_path,
                                kp_conf->system.excluded_patterns_list,
                                kp_conf->system.excluded_patterns_count)) {
        if (reason_out) *reason_out = g_strdup("excluded pattern");
        result = POOL_OBSERVATION;
        goto cleanup;
    }
    
    /* Priority 4: User app path check */
    if (kp_path_in_directories(check_path,
                                 kp_conf->system.user_app_paths_list,
                                 kp_conf->system.user_app_paths_count)) {
        if (reason_out) *reason_out = g_strdup("user app directory");
        result = POOL_PRIORITY;
        goto cleanup;
    }
    
    /* Default: Observation pool */
    if (reason_out) *reason_out = g_strdup("default (no match)");
    result = POOL_OBSERVATION;
    
cleanup:
    g_free(plain_path);
    g_free(canonical_path);
    return result;
}

/**
 * Record a preload event
 */
void
kp_stats_record_preload(const char *app_path)
{
    const char *name;

    if (!stats.initialized) return;

    name = get_app_name(app_path);
    stats.preloads_total++;

    /* Mark as preloaded */
    g_hash_table_replace(stats.app_preloaded, g_strdup(name), GINT_TO_POINTER(1));
}

/**
 * Record a hit (app was preloaded when launched)
 */
void
kp_stats_record_hit(const char *app_path)
{
    const char *name;
    gpointer count;
    pool_type_t pool;
    char *reason = NULL;
    app_pool_info_t *pool_info;

    if (!stats.initialized) return;

    name = get_app_name(app_path);
    pool = classify_app_pool(app_path, &reason);
    stats.hits++;

    /* Track pool classification */
    pool_info = g_new0(app_pool_info_t, 1);
    pool_info->pool = pool;
    pool_info->reason = reason;  /* Takes ownership */
    g_hash_table_replace(stats.app_pools, g_strdup(name), pool_info);

    /* Increment launch count */
    count = g_hash_table_lookup(stats.app_launches, name);
    g_hash_table_replace(stats.app_launches, g_strdup(name),
                         GINT_TO_POINTER(GPOINTER_TO_INT(count) + 1));

    if (pool == POOL_PRIORITY) {
        g_debug("Stats: HIT for %s (priority pool: %s)", name, reason ? reason : "unknown");
    } else {
        g_debug("Stats: HIT for %s (observation pool: %s)", name, reason ? reason : "unknown");
    }
}

/**
 * Record a miss (app was NOT preloaded when launched)
 */
void
kp_stats_record_miss(const char *app_path)
{
    const char *name;
    gpointer count;
    pool_type_t pool;
    char *reason = NULL;
    app_pool_info_t *pool_info;

    if (!stats.initialized) return;

    name = get_app_name(app_path);
    pool = classify_app_pool(app_path, &reason);
    stats.misses++;

    /* Track pool classification */
    pool_info = g_new0(app_pool_info_t, 1);
    pool_info->pool = pool;
    pool_info->reason = reason;  /* Takes ownership */
    g_hash_table_replace(stats.app_pools, g_strdup(name), pool_info);

    /* Increment launch count */
    count = g_hash_table_lookup(stats.app_launches, name);
    g_hash_table_replace(stats.app_launches, g_strdup(name),
                         GINT_TO_POINTER(GPOINTER_TO_INT(count) + 1));

    if (pool == POOL_PRIORITY) {
        g_debug("Stats: MISS for %s (priority pool: %s)", name, reason ? reason : "unknown");
    } else {
        g_debug("Stats: MISS for %s (observation pool: %s)", name, reason ? reason : "unknown");
    }
}


/**
 * Compare function for sorting by weighted launch count (Enhancement #2)
 */
typedef struct {
    const char *name;
    double weighted_launches;   /* Weighted count (duration + user-init) */
    unsigned long raw_launches; /* Raw count for reference */
} app_count_t;

static gint
app_count_compare(gconstpointer a, gconstpointer b)
{
    const app_count_t *aa = a;
    const app_count_t *bb = b;
    /* Sort by weighted launches (descending) */
    if (bb->weighted_launches > aa->weighted_launches) return 1;
    if (bb->weighted_launches < aa->weighted_launches) return -1;
    return 0;
}

/**
 * Get current statistics summary (Enhanced #5: detailed metrics)
 */
void
kp_stats_get_summary(kp_stats_summary_t *summary)
{
    GHashTableIter iter;
    gpointer key, value;
    GArray *sorted;

    memset(summary, 0, sizeof(*summary));

    if (!stats.initialized) return;

    summary->preloads_total = stats.preloads_total;
    summary->preload_hits = stats.hits;
    summary->preload_misses = stats.misses;
    summary->daemon_start = stats.daemon_start;

    /* Calculate hit rate */
    if (stats.hits + stats.misses > 0) {
        summary->hit_rate = (double)stats.hits / (stats.hits + stats.misses) * 100.0;
    }

    /* Count tracked apps from state */
    if (kp_state->exes) {
        summary->apps_tracked = g_hash_table_size(kp_state->exes);
    }

    /* Enhancement #5: Count apps by pool and collect memory stats */
    summary->priority_pool_count = 0;
    summary->observation_pool_count = 0;
    summary->total_preloaded_bytes = 0;
    summary->memory_pressure_events = stats.memory_pressure_events;

    if (kp_state->exes) {
        g_hash_table_iter_init(&iter, kp_state->exes);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            kp_exe_t *exe = (kp_exe_t *)value;
            
            /* Count by pool */
            if (exe->pool == POOL_PRIORITY) {
                summary->priority_pool_count++;
            } else {
                summary->observation_pool_count++;
            }
            
            /* Accumulate memory */
            summary->total_preloaded_bytes += exe->size;
        }
    }

    /* Get top apps from kp_state - PRIORITY POOL ONLY (Enhancement #2/3) */
    /* Enhancement #3: Aggregate by families first, then individual apps */
    sorted = g_array_new(FALSE, FALSE, sizeof(app_count_t));

    /* First, update all family stats */
    if (kp_state->app_families) {
        GHashTableIter fam_iter;
        g_hash_table_iter_init(&fam_iter, kp_state->app_families);
        while (g_hash_table_iter_next(&fam_iter, &key, &value)) {
            kp_app_family_t *family = (kp_app_family_t *)value;
            kp_family_update_stats(family);
        }
    }

    /* Track which exes are in families to avoid double-counting */
    GHashTable *processed_exes = g_hash_table_new(g_str_hash, g_str_equal);

    /* Add families to sortedlist */
    if (kp_state->app_families) {
        GHashTableIter fam_iter;
        g_hash_table_iter_init(&fam_iter, kp_state->app_families);
        while (g_hash_table_iter_next(&fam_iter, &key, &value)) {
            kp_app_family_t *family = (kp_app_family_t *)value;
            
            if (family->total_weighted_launches > 0.0) {
                /* Mark all family members as processed */
                for (guint i = 0; i < family->member_paths->len; i++) {
                    g_hash_table_insert(processed_exes,
                                        g_ptr_array_index(family->member_paths, i),
                                        GINT_TO_POINTER(1));
                }
                
                app_count_t ac = {
                    .name = family->family_id,
                    .weighted_launches = family->total_weighted_launches,
                    .raw_launches = family->total_raw_launches
                };
                g_array_append_val(sorted, ac);
            }
        }
    }

    /* Add individual priority pool apps not in families */
    if (kp_state->exes) {
        g_hash_table_iter_init(&iter, kp_state->exes);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            kp_exe_t *exe = (kp_exe_t *)value;
            
            /* Skip if in a family or not priority pool */
            if (g_hash_table_contains(processed_exes, exe->path))
                continue;
                
            if (exe->pool == POOL_PRIORITY && exe->weighted_launches > 0.0) {
                /* BUGFIX: get_app_name() uses static buffer, so we must copy
                 * the name before storing pointer in GArray. Otherwise all
                 * entries would point to the same (last) name. */
                app_count_t ac = {
                    .name = g_strdup(get_app_name(exe->path)),
                    .weighted_launches = exe->weighted_launches,
                    .raw_launches = exe->raw_launches
                };
                g_array_append_val(sorted, ac);
            }
        }
    }
    
    g_hash_table_destroy(processed_exes);

    g_array_sort(sorted, app_count_compare);

    for (guint i = 0; i < sorted->len && i < STATS_TOP_APPS; i++) {
        app_count_t *ac = &g_array_index(sorted, app_count_t, i);
        app_pool_info_t *pool_info = g_hash_table_lookup(stats.app_pools, ac->name);
        
        summary->top_apps[i].name = g_strdup(ac->name);
        summary->top_apps[i].launches = ac->raw_launches;  /* Use raw count for display */
        summary->top_apps[i].weighted_launches = ac->weighted_launches;
        summary->top_apps[i].preloaded =
            g_hash_table_contains(stats.app_preloaded, ac->name);
        summary->top_apps[i].pool = POOL_PRIORITY;
        summary->top_apps[i].promotion_reason = 
            pool_info ? g_strdup(pool_info->reason) : g_strdup("unknown");
    }

    /* Free the duplicated names in the sorted array */
    for (guint i = 0; i < sorted->len; i++) {
        app_count_t *ac = &g_array_index(sorted, app_count_t, i);
        g_free((gchar *)ac->name);
    }

    g_array_free(sorted, TRUE);
    
    g_debug("Stats summary: %u priority pool apps in top list",
              sorted->len);
}

/**
 * Dump statistics to file (Enhanced for #5: verbose metrics)
 * 
 * SECURITY: Uses O_NOFOLLOW to prevent symlink attacks.
 * If path is a symlink, it's removed and recreated as regular file.
 */
int
kp_stats_dump_to_file(const char *path)
{
    FILE *f;
    kp_stats_summary_t summary;
    time_t now = time(NULL);
    int uptime;
    int fd;

    /* SECURITY: O_NOFOLLOW prevents symlink attacks */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        if (errno == ELOOP) {
            /* Path is a symlink - remove it and retry */
            g_warning("Stats path %s is a symlink (removing)", path);
            if (unlink(path) == 0) {
                fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
            }
        }
        if (fd < 0) {
            g_warning("Cannot create stats file %s: %s", path, strerror(errno));
            return -1;
        }
    }

    f = fdopen(fd, "w");
    if (!f) {
        g_warning("fdopen failed for %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    kp_stats_get_summary(&summary);
    uptime = (int)(now - summary.daemon_start);

    /* Basic stats (backward compatible) */
    fprintf(f, "# Preheat Statistics\n");
    fprintf(f, "version=%s\n", VERSION);
    fprintf(f, "uptime_seconds=%d\n", uptime);
    fprintf(f, "preloads_total=%lu\n", summary.preloads_total);
    fprintf(f, "hits=%lu\n", summary.preload_hits);
    fprintf(f, "misses=%lu\n", summary.preload_misses);
    fprintf(f, "hit_rate=%.1f\n", summary.hit_rate);
    fprintf(f, "apps_tracked=%d\n", summary.apps_tracked);

    /* Enhancement #5: Pool breakdown */
    fprintf(f, "\n# Pool Breakdown\n");
    fprintf(f, "priority_pool=%d\n", summary.priority_pool_count);
    fprintf(f, "observation_pool=%d\n", summary.observation_pool_count);

    /* Enhancement #5: Memory metrics */
    fprintf(f, "\n# Memory\n");
    fprintf(f, "total_preloaded_mb=%zu\n", summary.total_preloaded_bytes / (1024 * 1024));
    fprintf(f, "memory_pressure_events=%lu\n", summary.memory_pressure_events);

    /* Enhancement #5: Top apps (extended to 20 with more details) */
    fprintf(f, "\n# Top Apps (name:weighted:raw:preloaded:pool)\n");
    for (int i = 0; i < STATS_TOP_APPS; i++) {
        if (summary.top_apps[i].name) {
            fprintf(f, "top_app_%d=%s:%.2f:%lu:%d:%s\n",
                    i + 1,
                    summary.top_apps[i].name,
                    summary.top_apps[i].weighted_launches,
                    summary.top_apps[i].launches,
                    summary.top_apps[i].preloaded ? 1 : 0,
                    summary.top_apps[i].pool == POOL_PRIORITY ? "priority" : "observation");
            g_free(summary.top_apps[i].name);
        }
    }

    fclose(f);  /* Also closes fd */

    return 0;
}

/**
 * Record a memory pressure event (Enhancement #5)
 * 
 * Called when preloading is skipped due to insufficient memory.
 * Helps diagnose if system is memory-constrained.
 */
void
kp_stats_record_memory_pressure(void)
{
    if (!stats.initialized) return;
    
    stats.memory_pressure_events++;
    g_debug("Memory pressure event recorded (total: %lu)", stats.memory_pressure_events);
}

/**
 * Get hit rate for a specific app (Enhancement #5)
 * 
 * NOTE: This is currently a stub implementation.
 * Per-app hit rate tracking will be implemented in future enhancement.
 * 
 * @param app_path Path of application
 * @return Hit rate (0.0-100.0), or -1.0 if app not tracked
 */
double
kp_stats_get_app_hit_rate(const char *app_path)
{
    if (!stats.initialized || !app_path) return -1.0;
    
    /* STUB: Per-app hit rate tracking not yet implemented.
     * Returns overall daemon hit rate for now.
     * TODO: Add per-app hits/misses tracking in future enhancement. */
    if (stats.hits + stats.misses > 0) {
        return (double)stats.hits / (stats.hits + stats.misses) * 100.0;
    }
    
    return 0.0;
}

/**
 * Free statistics resources
 * B011 FIX: Ensure all hash tables are freed
 */
void
kp_stats_free(void)
{
    if (stats.app_launches) {
        g_hash_table_destroy(stats.app_launches);
        stats.app_launches = NULL;
    }

    if (stats.app_preloaded) {
        g_hash_table_destroy(stats.app_preloaded);
        stats.app_preloaded = NULL;
    }

    /* B011 FIX: Also free app_pools */
    if (stats.app_pools) {
        g_hash_table_destroy(stats.app_pools);
        stats.app_pools = NULL;
    }

    stats.initialized = FALSE;
}
