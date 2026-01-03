/* config.c - Configuration implementation for Preheat
 *
 * Based on preload 0.6.4 conf.c (VERBATIM logic)
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: Configuration Management
 * =============================================================================
 *
 * This module handles loading, validating, and providing access to daemon
 * configuration. It uses the X-Macro pattern via confkeys.h to avoid
 * duplicating configuration key definitions.
 *
 * CONFIGURATION FLOW:
 *   1. kp_config_load() called at startup with path to preheat.conf
 *   2. Defaults set via set_default_conf() 
 *   3. INI file parsed using GLib's GKeyFile
 *   4. Values validated and clamped to valid ranges
 *   5. Manual apps list loaded from separate file if configured
 *   6. Global kp_conf singleton populated for use by other modules
 *
 * RELOAD SUPPORT:
 *   On SIGHUP, kp_config_load() can be called again to reload configuration
 *   without restarting the daemon. Old string values are properly freed.
 *
 * FILE FORMAT:
 *   Standard INI format with [model] and [system] sections.
 *   See preheat.conf.5 man page for full documentation.
 *
 * =============================================================================
 */

#include "common.h"
#include "../utils/logging.h"
#include "../state/state.h"
#include "config.h"
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

/* ========================================================================
 * BINARY PATH RESOLUTION - Security-Hardened
 * ========================================================================
 *
 * Resolves user-provided paths to actual ELF binaries, handling:
 *   - Symlinks → followed to real path
 *   - Shell wrappers → parsed for 'exec' statements
 *   - Interpreter scripts → returns the interpreter
 *
 * SECURITY MODEL:
 *   - Only trusted paths are accepted (usr, opt, usr/local prefixes)
 *   - All file operations use O_NOFOLLOW after initial realpath()
 *   - File size limits prevent resource exhaustion
 *   - All paths are canonicalized before use
 *
 * ======================================================================== */

/* Trusted path prefixes - files outside these are rejected */
static const char * const TRUSTED_PREFIXES[] = {
    "/usr/bin/",
    "/usr/sbin/",
    "/usr/lib/",
    "/usr/lib64/",
    "/usr/libexec/",
    "/usr/local/bin/",
    "/usr/local/lib/",
    "/usr/share/",
    "/opt/",
    NULL
};

/* Maximum script size to parse (prevent resource exhaustion)
 * 64KB: Largest reasonable shell wrapper in practice.
 * Sufficient for all legitimate wrappers, prevents DoS from
 * maliciously large files or accidental binary reads. */
#define MAX_SCRIPT_SIZE (64 * 1024)

/* Maximum lines to scan in script
 * 100 lines: Shell wrappers are typically 5-20 lines.
 * Hard limit prevents infinite loops on malformed files. */
#define MAX_SCRIPT_LINES 100

/**
 * Check if a path is in a trusted location
 * SECURITY: Prevents loading arbitrary binaries from /tmp, /home, etc.
 */
static gboolean
is_trusted_path(const char *path)
{
    if (!path || path[0] != '/') {
        return FALSE;
    }
    
    for (int i = 0; TRUSTED_PREFIXES[i] != NULL; i++) {
        if (g_str_has_prefix(path, TRUSTED_PREFIXES[i])) {
            return TRUE;
        }
    }
    
    return FALSE;
}

/**
 * Check if file is an ELF binary using file descriptor
 * SECURITY: Uses fd to prevent TOCTOU race
 */
static gboolean
is_elf_binary_fd(int fd)
{
    unsigned char magic[4];
    
    /* Seek to start */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return FALSE;
    }
    
    if (read(fd, magic, 4) != 4) {
        return FALSE;
    }
    
    return (magic[0] == 0x7f && magic[1] == 'E' && 
            magic[2] == 'L' && magic[3] == 'F');
}

/**
 * Check if file is an ELF binary (path-based, for convenience)
 */
static gboolean
is_elf_binary(const char *path)
{
    int fd;
    gboolean result;
    
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        return FALSE;
    }
    
    result = is_elf_binary_fd(fd);
    close(fd);
    return result;
}

/**
 * Parse a shell script to find the executed binary
 * SECURITY: Only extracts from trusted exec patterns
 * 
 * Handles patterns like:
 *   exec firefox-esr "$@"
 *   exec /usr/lib/foo/bin "$@"
 *   exec "$BROWSER" "$@"  (skipped - variable)
 */
static char *
parse_script_for_exec(const char *script_path)
{
    g_autofree char *contents = NULL;
    gsize length;
    GError *error = NULL;
    char *result = NULL;
    
    /* Read file with size limit */
    if (!g_file_get_contents(script_path, &contents, &length, &error)) {
        g_debug("Cannot read script %s: %s", script_path, error->message);
        g_error_free(error);
        return NULL;
    }
    
    if (length > MAX_SCRIPT_SIZE) {
        g_debug("Script too large (%zu bytes), skipping: %s", length, script_path);
        return NULL;
    }
    
    /* Split into lines and scan */
    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    int line_count = 0;
    
    for (int i = 0; lines[i] && line_count < MAX_SCRIPT_LINES; i++, line_count++) {
        char *line = g_strstrip(lines[i]);
        
        /* Skip comments and empty lines */
        if (!*line || line[0] == '#') continue;
        
        /* Look for exec command */
        char *exec_pos = strstr(line, "exec ");
        if (!exec_pos) continue;
        
        /* Extract the command after 'exec ' */
        char *cmd_start = exec_pos + 5;
        while (*cmd_start && g_ascii_isspace(*cmd_start)) cmd_start++;
        
        /* Skip if it's a variable ($FOO) or conditional */
        if (*cmd_start == '$' || *cmd_start == '[' || *cmd_start == '-') {
            continue;
        }
        
        /* Extract command token using GLib (safe) */
        g_auto(GStrv) tokens = g_strsplit_set(cmd_start, " \t\"'", 2);
        if (!tokens || !tokens[0] || !*tokens[0]) {
            continue;
        }
        
        char *binary = tokens[0];
        
        /* Skip shell builtins */
        if (strcmp(binary, "test") == 0 || strcmp(binary, "echo") == 0 ||
            strcmp(binary, "true") == 0 || strcmp(binary, "false") == 0) {
            continue;
        }
        
        /* Resolve path */
        g_autofree char *candidate = NULL;
        if (binary[0] == '/') {
            candidate = g_strdup(binary);
        } else {
            candidate = g_find_program_in_path(binary);
        }
        
        if (!candidate) continue;
        
        /* Canonicalize */
        char canonical[PATH_MAX];
        if (!realpath(candidate, canonical)) {
            continue;
        }
        
        /* SECURITY: Verify resolved path is trusted */
        if (!is_trusted_path(canonical)) {
            g_warning("Resolved binary not in trusted path: %s -> %s", 
                      binary, canonical);
            continue;
        }
        
        /* Verify it's actually an ELF */
        if (is_elf_binary(canonical)) {
            result = g_strdup(canonical);
            break;
        }
    }
    
    return result;
}

/**
 * Extract interpreter from shebang line
 * For Python/Ruby/Node scripts, preload the interpreter
 */
static char *
extract_interpreter(const char *script_path)
{
    FILE *fp;
    char line[PATH_MAX + 16];  /* #!/path + args */
    char *interp = NULL;
    
    fp = fopen(script_path, "r");
    if (!fp) return NULL;
    
    /* Read first line only */
    if (fgets(line, sizeof(line), fp)) {
        if (g_str_has_prefix(line, "#!")) {
            char *start = line + 2;
            start = g_strstrip(start);
            
            /* Handle "#!/usr/bin/env python3" */
            if (g_str_has_prefix(start, "/usr/bin/env ")) {
                start += 13;
                start = g_strstrip(start);
                
                /* Get first word (interpreter name) */
                g_auto(GStrv) parts = g_strsplit(start, " ", 2);
                if (parts && parts[0]) {
                    interp = g_find_program_in_path(parts[0]);
                }
            } else {
                /* Direct path: #!/usr/bin/python3 */
                g_auto(GStrv) parts = g_strsplit(start, " ", 2);
                if (parts && parts[0]) {
                    interp = g_strdup(parts[0]);
                }
            }
        }
    }
    
    fclose(fp);
    
    /* Validate interpreter path */
    if (interp) {
        char canonical[PATH_MAX];
        if (realpath(interp, canonical)) {
            /* SECURITY: Verify it's in a trusted location */
            if (!is_trusted_path(canonical)) {
                g_warning("Interpreter resolved to untrusted path: %s -> %s (skipping)",
                         interp, canonical);
                g_free(interp);
                return NULL;
            }
            
            /* AUDIT FIX M-2: Verify resolved binary is executable */
            if (access(canonical, X_OK) != 0) {
                g_warning("Interpreter not executable: %s -> %s (skipping)", 
                         interp, canonical);
                g_free(interp);
                return NULL;
            }
            
            /* Verify it's actually an ELF */
            if (is_elf_binary(canonical)) {
                g_free(interp);
                return g_strdup(canonical);
            }
        } else {
            g_warning("Cannot canonicalize interpreter path: %s (skipping)", interp);
        }
        g_free(interp);
    }
    
    return NULL;
}

/**
 * Resolve a path to its actual ELF binary
 * 
 * SECURITY-HARDENED:
 *   - Validates path is in trusted locations
 *   - Uses O_NOFOLLOW after symlink resolution
 *   - Prevents path traversal attacks
 *   - Limits resource usage
 *
 * @param path  User-provided path (may be symlink, wrapper, or direct)
 * @return      Newly allocated path to ELF binary, or NULL if unresolvable
 */
char *
resolve_binary_path(const char *path)
{
    char resolved[PATH_MAX];
    struct stat st;
    int fd;
    
    if (!path || !*path || path[0] != '/') {
        g_debug("Invalid path (must be absolute): %s", path ? path : "(null)");
        return NULL;
    }
    
    /* Step 1: Resolve symlinks to get canonical path */
    if (!realpath(path, resolved)) {
        g_debug("Cannot resolve path %s: %s", path, strerror(errno));
        return NULL;
    }
    
    /* Step 2: SECURITY - Verify path is in trusted location */
    if (!is_trusted_path(resolved)) {
        g_warning("Rejecting untrusted path: %s (resolved from %s)", resolved, path);
        return NULL;
    }
    
    /* Step 3: Open file with O_NOFOLLOW to prevent TOCTOU */
    fd = open(resolved, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        g_debug("Cannot open %s: %s", resolved, strerror(errno));
        return NULL;
    }
    
    /* Step 4: Stat using fd (TOCTOU-safe) */
    if (fstat(fd, &st) < 0) {
        close(fd);
        g_debug("Cannot stat %s: %s", resolved, strerror(errno));
        return NULL;
    }
    
    /* Step 5: Must be regular file */
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        g_debug("Not a regular file: %s", resolved);
        return NULL;
    }
    
    /* Step 6: Size sanity check */
    if (st.st_size < 64) {
        close(fd);
        g_debug("File too small to be ELF: %s (%ld bytes)", resolved, (long)st.st_size);
        return NULL;
    }
    
    /* Step 7: Check if it's an ELF binary */
    if (is_elf_binary_fd(fd)) {
        close(fd);
        return g_strdup(resolved);
    }
    
    close(fd);
    
    /* Step 8: Not ELF - try to parse as script wrapper */
    g_debug("Attempting to parse script wrapper: %s", resolved);
    
    char *real_binary = parse_script_for_exec(resolved);
    if (real_binary) {
        g_message("Resolved script wrapper: %s -> %s", path, real_binary);
        return real_binary;
    }
    
    /* Step 9: Try extracting interpreter */
    char *interpreter = extract_interpreter(resolved);
    if (interpreter) {
        g_message("Using interpreter for script: %s -> %s", path, interpreter);
        return interpreter;
    }
    
    g_warning("Cannot resolve %s to ELF binary", path);
    return NULL;
}

/* 
 * Global configuration singleton.
 * Allocated as array of 1 element so kp_conf can be used like a pointer:
 * kp_conf->model.cycle instead of (&kp_conf)->model.cycle
 */
kp_conf_t kp_conf[1];

/**
 * Load manual apps from whitelist file
 * 
 * This function reads a user-specified file containing absolute paths to
 * applications that should ALWAYS be preloaded, regardless of prediction scores.
 * Useful for guaranteeing fast startup of frequently-used tools.
 *
 * FILE FORMAT:
 *   - One absolute path per line (must start with /)
 *   - Lines starting with # are comments
 *   - Empty lines are ignored
 *   - Trailing whitespace is stripped
 *
 * EXAMPLE FILE:
 *   # My frequently used tools
 *   /usr/bin/firefox
 *   /usr/bin/code
 *
 * @param conf Configuration structure to populate with loaded apps
 *
 * SIDE EFFECTS:
 *   - Frees any previously loaded manual_apps_loaded array
 *   - Updates conf->system.manual_apps_loaded and manual_apps_count
 *   - Logs message on success, debug on file not found
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
        g_debug("No manual apps file configured");
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

        /* Resolve to actual binary (security-checked) */
        char *resolved = resolve_binary_path(p);
        if (resolved) {
            if (strcmp(resolved, p) != 0) {
                g_message("Manual app resolved: %s -> %s", p, resolved);
            }
            g_ptr_array_add(apps, resolved);  /* Takes ownership */
        } else {
            g_warning("Skipping unresolvable manual app: %s", p);
        }
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
 * Parse semicolon-separated pattern list
 *
 * Used for excluded_patterns and user_app_paths configuration values.
 * Splits on semicolon, strips whitespace, expands ~ to home directory.
 *
 * @param value       Raw config value (semicolon-separated)
 * @param out_list    Output: NULL-terminated array of pattern strings
 * @param out_count   Output: Number of patterns in array
 */
static void
parse_pattern_list(const char *value, char ***out_list, int *out_count)
{
    GPtrArray *arr;
    char **tokens;

    *out_list = NULL;
    *out_count = 0;

    if (!value || !*value) {
        return;
    }

    tokens = g_strsplit(value, ";", -1);
    arr = g_ptr_array_new();

    for (char **p = tokens; *p; p++) {
        char *pattern = g_strstrip(*p);  /* Remove whitespace */

        if (!*pattern) {
            continue;  /* Skip empty entries */
        }

        /* Expand ~ to home directory */
        if (pattern[0] == '~' && (pattern[1] == '/' || pattern[1] == '\0')) {
            const char *home = g_get_home_dir();
            if (home) {
                char *expanded = g_strdup_printf("%s%s", home, pattern + 1);
                g_ptr_array_add(arr, expanded);
            } else {
                g_warning("Cannot expand ~: HOME not set, using pattern as-is");
                g_ptr_array_add(arr, g_strdup(pattern));
            }
        } else {
            g_ptr_array_add(arr, g_strdup(pattern));
        }
    }

    g_strfreev(tokens);

    /* Convert to NULL-terminated array */
    g_ptr_array_add(arr, NULL);
    *out_list = (char **)g_ptr_array_free(arr, FALSE);

    /* Count patterns */
    if (*out_list) {
        for (char **p = *out_list; *p; p++) {
            (*out_count)++;
        }
    }
}

/**
 * Set default configuration values
 * 
 * Uses the X-Macro pattern: includes confkeys.h with a custom confkey() macro
 * that extracts the default value for each parameter and assigns it.
 *
 * This technique ensures defaults are defined in ONE place (confkeys.h) and
 * automatically applied here without risk of mismatch.
 *
 * @param conf Configuration structure to initialize with defaults
 */
static void
set_default_conf(kp_conf_t *conf)
{
#define true TRUE
#define false FALSE
#define default_integer(def, unit) (unit * def)
#define default_boolean(def, unit) def
#define default_enum(def, unit) def
#define default_string(def, unit) (def ? g_strdup(def) : NULL)
#define default_string_list(def, unit) NULL
#define confkey(grp, type, key, def, unit) \
    conf->grp.key = default_##type (def, unit);
#include "confkeys.h"
#undef confkey
#undef default_string

    /* Initialize runtime fields */
    conf->system.manual_apps_loaded = NULL;
    conf->system.manual_apps_count = 0;
    
    /* Initialize pattern list runtime fields */
    conf->system.excluded_patterns_list = NULL;
    conf->system.excluded_patterns_count = 0;
    conf->system.user_app_paths_list = NULL;
    conf->system.user_app_paths_count = 0;
}

/* Forward declaration for family config loading */
static void load_families_from_config(GKeyFile *keyfile);

/**
 * Load configuration from file
 *
 * This is the main entry point for loading configuration. It:
 *   1. Sets all configuration values to defaults
 *   2. Parses the INI file (if provided) and overlays values
 *   3. Validates values and clamps to acceptable ranges
 *   4. Frees old configuration and installs new one
 *   5. Loads manual apps list if configured
 *
 * ERROR HANDLING:
 *   - If 'fail' is TRUE: exits process on any error (for startup)
 *   - If 'fail' is FALSE: logs warning and continues with defaults (for reload)
 *
 * @param conffile Path to preheat.conf file (can be NULL for defaults-only)
 * @param fail     If TRUE, exit on error; if FALSE, warn and continue
 *
 * THREAD SAFETY:
 *   Not thread-safe. Should only be called from main thread during
 *   initialization or from signal handler context.
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

        /* Load family definitions */
        load_families_from_config(f);
        g_debug("configuration loading complete");
        
        g_key_file_free(f);
    }

    /* Free old configuration string values */
    g_free(kp_conf->system.mapprefix_raw);
    g_strfreev(kp_conf->system.mapprefix);
    g_free(kp_conf->system.exeprefix_raw);
    g_strfreev(kp_conf->system.exeprefix);
    g_free(kp_conf->system.manualapps);
    g_strfreev(kp_conf->system.manual_apps_loaded);
    
    /* Free old pattern lists */
    g_free(kp_conf->system.excluded_patterns);
    g_strfreev(kp_conf->system.excluded_patterns_list);
    g_free(kp_conf->system.user_app_paths);
    g_strfreev(kp_conf->system.user_app_paths_list);

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
        kp_conf->model.cycle = 90;
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

    /* Parse pattern lists */
    parse_pattern_list(kp_conf->system.excluded_patterns,
                       &kp_conf->system.excluded_patterns_list,
                       &kp_conf->system.excluded_patterns_count);
    
    parse_pattern_list(kp_conf->system.user_app_paths,
                       &kp_conf->system.user_app_paths_list,
                       &kp_conf->system.user_app_paths_count);
    
    /* Parse prefix strings into arrays (semicolon-separated) */
    if (kp_conf->system.mapprefix_raw && *kp_conf->system.mapprefix_raw) {
        kp_conf->system.mapprefix = g_strsplit(kp_conf->system.mapprefix_raw, ";", -1);
        int count = 0;
        for (char **p = kp_conf->system.mapprefix; p && *p; p++) count++;
        g_message("Parsed %d map prefixes from config", count);
    }
    
    if (kp_conf->system.exeprefix_raw && *kp_conf->system.exeprefix_raw) {
        kp_conf->system.exeprefix = g_strsplit(kp_conf->system.exeprefix_raw, ";", -1);
        int count = 0;
        for (char **p = kp_conf->system.exeprefix; p && *p; p++) count++;
        g_message("Parsed %d exe prefixes from config", count);
    }
    
    if (kp_conf->system.excluded_patterns_count > 0) {
        g_message("Loaded %d exclusion patterns for observation pool",
                  kp_conf->system.excluded_patterns_count);
    }
    
    if (kp_conf->system.user_app_paths_count > 0) {
        g_message("Monitoring %d user app directories for priority pool",
                  kp_conf->system.user_app_paths_count);
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

/**
 * Load family definitions from config file
 * Parses [families] section for user-defined application families
 * 
 * FORMAT: family_name = exe1;exe2;exe3
 * EXAMPLE:
 *   [families]
 *   chrome = /usr/bin/chrome;/usr/lib/chrome/chrome-sandbox
 *   firefox = /usr/bin/firefox;/usr/lib/firefox/plugin-container
 */
static void
load_families_from_config(GKeyFile *keyfile)
{
    gchar **keys;
    gsize num_keys;
    GError *error = NULL;
    
    if (!keyfile) return;
    
    /* BUG 2 FIX: Guard against NULL state during early startup */
    if (!kp_state->app_families) {
        g_debug("Deferring family loading - state not initialized");
        return;
    }
    
    keys = g_key_file_get_keys(keyfile, "families", &num_keys, &error);
    if (!keys) {
        if (error && error->code != G_KEY_FILE_ERROR_GROUP_NOT_FOUND) {
            g_debug("Error reading [families] section: %s", error->message);
        }
        g_clear_error(&error);
        return;
    }
    
    g_message("Loading family definitions from config...");
    
    for (gsize i = 0; i < num_keys; i++) {
        gchar *value = g_key_file_get_string(keyfile, "families", keys[i], &error);
        if (!value) {
            g_warning("Cannot read family '%s': %s", keys[i], error->message);
            g_clear_error(&error);
            continue;
        }
        
        /* Parse semicolon-separated member paths */
        gchar **members = g_strsplit(value, ";", -1);
        int member_count = 0;
        
        /* Create family */
        kp_app_family_t *family = kp_family_new(keys[i], FAMILY_CONFIG);
        
        for (gchar **m = members; *m; m++) {
            gchar *member = g_strstrip(*m);
            if (*member && member[0] == '/') {  /* Must be absolute path */
                kp_family_add_member(family, member);
                member_count++;
            }
        }
        
        if (member_count > 0) {
            g_hash_table_insert(kp_state->app_families, g_strdup(keys[i]), family);
            g_message("  Loaded family '%s' with %d members", keys[i], member_count);
        } else {
            kp_family_free(family);
            g_warning("  Family '%s' has no valid members, skipping", keys[i]);
        }
        
        g_strfreev(members);
        g_free(value);
    }
    
    g_strfreev(keys);
}
