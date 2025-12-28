/* seeding.c - Smart first-run seeding implementation
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * Populates initial state from:
 * - XDG recently-used files
 * - Desktop file access times  
 * - Shell history (bash/zsh)
 *
 * Provides immediate value on first daemon start.
 */

#include "common.h"
#include "seeding.h"
#include "../state/state.h"
#include "logging.h"

#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "desktop.h"

/* Seed from XDG recently-used files */
static int
kp_seed_from_xdg_recent(void)
{
    const char *home = g_get_home_dir();
    char xbel_path[PATH_MAX];
    FILE *fp;
    char line[2048];
    int seeded = 0;
    
    /* XDG recently-used is at ~/.local/share/recently-used.xbel */
    snprintf(xbel_path, sizeof(xbel_path), "%s/.local/share/recently-used.xbel", home);
    fp = fopen(xbel_path, "r");
    if (!fp) {
        g_debug("XDG recently-used file not found: %s", xbel_path);
        return 0;
    }
    
    /* Simple line-by-line parser looking for application exec lines */
    while (fgets(line, sizeof(line), fp)) {
        char *exec_start = strstr(line, "exec=\"");
        
        if (exec_start) {
            exec_start += 6;  /* Skip 'exec="' */
            char *exec_end = strchr(exec_start, '"');
            if (!exec_end) continue;
            
            char exec_line[PATH_MAX];
            size_t len = exec_end - exec_start;
            if (len >= sizeof(exec_line)) continue;
            
            strncpy(exec_line, exec_start, len);
            exec_line[len] = '\0';
            
            /* Extract first word (the actual binary) */
            char *app_path = strtok(exec_line, " ");
            if (!app_path || app_path[0] != '/') continue;
            
            /* Check if file exists */
            if (access(app_path, X_OK) != 0) continue;
            
            /* Calculate score based on recency */
            double score = 5.0;  /* Base score for being in recently-used */
            
            /* Check if exe already exists */
            kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, app_path);
            if (!exe) {
                exe = kp_exe_new(app_path, FALSE, NULL);
                exe->pool = POOL_PRIORITY;  /* Recently used = priority */
                kp_state_register_exe(exe, FALSE);
            }
            
            exe->weighted_launches += score;
            exe->raw_launches += 1;
            seeded++;
        }
    }
    
    fclose(fp);
    g_debug("Seeded %d apps from XDG recently-used", seeded);
    return seeded;
}

/* Seed from desktop file modification times */
static int
kp_seed_from_desktop_times(void)
{
    const char *desktop_dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        NULL
    };
    int seeded = 0;
    time_t now = time(NULL);
    
    /* Also check user's local applications */
    const char *home = g_get_home_dir();
    char user_apps[PATH_MAX];
    snprintf(user_apps, sizeof(user_apps), "%s/.local/share/applications", home);
    
    for (int d = 0; d < 4; d++) {
        const char *dir = (d < 2) ? desktop_dirs[d] : (d == 2 ? user_apps : NULL);
        if (!dir) break;
        
        DIR *dp = opendir(dir);
        if (!dp) continue;
        
        struct dirent *entry;
        while ((entry = readdir(dp))) {
            if (!g_str_has_suffix(entry->d_name, ".desktop")) continue;
            
            char desktop_path[PATH_MAX];
            struct stat st;
            snprintf(desktop_path, sizeof(desktop_path), "%s/%s", dir, entry->d_name);
            
            if (stat(desktop_path, &st) != 0) continue;
            
            /* Calculate age in days */
            double days_ago = (double)(now - st.st_mtime) / 86400.0;
            
            /* Skip very old files (> 180 days) */
            if (days_ago > 180) continue;
            
            /* Score with exponential decay: score = 3.0 * exp(-days/60) */
            double score = 3.0 * exp(-days_ago / 60.0);
            
            /* Parse desktop file to extract Exec= line */
            FILE *desktop_fp = fopen(desktop_path, "r");
            if (!desktop_fp) continue;
            
            char desktop_line[1024];
            char *exec_value = NULL;
            while (fgets(desktop_line, sizeof(desktop_line), desktop_fp)) {
                /* Skip comments and empty lines */
                if (desktop_line[0] == '#' || desktop_line[0] == '\n') continue;
                
                /* Look for Exec= line */
                if (strncmp(desktop_line, "Exec=", 5) == 0) {
                    exec_value = desktop_line + 5;
                    /* Remove newline */
                    char *newline = strchr(exec_value, '\n');
                    if (newline) *newline = '\0';
                    break;
                }
            }
            fclose(desktop_fp);
            
            if (!exec_value) continue;
            
            /* Extract binary path (first word, handle field codes like %u, %f) */
            char exec_copy[1024];
            strncpy(exec_copy, exec_value, sizeof(exec_copy) - 1);
            exec_copy[sizeof(exec_copy) - 1] = '\0';
            
            char *binary = strtok(exec_copy, " ");
            if (!binary) continue;
            
            /* Resolve to full path if needed */
            char full_path[PATH_MAX];
            if (binary[0] == '/') {
                /* Already absolute path */
                strncpy(full_path, binary, sizeof(full_path) - 1);
            } else {
                /* Search in PATH */
                snprintf(full_path, sizeof(full_path), "/usr/bin/%s", binary);
                if (access(full_path, X_OK) != 0) {
                    snprintf(full_path, sizeof(full_path), "/bin/%s", binary);
                    if (access(full_path, X_OK) != 0) {
                        continue;  /* Binary not found */
                    }
                }
            }
            
            /* FILTER: Skip shell wrapper scripts (e.g., kali-menu's exec-in-shell) */
            if (strstr(full_path, "exec-in-shell") ||
                strstr(full_path, "/usr/share/kali-menu/") ||
                strstr(full_path, "/usr/share/legion/")) {
                continue;  /* Skip wrapper scripts, we want the actual app */
            }
            
            /* Seed the app */
            kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, full_path);
            if (!exe) {
                exe = kp_exe_new(full_path,FALSE, NULL);
                exe->pool = POOL_PRIORITY;  /* Desktop apps = priority */
                kp_state_register_exe(exe, FALSE);
            }
            
            exe->weighted_launches += score;
            exe->raw_launches += 1;
            seeded++;
        }
        closedir(dp);
    }
    
    g_debug("Seeded %d apps from desktop file times", seeded);
    return seeded;
}

/* Seed from shell history */
static int
kp_seed_from_shell_history(void)
{
    const char *home = g_get_home_dir();
    const char *history_files[] = {
        "/.bash_history",
        "/.zsh_history",
        NULL
    };
    GHashTable *cmd_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int seeded = 0;
    
    for (int i = 0; history_files[i]; i++) {
        char history_path[PATH_MAX];
        FILE *fp;
        char line[1024];
        
        snprintf(history_path, sizeof(history_path), "%s%s", home, history_files[i]);
        fp = fopen(history_path, "r");
        if (!fp) continue;
        
        while (fgets(line, sizeof(line), fp)) {
            char *cmd = strtok(line, " \t\n");
            if (!cmd || cmd[0] == '#') continue;
            
            /* Skip common non-apps */
            if (strcmp(cmd, "cd") == 0 || strcmp(cmd, "ls") == 0 ||
                strcmp(cmd, "echo") == 0 || strcmp(cmd, "cat") == 0)
                continue;
            
            /* Count frequency */
            gpointer count = g_hash_table_lookup(cmd_counts, cmd);
            g_hash_table_insert(cmd_counts,  g_strdup(cmd),
                                GINT_TO_POINTER(GPOINTER_TO_INT(count) + 1));
        }
        fclose(fp);
    }
    
    /* Convert counts to state entries */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, cmd_counts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *cmd = key;
        int count = GPOINTER_TO_INT(value);
        char full_path[PATH_MAX];
        kp_exe_t *exe;
        
        /* Try to resolve command to full path */
        snprintf(full_path, sizeof(full_path), "/usr/bin/%s", cmd);
        if (access(full_path, X_OK) != 0) {
            snprintf(full_path, sizeof(full_path), "/bin/%s", cmd);
            if (access(full_path, X_OK) != 0) {
                continue;  /* Can't find executable */
            }
        }
        
        /* Check if exe already exists */
        exe = g_hash_table_lookup(kp_state->exes, full_path);
        if (!exe) {
            /* FILTER: Only seed apps with .desktop files (skip CLI tools) */
            if (!kp_desktop_has_file(full_path)) {
                continue;  /* Skip CLI tools like grep, ls, exec-in-shell */
            }
            exe = kp_exe_new(full_path, FALSE, NULL);
            exe->pool = POOL_PRIORITY;  /* Desktop apps are user-initiated */
            kp_state_register_exe(exe, FALSE);
        }
        
        /* Score: sqrt to prevent domination by very frequent commands */
        exe->weighted_launches = sqrt((double)count);
        exe->raw_launches = count;
        seeded++;
    }
    
    g_hash_table_destroy(cmd_counts);
    g_debug("Seeded %d apps from shell history", seeded);
    return seeded;
}

/* Seed from browser profile detection */
static int
kp_seed_from_browser_profiles(void)
{
    const char *home = g_get_home_dir();
    struct {
        const char *profile_path;
        const char *binary_path;
        const char *name;
    } browsers[] = {
        {".mozilla/firefox", "/usr/bin/firefox", "Firefox"},
        {".config/google-chrome", "/usr/bin/google-chrome", "Chrome"},
        {".config/chromium", "/usr/bin/chromium", "Chromium"},
        {".config/microsoft-edge", "/usr/bin/microsoft-edge", "Edge"},
        {".config/BraveSoftware/Brave-Browser", "/usr/bin/brave", "Brave"},
        {NULL, NULL, NULL}
    };
    int seeded = 0;
    time_t now = time(NULL);
    
    for (int i = 0; browsers[i].profile_path; i++) {
        char profile_full[PATH_MAX];
        struct stat st;
        
        snprintf(profile_full, sizeof(profile_full), "%s/%s", home, browsers[i].profile_path);
        
        /* Check if profile directory exists and was accessed recently */
        if (stat(profile_full, &st) == 0 && S_ISDIR(st.st_mode)) {
            double days_ago = (double)(now - st.st_mtime) / 86400.0;
            
            /* Only seed if used within last 30 days */
            if (days_ago <= 30) {
                /* Check if browser binary exists */
                if (access(browsers[i].binary_path, X_OK) == 0) {
                    kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, browsers[i].binary_path);
                    if (!exe) {
                        exe = kp_exe_new(browsers[i].binary_path, FALSE, NULL);
                        exe->pool = POOL_PRIORITY;
                        kp_state_register_exe(exe, FALSE);
                    }
                    
                    /* Score based on recency: 10.0 * exp(-days/15) */
                    double score = 10.0 * exp(-days_ago / 15.0);
                    exe->weighted_launches += score;
                    exe->raw_launches += 1;
                    seeded++;
                    
                    g_debug("Seeded browser: %s (profile age: %.1f days, score: %.2f)", 
                           browsers[i].name, days_ago, score);
                }
            }
        }
    }
    
    g_debug("Seeded %d browsers from profile detection", seeded);
    return seeded;
}

/* Seed common developer tools
 * NOTE: Currently disabled - most dev tools are CLI without .desktop files */
static int __attribute__((unused))
kp_seed_from_dev_tools(void)
{
    const char *dev_tools[] = {
        "/usr/bin/vim", "/usr/bin/nvim", "/usr/bin/emacs",
        "/usr/bin/git", "/usr/bin/make", "/usr/bin/gcc",
        "/usr/bin/python3", "/usr/bin/node", "/usr/bin/npm",
        "/usr/bin/docker", "/usr/bin/code", "/usr/bin/code-insiders",
        NULL
    };
    int seeded = 0;
    struct stat st;
    time_t now = time(NULL);
    
    for (int i = 0; dev_tools[i]; i++) {
        /* Check if tool exists and was accessed recently */
        if (stat(dev_tools[i], &st) == 0) {
            double days_ago = (double)(now - st.st_atime) / 86400.0;
            
            /* Only seed if accessed within last 60 days */
            if (days_ago <= 60) {
                kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, dev_tools[i]);
                if (!exe) {
                    exe = kp_exe_new(dev_tools[i], FALSE, NULL);
                    exe->pool = POOL_PRIORITY;
                    kp_state_register_exe(exe, FALSE);
                }
                
                /* Fixed score for dev tools */
                double score = 4.0;
                exe->weighted_launches += score;
                exe->raw_launches += 1;
                seeded++;
            }
        }
    }
    
    g_debug("Seeded %d developer tools", seeded);
    return seeded;
}

/**
 * Seed initial state from all available sources
 */
static int kp_seed_from_system_patterns(void);

void
kp_seed_from_sources(void)
{
    int total_seeded = 0;
    int by_source[6] = {0};
    
    g_message("=== Smart First-Run Seeding ===");
    g_message("Analyzing user data to populate initial state...");
    
    by_source[0] = kp_seed_from_xdg_recent();
    by_source[1] = kp_seed_from_desktop_times();
    by_source[2] = kp_seed_from_shell_history();
    by_source[3] = kp_seed_from_browser_profiles();
    /* Disabled: dev_tools are mostly CLI without .desktop files */
    by_source[4] = 0;  /* kp_seed_from_dev_tools(); */
    by_source[5] = kp_seed_from_system_patterns();
    
    for (int i = 0; i < 6; i++) {
        total_seeded += by_source[i];
    }
    
    if (total_seeded > 0) {
        g_message("Successfully seeded %d applications:", total_seeded);
        g_message("  • XDG recently-used: %d apps", by_source[0]);
        g_message("  • Desktop files: %d apps", by_source[1]);
        g_message("  • Shell history: %d apps", by_source[2]);
        g_message("  • Browser profiles: %d apps", by_source[3]);
        g_message("  • Developer tools: %d apps", by_source[4]);
        g_message("  • System defaults: %d apps", by_source[5]);
        g_message("Preheat is now ready with intelligent defaults!");
    } else {
        g_message("No seeding data available - will learn from your usage");
    }
    g_message("===============================");
}

/* Seed system-specific default apps based on desktop environment */
static int
kp_seed_from_system_patterns(void)
{
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    const char *session = g_getenv("DESKTOP_SESSION");
    int seeded = 0;
    
    /* Detect desktop environment */
    const char *de = desktop ? desktop : (session ? session : "unknown");
    g_debug("Detected desktop environment: %s", de);
    
    /* GNOME defaults */
    if (strstr(de, "GNOME") || strstr(de, "gnome")) {
        const char *gnome_apps[] = {
            "/usr/bin/nautilus",      /* File manager */
            "/usr/bin/gnome-terminal", /* Terminal */
            "/usr/bin/gnome-control-center", /* Settings */
            NULL
        };
        
        for (int i = 0; gnome_apps[i]; i++) {
            if (access(gnome_apps[i], X_OK) == 0) {
                kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, gnome_apps[i]);
                if (!exe) {
                    exe = kp_exe_new(gnome_apps[i], FALSE, NULL);
                    exe->pool = POOL_PRIORITY;
                    kp_state_register_exe(exe, FALSE);
                    exe->weighted_launches = 3.0;
                    exe->raw_launches = 1;
                    seeded++;
                }
            }
        }
    }
    
    /* KDE defaults */
    if (strstr(de, "KDE") || strstr(de, "kde") || strstr(de, "plasma")) {
        const char *kde_apps[] = {
            "/usr/bin/dolphin",       /* File manager */
            "/usr/bin/konsole",       /* Terminal */
            "/usr/bin/systemsettings", /* Settings */
            NULL
        };
        
        for (int i = 0; kde_apps[i]; i++) {
            if (access(kde_apps[i], X_OK) == 0) {
                kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, kde_apps[i]);
                if (!exe) {
                    exe = kp_exe_new(kde_apps[i], FALSE, NULL);
                    exe->pool = POOL_PRIORITY;
                    kp_state_register_exe(exe, FALSE);
                    exe->weighted_launches = 3.0;
                    exe->raw_launches = 1;
                    seeded++;
                }
            }
        }
    }
    
    /* XFCE defaults */
    if (strstr(de, "XFCE") || strstr(de, "xfce")) {
        const char *xfce_apps[] = {
            "/usr/bin/thunar",        /* File manager */
            "/usr/bin/xfce4-terminal", /* Terminal */
            NULL
        };
        
        for (int i = 0; xfce_apps[i]; i++) {
            if (access(xfce_apps[i], X_OK) == 0) {
                kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, xfce_apps[i]);
                if (!exe) {
                    exe = kp_exe_new(xfce_apps[i], FALSE, NULL);
                    exe->pool = POOL_PRIORITY;
                    kp_state_register_exe(exe, FALSE);
                    exe->weighted_launches = 3.0;
                    exe->raw_launches = 1;
                    seeded++;
                }
            }
        }
    }
    
    g_debug("Seeded %d system-specific apps for %s", seeded, de);
    return seeded;
}


/* Calculate confidence score for seeded app (0.0 to 1.0)
 * NOTE: Currently unused but reserved for future filtering/prioritization
 */
static double __attribute__((unused))
calculate_seed_confidence(double weighted_launches, int raw_launches, const char *source)
{
    double confidence = 0.5;  /* Base confidence */
    
    /* Higher weight = higher confidence */
    if (weighted_launches > 10.0) confidence = 0.9;
    else if (weighted_launches > 5.0) confidence = 0.8;
    else if (weighted_launches > 3.0) confidence = 0.7;
    else if (weighted_launches > 1.0) confidence = 0.6;
    
    /* Multiple raw launches increase confidence */
    if (raw_launches > 10) confidence += 0.05;
    else if (raw_launches > 5) confidence += 0.03;
    
    /* Certain sources are more reliable */
    if (strcmp(source, "browser") == 0) confidence += 0.1;
    else if (strcmp(source, "shell") == 0) confidence += 0.05;
    
    /* Clamp to 0.0-1.0 */
    if (confidence > 1.0) confidence = 1.0;
    if (confidence < 0.0) confidence = 0.0;
    
    return confidence;
}
