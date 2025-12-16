/* state.c - State management implementation for Preheat
 *
 * Based on preload 0.6.4 state.c (VERBATIM helper functions)
 * Based on the preload daemon
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.h"
#include "../utils/logging.h"
#include "../utils/crc32.h"
#include "../config/config.h"
#include "state.h"
#include "../monitor/proc.h"
#include "../monitor/spy.h"
#include "../predict/prophet.h"

#include <math.h>

/* Global state singleton */
kp_state_t kp_state[1];

/* ========================================================================
 * MAP MANAGEMENT FUNCTIONS (VERBATIM from upstream)
 * ======================================================================== */

/**
 * Create new map
 * (VERBATIM from upstream preload_map_new)
 */
kp_map_t *
kp_map_new(const char *path, size_t offset, size_t length)
{
    kp_map_t *map;
    
    g_return_val_if_fail(path, NULL);
    
    map = g_slice_new(kp_map_t);
    map->path = g_strdup(path);
    map->offset = offset;
    map->length = length;
    map->refcount = 0;
    map->update_time = kp_state->time;
    map->block = -1;
    return map;
}

/**
 * Free map
 * (VERBATIM from upstream preload_map_free)
 */
void
kp_map_free(kp_map_t *map)
{
    g_return_if_fail(map);
    g_return_if_fail(map->refcount == 0);
    g_return_if_fail(map->path);
    
    g_free(map->path);
    map->path = NULL;
    g_slice_free(kp_map_t, map);
}

/**
 * Register map in state
 * (VERBATIM from upstream preload_state_register_map)
 */
static void
kp_state_register_map(kp_map_t *map)
{
    g_return_if_fail(!g_hash_table_lookup(kp_state->maps, map));
    
    map->seq = ++(kp_state->map_seq);
    g_hash_table_insert(kp_state->maps, map, GINT_TO_POINTER(1));
    g_ptr_array_add(kp_state->maps_arr, map);
}

/**
 * Unregister map from state
 * (VERBATIM from upstream preload_state_unregister_map)
 */
static void
kp_state_unregister_map(kp_map_t *map)
{
    g_return_if_fail(g_hash_table_lookup(kp_state->maps, map));
    
    g_ptr_array_remove(kp_state->maps_arr, map);
    g_hash_table_remove(kp_state->maps, map);
}

/**
 * Reference map (register if needed)
 * (VERBATIM from upstream preload_map_ref)
 */
void
kp_map_ref(kp_map_t *map)
{
    if (!map->refcount)
        kp_state_register_map(map);
    map->refcount++;
}

/**
 * Unreference map (unregister and free if refcount reaches 0)
 * (VERBATIM from upstream preload_map_unref)
 */
void
kp_map_unref(kp_map_t *map)
{
    g_return_if_fail(map);
    g_return_if_fail(map->refcount > 0);
    
    map->refcount--;
    if (!map->refcount) {
        kp_state_unregister_map(map);
        kp_map_free(map);
    }
}

/**
 * Get map size
 * (VERBATIM from upstream preload_map_get_size)
 */
size_t
kp_map_get_size(kp_map_t *map)
{
    g_return_val_if_fail(map, 0);
    return map->length;
}

/**
 * Hash function for maps
 * (VERBATIM from upstream preload_map_hash)
 */
guint
kp_map_hash(kp_map_t *map)
{
    g_return_val_if_fail(map, 0);
    g_return_val_if_fail(map->path, 0);
    
    return g_str_hash(map->path)
         + g_direct_hash(GSIZE_TO_POINTER(map->offset))
         + g_direct_hash(GSIZE_TO_POINTER(map->length));
}

/**
 * Equality function for maps
 * (VERBATIM from upstream preload_map_equal)
 */
gboolean
kp_map_equal(kp_map_t *a, kp_map_t *b)
{
    return a->offset == b->offset && a->length == b->length && !strcmp(a->path, b->path);
}

/* ========================================================================
 * EXEMAP MANAGEMENT FUNCTIONS (VERBATIM from upstream)
 * ======================================================================== */

/**
 * Create new exemap
 * (VERBATIM from upstream preload_exemap_new)
 */
kp_exemap_t *
kp_exemap_new(kp_map_t *map)
{
    kp_exemap_t *exemap;
    
    g_return_val_if_fail(map, NULL);
    
    kp_map_ref(map);
    exemap = g_slice_new(kp_exemap_t);
    exemap->map = map;
    exemap->prob = 1.0;
    return exemap;
}

/**
 * Free exemap
 * (VERBATIM from upstream preload_exemap_free)
 */
void
kp_exemap_free(kp_exemap_t *exemap)
{
    g_return_if_fail(exemap);
    
    if (exemap->map)
        kp_map_unref(exemap->map);
    g_slice_free(kp_exemap_t, exemap);
}

/**
 * Context for exemap iteration
 * (VERBATIM from upstream)
 */
typedef struct _exemap_foreach_context_t
{
    kp_exe_t *exe;
    GHFunc func;
    gpointer data;
} exemap_foreach_context_t;

/**
 * Callback for exemap iteration
 * (VERBATIM from upstream exe_exemap_callback)
 */
static void
exe_exemap_callback(kp_exemap_t *exemap, exemap_foreach_context_t *ctx)
{
    ctx->func(exemap, ctx->exe, ctx->data);
}

/**
 * Iterate exemaps for an exe
 * (VERBATIM from upstream exe_exemap_foreach)
 */
static void
exe_exemap_foreach(gpointer G_GNUC_UNUSED key, kp_exe_t *exe, exemap_foreach_context_t *ctx)
{
    ctx->exe = exe;
    g_set_foreach(exe->exemaps, (GFunc)exe_exemap_callback, ctx);
}

/**
 * Iterate all exemaps
 * (VERBATIM from upstream preload_exemap_foreach)
 */
void
kp_exemap_foreach(GHFunc func, gpointer user_data)
{
    exemap_foreach_context_t ctx;
    ctx.func = func;
    ctx.data = user_data;
    g_hash_table_foreach(kp_state->exes, (GHFunc)exe_exemap_foreach, &ctx);
}

/* ========================================================================
 * MARKOV MANAGEMENT FUNCTIONS (VERBATIM from upstream)
 * ======================================================================== */

/**
 * Create new Markov chain
 * (VERBATIM from upstream preload_markov_new)
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
                markov->change_timestamp = a->change_timestamp;
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
    
    g_return_if_fail(old_state != new_state);
    
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

/**
 * Iterate markovs for an exe
 * (VERBATIM from upstream exe_markov_foreach)
 */
static void
exe_markov_foreach(gpointer G_GNUC_UNUSED key, kp_exe_t *exe, markov_foreach_context_t *ctx)
{
    ctx->exe = exe;
    g_set_foreach(exe->markovs, (GFunc)exe_markov_callback, ctx);
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
    g_hash_table_foreach(kp_state->exes, (GHFunc)exe_markov_foreach, &ctx);
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
        correlation = numerator / sqrt(denominator2);
    }
    
    g_assert(fabs(correlation) <= 1.00001);
    return correlation;
}

/* ========================================================================
 * EXE MANAGEMENT FUNCTIONS (VERBATIM from upstream)
 * ======================================================================== */

/**
 * Add map size to exe
 * (VERBATIM from upstream exe_add_map_size)
 */
static void
exe_add_map_size(kp_exemap_t *exemap, kp_exe_t *exe)
{
    exe->size += kp_map_get_size(exemap->map);
}

/**
 * Create new exe
 * (VERBATIM from upstream preload_exe_new)
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
    
    if (running) {
        exe->update_time = exe->running_timestamp = kp_state->last_running_timestamp;
    } else {
        exe->update_time = exe->running_timestamp = -1;
    }
    
    if (!exemaps)
        exe->exemaps = g_set_new();
    else
        exe->exemaps = exemaps;
    
    g_set_foreach(exe->exemaps, (GFunc)exe_add_map_size, exe);
    exe->markovs = g_set_new();
    return exe;
}

/**
 * Free exe
 * (VERBATIM from upstream preload_exe_free)
 */
void
kp_exe_free(kp_exe_t *exe)
{
    g_return_if_fail(exe);
    g_return_if_fail(exe->path);
    
    g_set_foreach(exe->exemaps, (GFunc)kp_exemap_free, NULL);
    g_set_free(exe->exemaps);
    exe->exemaps = NULL;
    
    g_set_foreach(exe->markovs, (GFunc)kp_markov_free, exe);
    g_set_free(exe->markovs);
    exe->markovs = NULL;
    
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

/**
 * Register exe in state
 * (VERBATIM from upstream preload_state_register_exe)
 */
void
kp_state_register_exe(kp_exe_t *exe, gboolean create_markovs)
{
    g_return_if_fail(!g_hash_table_lookup(kp_state->exes, exe));
    
    exe->seq = ++(kp_state->exe_seq);
    if (create_markovs) {
        g_hash_table_foreach(kp_state->exes, (GHFunc)shift_kp_markov_new, exe);
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
    
    g_set_foreach(exe->markovs, (GFunc)kp_markov_free, exe);
    g_set_free(exe->markovs);
    exe->markovs = NULL;
    g_hash_table_remove(kp_state->exes, exe);
}

/* ========================================================================
 * STATE FILE FORMAT (VERBATIM from upstream)
 * ======================================================================== */

#define TAG_PRELOAD     "PRELOAD"
#define TAG_MAP         "MAP"
#define TAG_BADEXE      "BADEXE"
#define TAG_EXE         "EXE"
#define TAG_EXEMAP      "EXEMAP"
#define TAG_MARKOV      "MARKOV"
#define TAG_CRC32       "CRC32"

#define READ_TAG_ERROR              "invalid tag"
#define READ_SYNTAX_ERROR           "invalid syntax"
#define READ_INDEX_ERROR            "invalid index"
#define READ_DUPLICATE_INDEX_ERROR  "duplicate index"
#define READ_DUPLICATE_OBJECT_ERROR "duplicate object"
#define READ_CRC_ERROR              "CRC32 checksum mismatch"

typedef struct _read_context_t
{
    char *line;
    const char *errmsg;
    char *path;
    GHashTable *maps;
    GHashTable *exes;
    gpointer data;
    GError *err;
    char filebuf[FILELEN];
} read_context_t;

/* Read map from state file (VERBATIM from upstream) */
static void
read_map(read_context_t *rc)
{
    kp_map_t *map;
    int update_time;
    int i, expansion;
    long offset, length;
    char *path;
    
    if (6 > sscanf(rc->line,
                   "%d %d %lu %lu %d %"FILELENSTR"s",
                   &i, &update_time, &offset, &length, &expansion, rc->filebuf)) {
        rc->errmsg = READ_SYNTAX_ERROR;
        return;
    }
    
    path = g_filename_from_uri(rc->filebuf, NULL, &(rc->err));
    if (!path)
        return;
    
    map = kp_map_new(path, offset, length);
    g_free(path);
    if (g_hash_table_lookup(rc->maps, GINT_TO_POINTER(i))) {
        rc->errmsg = READ_DUPLICATE_INDEX_ERROR;
        goto err;
    }
    if (g_hash_table_lookup(kp_state->maps, map)) {
        rc->errmsg = READ_DUPLICATE_OBJECT_ERROR;
        goto err;
    }
    
    map->update_time = update_time;
    kp_map_ref(map);
    g_hash_table_insert(rc->maps, GINT_TO_POINTER(i), map);
    return;
    
err:
    kp_map_free(map);
}

/* Read bad exe from state file (VERBATIM from upstream) */
static void
read_badexe(read_context_t *rc)
{
    /* We do not read-in badexes. Let's clean them up on every start, give them
     * another chance! */
    return;
}

/* Read exe from state file (VERBATIM from upstream) */
static void
read_exe(read_context_t *rc)
{
    kp_exe_t *exe;
    int update_time, time;
    int i, expansion;
    char *path;
    
    if (5 > sscanf(rc->line,
                   "%d %d %d %d %"FILELENSTR"s",
                   &i, &update_time, &time, &expansion, rc->filebuf)) {
        rc->errmsg = READ_SYNTAX_ERROR;
        return;
    }
    
    path = g_filename_from_uri(rc->filebuf, NULL, &(rc->err));
    if (!path)
        return;
    
    exe = kp_exe_new(path, FALSE, NULL);
    exe->change_timestamp = -1;
    g_free(path);
    if (g_hash_table_lookup(rc->exes, GINT_TO_POINTER(i))) {
        rc->errmsg = READ_DUPLICATE_INDEX_ERROR;
        goto err;
    }
    if (g_hash_table_lookup(kp_state->exes, exe->path)) {
        rc->errmsg = READ_DUPLICATE_OBJECT_ERROR;
        goto err;
    }
    
    exe->update_time = update_time;
    exe->time = time;
    g_hash_table_insert(rc->exes, GINT_TO_POINTER(i), exe);
    kp_state_register_exe(exe, FALSE);
    return;
    
err:
    kp_exe_free(exe);
}

/* Read exemap from state file (VERBATIM from upstream) */
static void
read_exemap(read_context_t *rc)
{
    int iexe, imap;
    kp_exe_t *exe;
    kp_map_t *map;
    kp_exemap_t *exemap;
    double prob;
    
    if (3 > sscanf(rc->line,
                   "%d %d %lg",
                   &iexe, &imap, &prob)) {
        rc->errmsg = READ_SYNTAX_ERROR;
        return;
    }
    
    exe = g_hash_table_lookup(rc->exes, GINT_TO_POINTER(iexe));
    map = g_hash_table_lookup(rc->maps, GINT_TO_POINTER(imap));
    if (!exe || !map) {
        rc->errmsg = READ_INDEX_ERROR;
        return;
    }
    
    exemap = kp_exe_map_new(exe, map);
    exemap->prob = prob;
}

/* Read markov from state file (VERBATIM from upstream) */
static void
read_markov(read_context_t *rc)
{
    int time, state, state_new;
    int ia, ib;
    kp_exe_t *a, *b;
    kp_markov_t *markov;
    int n;
    
    n = 0;
    if (3 > sscanf(rc->line,
                   "%d %d %d%n",
                   &ia, &ib, &time, &n)) {
        rc->errmsg = READ_SYNTAX_ERROR;
        return;
    }
    rc->line += n;
    
    a = g_hash_table_lookup(rc->exes, GINT_TO_POINTER(ia));
    b = g_hash_table_lookup(rc->exes, GINT_TO_POINTER(ib));
    if (!a || !b) {
        rc->errmsg = READ_INDEX_ERROR;
        return;
    }
    
    markov = kp_markov_new(a, b, FALSE);
    markov->time = time;
    
    for (state = 0; state < 4; state++) {
        double x;
        if (1 > sscanf(rc->line,
                       "%lg%n",
                       &x, &n)) {
            rc->errmsg = READ_SYNTAX_ERROR;
            return;
        }
        
        rc->line += n;
        markov->time_to_leave[state] = x;
    }
    for (state = 0; state < 4; state++) {
        for (state_new = 0; state_new < 4; state_new++) {
            int x;
            if (1 > sscanf(rc->line,
                           "%d%n",
                           &x, &n)) {
                rc->errmsg = READ_SYNTAX_ERROR;
                return;
            }
            
            rc->line += n;
            markov->weight[state][state_new] = x;
        }
    }
}

/* Helper callbacks for state initialization (VERBATIM from upstream) */
static void
set_running_process_callback(pid_t G_GNUC_UNUSED pid, const char *path, int time)
{
    kp_exe_t *exe;
    
    exe = g_hash_table_lookup(kp_state->exes, path);
    if (exe) {
        exe->running_timestamp = time;
        kp_state->running_exes = g_slist_prepend(kp_state->running_exes, exe);
    }
}

static void
set_markov_state_callback(kp_markov_t *markov)
{
    markov->state = markov_state(markov);
}

/**
 * Handle corrupt state file by renaming it and logging
 * Returns TRUE if state file can be recovered (fresh start)
 */
static gboolean
handle_corrupt_statefile(const char *statefile, const char *reason)
{
    char *broken_path;
    time_t now;
    struct tm *tm_info;
    char timestamp[32];
    
    now = time(NULL);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
    
    broken_path = g_strdup_printf("%s.broken.%s", statefile, timestamp);
    
    if (rename(statefile, broken_path) == 0) {
        g_warning("State file corrupt (%s), renamed to %s - starting fresh", 
                  reason, broken_path);
    } else {
        g_warning("State file corrupt (%s), could not rename: %s - starting fresh",
                  reason, strerror(errno));
    }
    
    g_free(broken_path);
    return TRUE;  /* OK to continue with fresh state */
}

/* Read state from GIOChannel (VERBATIM from upstream) */
static char *
read_state(GIOChannel *f)
{
    int lineno;
    GString *linebuf;
    GIOStatus s;
    char tag[32] = "";
    char *errmsg;
    
    read_context_t rc;
    
    rc.errmsg = NULL;
    rc.err = NULL;
    rc.maps = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)kp_map_unref);
    rc.exes = g_hash_table_new(g_direct_hash, g_direct_equal);
    
    linebuf = g_string_sized_new(100);
    lineno = 0;
    
    while (!rc.err && !rc.errmsg) {
        s = g_io_channel_read_line_string(f, linebuf, NULL, &rc.err);
        if (s == G_IO_STATUS_AGAIN)
            continue;
        if (s == G_IO_STATUS_EOF || s == G_IO_STATUS_ERROR)
            break;
        
        lineno++;
        rc.line = linebuf->str;
        
        if (1 > sscanf(rc.line,
                       "%31s",
                       tag)) {
            rc.errmsg = READ_TAG_ERROR;
            break;
        }
        rc.line += strlen(tag);
        
        if (lineno == 1 && strcmp(tag, TAG_PRELOAD)) {
            g_warning("State file has invalid header, ignoring it");
            break;
        }
        
        if (!strcmp(tag, TAG_PRELOAD)) {
            int major_ver_read, major_ver_run;
            const char *version;
            int time;
            
            if (lineno != 1 || 2 > sscanf(rc.line,
                                          "%d.%*[^\t]\t%d",
                                          &major_ver_read, &time)) {
                rc.errmsg = READ_SYNTAX_ERROR;
                break;
            }
            
            version = VERSION;
            major_ver_run = strtod(version, NULL);
            
            if (major_ver_run < major_ver_read) {
                g_warning("State file is of a newer version, ignoring it");
                break;
            } else if (major_ver_run > major_ver_read) {
                g_warning("State file is of an old version that I cannot understand anymore, ignoring it");
                break;
            }
            
            kp_state->last_accounting_timestamp = kp_state->time = time;
        }
        else if (!strcmp(tag, TAG_MAP))    read_map(&rc);
        else if (!strcmp(tag, TAG_BADEXE)) read_badexe(&rc);
        else if (!strcmp(tag, TAG_EXE))    read_exe(&rc);
        else if (!strcmp(tag, TAG_EXEMAP)) read_exemap(&rc);
        else if (!strcmp(tag, TAG_MARKOV)) read_markov(&rc);
        else if (linebuf->str[0] && linebuf->str[0] != '#') {
            rc.errmsg = READ_TAG_ERROR;
            break;
        }
    }
    
    g_string_free(linebuf, TRUE);
    g_hash_table_destroy(rc.exes);
    g_hash_table_destroy(rc.maps);
    
    if (rc.err)
        rc.errmsg = rc.err->message;
    if (rc.errmsg)
        errmsg = g_strdup_printf("line %d: %s", lineno, rc.errmsg);
    else
        errmsg = NULL;
    if (rc.err)
        g_error_free(rc.err);
    
    if (!errmsg) {
        kp_proc_foreach((GHFunc)set_running_process_callback, GINT_TO_POINTER(kp_state->time));
        kp_state->last_running_timestamp = kp_state->time;
        kp_markov_foreach((GFunc)set_markov_state_callback, NULL);
    }
    
    return errmsg;
}

/**
 * Load state from file
 * Modified from upstream to handle corruption gracefully
 */
void kp_state_load(const char *statefile)
{
    memset(kp_state, 0, sizeof(*kp_state));
    kp_state->exes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)kp_exe_free);
    kp_state->bad_exes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    kp_state->maps = g_hash_table_new((GHashFunc)kp_map_hash, (GEqualFunc)kp_map_equal);
    kp_state->maps_arr = g_ptr_array_new();
    
    if (statefile && *statefile) {
        GIOChannel *f;
        GError *err = NULL;
        
        g_message("loading state from %s", statefile);
        
        f = g_io_channel_new_file(statefile, "r", &err);
        if (!f) {
            if (err->code == G_FILE_ERROR_ACCES) {
                /* Permission denied - this is a configuration problem, warn but continue */
                g_critical("cannot open %s for reading: %s - continuing without saved state", 
                           statefile, err->message);
            } else {
                /* File doesn't exist or other non-fatal error */
                g_warning("cannot open %s for reading, ignoring: %s", statefile, err->message);
            }
            g_error_free(err);
        } else {
            char *errmsg;
            
            errmsg = read_state(f);
            g_io_channel_unref(f);
            if (errmsg) {
                /* Corruption detected - rename broken file and start fresh */
                handle_corrupt_statefile(statefile, errmsg);
                g_free(errmsg);
                /* Already warned in handle_corrupt_statefile, state remains fresh */
            }
        }
        
        g_debug("loading state done");
    }
    
    kp_proc_get_memstat(&(kp_state->memstat));
    kp_state->memstat_timestamp = kp_state->time;
}

/* ========================================================================
 * STATE WRITE FUNCTIONS (VERBATIM from upstream)
 * ======================================================================== */

#define write_it(s) \
    if (wc->err || G_IO_STATUS_NORMAL != g_io_channel_write_chars(wc->f, s, -1, NULL, &(wc->err))) \
        return;
#define write_tag(tag) write_it(tag "\t")
#define write_string(string) write_it((string)->str)
#define write_ln() write_it("\n")

typedef struct _write_context_t
{
    GIOChannel *f;
    GString *line;
    GError *err;
} write_context_t;

/* Write header (VERBATIM from upstream write_header) */
/* Write header (VERBATIM from upstream write_header) */
static void
write_header(write_context_t *wc)
{
    write_tag(TAG_PRELOAD);
    g_string_printf(wc->line,
                    "%s\t%d",
                    VERSION, kp_state->time);
    write_string(wc->line);
    write_ln();
}

/* Write map (VERBATIM from upstream write_map) */
static void
write_map(kp_map_t *map, gpointer G_GNUC_UNUSED data, write_context_t *wc)
{
    char *uri;
    
    uri = g_filename_to_uri(map->path, NULL, &(wc->err));
    if (!uri)
        return;
    
    write_tag(TAG_MAP);
    g_string_printf(wc->line,
                    "%d\t%d\t%lu\t%lu\t%d\t%s",
                    map->seq, map->update_time, (long)map->offset, (long)map->length, -1/*expansion*/, uri);
    write_string(wc->line);
    write_ln();
    
    g_free(uri);
}

/* Write bad exe (VERBATIM from upstream write_badexe) */
static void
write_badexe(char *path, int update_time, write_context_t *wc)
{
    char *uri;
    
    uri = g_filename_to_uri(path, NULL, &(wc->err));
    if (!uri)
        return;
    
    write_tag(TAG_BADEXE);
    g_string_printf(wc->line,
                    "%d\t%d\t%s",
                    update_time, -1/*expansion*/, uri);
    write_string(wc->line);
    write_ln();
    
    g_free(uri);
}

/* Write exe (VERBATIM from upstream write_exe) */
static void
write_exe(gpointer G_GNUC_UNUSED key, kp_exe_t *exe, write_context_t *wc)
{
    char *uri;
    
    uri = g_filename_to_uri(exe->path, NULL, &(wc->err));
    if (!uri)
        return;
    
    write_tag(TAG_EXE);
    g_string_printf(wc->line,
                    "%d\t%d\t%d\t%d\t%s",
                    exe->seq, exe->update_time, exe->time, -1/*expansion*/, uri);
    write_string(wc->line);
    write_ln();
    
    g_free(uri);
}

/* Write exemap (VERBATIM from upstream write_exemap) */
static void
write_exemap(kp_exemap_t *exemap, kp_exe_t *exe, write_context_t *wc)
{
    write_tag(TAG_EXEMAP);
    g_string_printf(wc->line,
                    "%d\t%d\t%lg",
                    exe->seq, exemap->map->seq, exemap->prob);
    write_string(wc->line);
    write_ln();
}

/* Write markov (VERBATIM from upstream write_markov) */
static void
write_markov(kp_markov_t *markov, write_context_t *wc)
{
    int state, state_new;
    
    write_tag(TAG_MARKOV);
    g_string_printf(wc->line,
                    "%d\t%d\t%d",
                    markov->a->seq, markov->b->seq, markov->time);
    write_string(wc->line);
    
    for (state = 0; state < 4; state++) {
        g_string_printf(wc->line,
                        "\t%lg",
                        markov->time_to_leave[state]);
        write_string(wc->line);
    }
    for (state = 0; state < 4; state++) {
        for (state_new = 0; state_new < 4; state_new++) {
            g_string_printf(wc->line,
                            "\t%d",
                            markov->weight[state][state_new]);
            write_string(wc->line);
        }
    }
    
    write_ln();
}

/* Write CRC32 footer for state file integrity */
static void
write_crc32(write_context_t *wc, int fd)
{
    uint32_t crc;
    off_t file_size;
    char *content;
    
    /* Get file size */
    file_size = lseek(fd, 0, SEEK_CUR);
    if (file_size <= 0) {
        return;  /* Can't calculate CRC, skip silently */
    }
    
    /* Read file contents to calculate CRC */
    content = g_malloc(file_size);
    if (!content) {
        return;
    }
    
    if (lseek(fd, 0, SEEK_SET) < 0) {
        g_free(content);
        return;
    }
    
    if (read(fd, content, file_size) != file_size) {
        g_free(content);
        /* Seek back to end */
        lseek(fd, 0, SEEK_END);
        return;
    }
    
    /* Calculate CRC32 */
    crc = kp_crc32(content, file_size);
    g_free(content);
    
    /* Seek back to end */
    lseek(fd, 0, SEEK_END);
    
    /* Write CRC32 footer */
    write_tag(TAG_CRC32);
    g_string_printf(wc->line, "%08X", crc);
    write_string(wc->line);
    write_ln();
}

/* Write state to GIOChannel with CRC32 footer */
static char *
write_state(GIOChannel *f, int fd)
{
    write_context_t wc;
    
    wc.f = f;
    wc.line = g_string_sized_new(100);
    wc.err = NULL;
    
    write_header(&wc);
    if (!wc.err) g_hash_table_foreach   (kp_state->maps, (GHFunc)write_map, &wc);
    if (!wc.err) g_hash_table_foreach   (kp_state->bad_exes, (GHFunc)write_badexe, &wc);
    if (!wc.err) g_hash_table_foreach   (kp_state->exes, (GHFunc)write_exe, &wc);
    if (!wc.err) kp_exemap_foreach ((GHFunc)write_exemap, &wc);
    if (!wc.err) kp_markov_foreach ((GFunc)write_markov, &wc);
    
    /* Flush before CRC calculation */
    if (!wc.err) {
        g_io_channel_flush(f, &wc.err);
    }
    
    /* Add CRC32 footer */
    if (!wc.err) {
        write_crc32(&wc, fd);
    }
    
    g_string_free(wc.line, TRUE);
    if (wc.err) {
        char *tmp;
        tmp = g_strdup(wc.err->message);
        g_error_free(wc.err);
        return tmp;
    } else
        return NULL;
}

static gboolean
true_func(void)
{
    return TRUE;
}

/**
 * Save state to file
 * Modified from upstream to add fsync for data durability
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
            
            errmsg = write_state(f, fd);
            g_io_channel_flush(f, NULL);
            g_io_channel_unref(f);
            
            if (errmsg) {
                g_critical("failed writing state to %s, ignoring: %s", tmpfile, errmsg);
                g_free(errmsg);
                close(fd);
                unlink(tmpfile);
            } else {
                /* fsync to ensure data durability before rename */
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
    
    /* Clean up bad exes once in a while */
    g_hash_table_foreach_remove(kp_state->bad_exes, (GHRFunc)true_func, NULL);
}

/**
 * Free state memory
 * (VERBATIM from upstream preload_state_free)
 */
void kp_state_free(void)
{
    g_message("freeing state memory begin");
    g_hash_table_destroy(kp_state->bad_exes);
    kp_state->bad_exes = NULL;
    g_hash_table_destroy(kp_state->exes);
    kp_state->exes = NULL;
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
 * (VERBATIM from upstream preload_state_dump_log)
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
 * STATE PERIODIC TASKS (VERBATIM from upstream)
 * ======================================================================== */

static gboolean kp_state_tick(gpointer data);

/* Tick2: Update model after half cycle delay (VERBATIM from upstream) */
static gboolean
kp_state_tick2(gpointer data)
{
    if (kp_state->model_dirty) {
        g_debug("state updating begin");
        kp_spy_update_model(data);
        kp_state->model_dirty = FALSE;
        g_debug("state updating end");
    }
    
    /* Increase time and reschedule */
    kp_state->time += (kp_conf->model.cycle + 1) / 2;
    g_timeout_add_seconds((kp_conf->model.cycle + 1) / 2, kp_state_tick, data);
    return FALSE;
}

/* Tick: Scan and predict (VERBATIM from upstream) */
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
        g_debug("state predicting begin");
        kp_prophet_predict(data);
        g_debug("state predicting end");
    }
    
    /* Increase time and reschedule */
    kp_state->time += kp_conf->model.cycle / 2;
    g_timeout_add_seconds(kp_conf->model.cycle / 2, kp_state_tick2, data);
    return FALSE;
}

static const char *autosave_statefile;

/* Autosave callback (VERBATIM from upstream) */
static gboolean
kp_state_autosave(void)
{
    kp_state_save(autosave_statefile);
    
    g_timeout_add_seconds(kp_conf->system.autosave, (GSourceFunc)kp_state_autosave, NULL);
    return FALSE;
}

/**
 * Start state periodic tasks
 * (VERBATIM from upstream preload_state_run)
 */
void kp_state_run(const char *statefile)
{
    g_timeout_add(0, kp_state_tick, NULL);
    if (statefile) {
        autosave_statefile = statefile;
        g_timeout_add_seconds(kp_conf->system.autosave, (GSourceFunc)kp_state_autosave, NULL);
    }
}
