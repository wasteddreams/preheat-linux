/* config.c - Configuration implementation for Preheat
 *
 * Based on preload 0.6.4 conf.c (VERBATIM logic)
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
#include "config.h"

#include <time.h>

/* Global configuration singleton */
kp_conf_t kp_conf[1];

/**
 * Load manual apps from whitelist file
 * File format: one absolute path per line, # for comments
 */
static void
load_manual_apps_file(kp_conf_t *conf)
{
    FILE *fp;
    char line[PATH_MAX];
    GPtrArray *apps;
    
    /* Free old list */
    if (conf->system.manual_apps_loaded) {
        g_strfreev(conf->system.manual_apps_loaded);
        conf->system.manual_apps_loaded = NULL;
        conf->system.manual_apps_count = 0;
    }
    
    if (!conf->system.manualapps || !*conf->system.manualapps) {
        return;
    }
    
    fp = fopen(conf->system.manualapps, "r");
    if (!fp) {
        g_debug("Manual apps file not found: %s", conf->system.manualapps);
        return;
    }
    
    apps = g_ptr_array_new();
    
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *end;
        
        /* Skip leading whitespace */
        while (*p && g_ascii_isspace(*p)) p++;
        
        /* Skip empty lines and comments */
        if (!*p || *p == '#' || *p == '\n') continue;
        
        /* Remove trailing newline/whitespace */
        end = p + strlen(p) - 1;
        while (end > p && g_ascii_isspace(*end)) *end-- = '\0';
        
        /* Must be absolute path */
        if (*p != '/') {
            g_warning("Manual app must be absolute path, skipping: %s", p);
            continue;
        }
        
        g_ptr_array_add(apps, g_strdup(p));
    }
    
    fclose(fp);
    
    /* Convert to NULL-terminated array */
    g_ptr_array_add(apps, NULL);
    conf->system.manual_apps_loaded = (char **)g_ptr_array_free(apps, FALSE);
    
    /* Count apps */
    conf->system.manual_apps_count = 0;
    if (conf->system.manual_apps_loaded) {
        for (char **p = conf->system.manual_apps_loaded; *p; p++) {
            conf->system.manual_apps_count++;
        }
    }
    
    if (conf->system.manual_apps_count > 0) {
        g_message("Loaded %d manual apps from %s", 
                  conf->system.manual_apps_count, conf->system.manualapps);
    }
}

/**
 * Set default configuration values
 * (VERBATIM from upstream set_default_conf)
 */
static void
set_default_conf(kp_conf_t *conf)
{
#define true TRUE
#define false FALSE
#define default_integer(def, unit) (unit * def)
#define default_boolean(def, unit) def
#define default_enum(def, unit) def
#define default_string(def, unit) NULL
#define default_string_list(def, unit) NULL
#define confkey(grp, type, key, def, unit) \
    conf->grp.key = default_##type (def, unit);
#include "confkeys.h"
#undef confkey
#undef default_string
    
    /* Initialize runtime fields */
    conf->system.manual_apps_loaded = NULL;
    conf->system.manual_apps_count = 0;
}

/**
 * Load configuration from file
 * (VERBATIM from upstream preload_conf_load)
 */
void
kp_config_load(const char *conffile, gboolean fail)
{
    GKeyFile *f;
    GError *e = NULL;
    kp_conf_t newconf;
    GLogLevelFlags flags = fail ? G_LOG_LEVEL_ERROR : G_LOG_LEVEL_CRITICAL;

    /* Set defaults first */
    set_default_conf(&newconf);
    
    if (conffile && *conffile) {
        kp_conf_t dummyconf;
        
        g_message("loading configuration from %s", conffile);
        
        f = g_key_file_new();
        if (!g_key_file_load_from_file(f, conffile, G_KEY_FILE_NONE, &e)) {
            g_log(G_LOG_DOMAIN, flags, 
                  "failed loading configuration from %s: %s", 
                  conffile, e->message);
            g_error_free(e);
            return;
        }

        /* Load all keys using macro pattern */
#define get_integer(grp, key, unit) (unit * g_key_file_get_integer(f, grp, key, &e))
#define get_enum(grp, key, unit) (g_key_file_get_integer(f, grp, key, &e))
#define get_boolean(grp, key, unit) g_key_file_get_boolean(f, grp, key, &e)
#define get_string(grp, key, unit) g_key_file_get_string(f, grp, key, &e)
#define get_string_list(grp, key, unit) g_key_file_get_string_list(f, grp, key, NULL, &e)
#define STRINGIZE_IMPL(x) #x
#define STRINGIZE(x) STRINGIZE_IMPL(x)
#define confkey(grp, type, key, def, unit) \
    dummyconf.grp.key = get_##type (STRINGIZE(grp), STRINGIZE(key), unit); \
    if (!e) \
        newconf.grp.key = dummyconf.grp.key; \
    else if (e->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) { \
        g_log(G_LOG_DOMAIN, flags, "failed loading config key %s.%s: %s", \
              STRINGIZE(grp), STRINGIZE(key), e->message); \
        g_error_free(e); \
        g_key_file_free(f); \
        return; \
    } else { \
        g_error_free(e); \
        e = NULL; \
    }
#include "confkeys.h"
#undef confkey
#undef STRINGIZE
#undef STRINGIZE_IMPL
#undef get_string
        
        g_key_file_free(f);
        g_debug("configuration loading complete");
    }
    
    /* Free old configuration string values */
    g_strfreev(kp_conf->system.mapprefix);
    g_strfreev(kp_conf->system.exeprefix);
    g_free(kp_conf->system.manualapps);
    g_strfreev(kp_conf->system.manual_apps_loaded);
    
#ifdef ENABLE_PREHEAT_EXTENSIONS
    g_free(kp_conf->preheat.manual_apps_list);
    g_free(kp_conf->preheat.blacklist);
#endif
    
    /* Copy new configuration */
    *kp_conf = newconf;
    
    /* Validate configuration values */
    if (kp_conf->model.cycle < 5 || kp_conf->model.cycle > 300) {
        g_warning("Invalid cycle value %d (must be 5-300), using default 20", 
                  kp_conf->model.cycle);
        kp_conf->model.cycle = 20;
    }
    
    if (kp_conf->model.memfree < 0 || kp_conf->model.memfree > 100) {
        g_warning("Invalid memfree value %d (must be 0-100%%), using default 50", 
                  kp_conf->model.memfree);
        kp_conf->model.memfree = 50;
    }
    
    if (kp_conf->system.maxprocs < 0 || kp_conf->system.maxprocs > 100) {
        g_warning("Invalid maxprocs value %d (must be 0-100), using default 30", 
                  kp_conf->system.maxprocs);
        kp_conf->system.maxprocs = 30;
    }
    
    if (kp_conf->system.sortstrategy < 0 || kp_conf->system.sortstrategy > 3) {
        g_warning("Invalid sortstrategy value %d (must be 0-3), using default 3", 
                  kp_conf->system.sortstrategy);
        kp_conf->system.sortstrategy = 3;
    }
    
    if (kp_conf->model.minsize < 0) {
        g_warning("Invalid min size value %d (must be >= 0), using default 2000000", 
                  kp_conf->model.minsize);
        kp_conf->model.minsize = 2000000;
    }
    
    /* Load manual apps from file */
    load_manual_apps_file(kp_conf);
}

/**
 * Dump configuration to log
 * (VERBATIM from upstream preload_conf_dump_log)
 */
void
kp_config_dump_log(void)
{
    const char *curgrp = "";
    time_t curtime;
    char *timestr;
    
    g_message("configuration dump requested");
    
    curtime = time(NULL);
    timestr = ctime(&curtime);
    fprintf(stderr, "#\n");
    fprintf(stderr, "# loaded configuration at %s", timestr);
    
#define print_integer(v, unit) \
    fprintf(stderr, "%d", v / unit);
#define print_enum(v, unit) \
    fprintf(stderr, "%d", v);
#define print_boolean(v, unit) \
    fprintf(stderr, "%s", v ? "true" : "false");
#define print_string(v, unit) \
    fprintf(stderr, "%s", v ? v : "(null)");
#define print_string_list(v, unit) G_STMT_START { \
    char **p = v; \
    if (p) \
        fprintf(stderr, "%s", *p++); \
    while (p && *p) \
        fprintf(stderr, ";%s", *p++); \
} G_STMT_END
#define STRINGIZE_IMPL(x) #x
#define STRINGIZE(x) STRINGIZE_IMPL(x)
#define confkey(grp, type, key, def, unit) \
    if (strcmp(STRINGIZE(grp), curgrp)) \
        fprintf(stderr, "\n[%s]\n", curgrp = STRINGIZE(grp)); \
    fprintf(stderr, "%s = ", STRINGIZE(key)); \
    print_##type (kp_conf->grp.key, unit); \
    fprintf(stderr, "\n");
#include "confkeys.h"
#undef confkey
#undef STRINGIZE
#undef STRINGIZE_IMPL
    
    fprintf(stderr, "# loaded configuration - end\n");
    fprintf(stderr, "#\n");
    
    g_debug("configuration dump complete");
}
