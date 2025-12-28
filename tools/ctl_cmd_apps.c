/* ctl_cmd_apps.c - Application management commands
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Commands: explain, predict, promote, demote, reset, show_hidden
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <linux/limits.h>
#include <libgen.h>
#include <glib.h>

#include "ctl_commands.h"
#include "ctl_daemon.h"
#include "ctl_state.h"
#include "ctl_config.h"

/* File paths */
#define STATEFILE "/usr/local/var/lib/preheat/preheat.state"
#define PACKAGE "preheat"

/**
 * Command: explain - Explain why an app is/isn't preloaded
 */
int
cmd_explain(const char *app_name)
{
    FILE *f;
    char line[1024];
    char resolved[PATH_MAX];
    const char *final_name;
    
    if (!app_name || !*app_name) {
        fprintf(stderr, "Error: Missing application name\n");
        fprintf(stderr, "Usage: preheat-ctl explain APP\n");
        fprintf(stderr, "Example: preheat-ctl explain firefox\n");
        return 1;
    }
    
    final_name = resolve_app_name(app_name, resolved, sizeof(resolved));
    
    f = fopen(STATEFILE, "r");
    if (!f) {
        f = fopen("/var/lib/preheat/preheat.state", "r");
        if (!f) {
            fprintf(stderr, "Error: Cannot read state file\n");
            if (access(STATEFILE, F_OK) == 0 || access("/var/lib/preheat/preheat.state", F_OK) == 0) {
                fprintf(stderr, "\nThe state file exists but you don't have permission to read it.\n");
                fprintf(stderr, "Try running with sudo:\n\n");
                fprintf(stderr, "    sudo preheat-ctl explain %s\n\n", app_name);
            } else {
                fprintf(stderr, "The daemon may not be running or state file doesn't exist.\n");
            }
            return 1;
        }
    }
    
    int found = 0;
    double weighted_launches = 0.0;
    unsigned long raw_launches = 0;
    time_t last_seen = 0, first_seen = 0;
    unsigned long total_runtime = 0;
    char pool_str[32] = "unknown";
    
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "EXE\t", 4) == 0) {
            char path[512];
            int seq, update_time, run_time, expansion, pool_val;
            unsigned long raw, duration;
            double weighted;
            
            if (sscanf(line, "EXE\t%d\t%d\t%d\t%d\t%d\t%lf\t%lu\t%lu\t%511s",
                       &seq, &update_time, &run_time, &expansion, &pool_val,
                       &weighted, &raw, &duration, path) >= 9) {
                
                if (paths_match(final_name, path)) {
                    found = 1;
                    weighted_launches = weighted;
                    raw_launches = raw;
                    last_seen = update_time;
                    first_seen = seq;
                    total_runtime = run_time;
                    strcpy(pool_str, pool_val == 0 ? "priority" : "observation");
                    break;
                }
            }
        }
    }
    fclose(f);
    
    if (!found) {
        GPtrArray *similar = g_ptr_array_new_with_free_func(g_free);
        char *search_copy = g_strdup(final_name);
        char *search_basename = g_strdup(basename(search_copy));
        g_free(search_copy);
        
        f = fopen(STATEFILE, "r");
        if (!f) f = fopen("/var/lib/preheat/preheat.state", "r");
        
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "EXE\t", 4) == 0) {
                    char path[512];
                    int seq, update_time, run_time, expansion, pool_val;
                    unsigned long raw, duration;
                    double weighted;
                    
                    if (sscanf(line, "EXE\t%d\t%d\t%d\t%d\t%d\t%lf\t%lu\t%lu\t%511s",
                               &seq, &update_time, &run_time, &expansion, &pool_val,
                               &weighted, &raw, &duration, path) >= 9) {
                        
                        char *path_plain = is_uri(path) ? uri_to_path(path) : g_strdup(path);
                        if (path_plain) {
                            char *path_copy = g_strdup(path_plain);
                            char *path_basename = g_strdup(basename(path_copy));
                            g_free(path_copy);
                            
                            if (strstr(path_basename, search_basename) || 
                                strstr(search_basename, path_basename)) {
                                gboolean already_added = FALSE;
                                for (guint i = 0; i < similar->len; i++) {
                                    if (strcmp(g_ptr_array_index(similar, i), path_plain) == 0) {
                                        already_added = TRUE;
                                        break;
                                    }
                                }
                                if (!already_added && similar->len < 5) {
                                    g_ptr_array_add(similar, g_strdup(path_plain));
                                }
                            }
                            g_free(path_basename);
                            g_free(path_plain);
                        }
                    }
                }
            }
            fclose(f);
        }
        
        printf("\n  App: %s\n", final_name);
        printf("  %s\n\n", "═══════════════════════════════════════");
        printf("  Status:  ❌ NOT TRACKED\n\n");
        printf("  This application has never been launched while\n");
        printf("  the preheat daemon was running.\n\n");
        
        if (similar->len > 0) {
            printf("  Did you mean:\n");
            for (guint i = 0; i < similar->len; i++) {
                printf("    - %s\n", (char*)g_ptr_array_index(similar, i));
            }
            printf("\n");
        }
        
        printf("  To start tracking:\n");
        printf("    1. Launch the application\n");
        printf("    2. Wait for preheat to learn your usage patterns\n");
        printf("    3. Run this command again to see predictions\n\n");
        
        g_free(search_basename);
        g_ptr_array_free(similar, TRUE);
        return 0;
    }
    
    double freq_score = (weighted_launches / 600.0);
    if (freq_score > 1.0) freq_score = 1.0;
    double recency_score = (raw_launches > 0) ? 0.5 : 0.0;
    double combined = (0.6 * freq_score) + (0.4 * recency_score);
    
    printf("\n  App: %s\n", final_name);
    printf("  %s\n\n", "═══════════════════════════════════════");
    
    int is_priority = (strcmp(pool_str, "priority") == 0);
    int should_preload = (combined > 0.30 && is_priority);
    
    if (should_preload) printf("  Status:  ✅ PRELOADED\n");
    else if (!is_priority) printf("  Status:  ⚠️  OBSERVATION POOL\n");
    else printf("  Status:  ❌ NOT PRELOADED\n");
    printf("  Pool:    %s\n\n", pool_str);
    
    printf("  Statistics:\n");
    printf("    Weighted Launches:  %.2f\n", weighted_launches);
    printf("    Raw Launches:       %lu\n", raw_launches);
    
    int runtime_hours = total_runtime / 3600;
    int runtime_mins = (total_runtime % 3600) / 60;
    printf("    Total Runtime:      %dh %dm\n", runtime_hours, runtime_mins);
    
    int time_since_update = last_seen - first_seen;
    if (time_since_update > 0) {
        int days_diff = time_since_update / 86400;
        int hours_diff = (time_since_update % 86400) / 3600;
        if (days_diff > 0) printf("    Activity Span:      %dd %dh (in daemon time)\n", days_diff, hours_diff);
        else if (hours_diff > 0) printf("    Activity Span:      %dh (in daemon time)\n", hours_diff);
        else printf("    Activity Span:      Recently started\n");
    } else {
        printf("    Activity Span:      Single session\n");
    }
    
    printf("\n  Prediction Scores:\n");
    printf("    Frequency:   %.2f ", freq_score);
    if (freq_score > 0.7) printf("(very frequently used)\n");
    else if (freq_score > 0.4) printf("(moderately used)\n");
    else printf("(infrequently used)\n");
    
    printf("    Recency:     %.2f ", recency_score);
    if (recency_score > 0.7) printf("(used very recently)\n");
    else if (recency_score > 0.4) printf("(used recently)\n");
    else printf("(not used recently)\n");
    
    printf("    ──────────────────────────────────────\n");
    printf("    Combined:    %.2f ", combined);
    if (combined > 0.6) printf("(HIGH PRIORITY)\n");
    else if (combined > 0.3) printf("(MEDIUM PRIORITY)\n");
    else printf("(LOW PRIORITY)\n");
    
    printf("\n  Decision: ");
    if (should_preload) {
        printf("✅ Preloaded\n");
        printf("    This app exceeds the preload threshold (%.2f > 0.30)\n", combined);
        printf("    It will be loaded into memory before you launch it.\n");
    } else if (!is_priority) {
        printf("⚠️  Not Eligible\n");
        printf("    This app is in the observation pool.\n");
        printf("    Observation pool apps are tracked but not preloaded.\n\n");
        printf("  Why observation pool?\n");
        printf("    - System utilities are typically not preloaded\n");
        printf("    - Use 'preheat-ctl promote %s' to force priority pool\n", app_name);
    } else {
        printf("❌ Not Preloaded\n");
        printf("    This app doesn't exceed the threshold (%.2f < 0.30)\n", combined);
        printf("    Reason: Insufficient usage frequency\n");
        printf("\n  Recommendation:\n");
        printf("    Launch this app more frequently to increase its priority.\n");
    }
    
    printf("\n");
    return 0;
}

/**
 * Command: predict - Show top predicted applications
 */
int
cmd_predict(int top_n)
{
    printf("Top %d Predicted Applications\n", top_n);
    printf("=============================\n\n");

    FILE *f = fopen(STATEFILE, "r");
    int first_errno = errno;
    if (!f) f = fopen("/var/lib/preheat/preheat.state", "r");

    if (!f) {
        if (first_errno == EACCES || first_errno == EPERM) {
            fprintf(stderr, "Error: Permission denied reading state file\n");
            fprintf(stderr, "Hint: Try with sudo\n");
        } else {
            printf("State file not found.\n");
            printf("The daemon needs to run and collect data first.\n");
            printf("\nHint: Start the daemon with 'systemctl start preheat'\n");
        }
        return 1;
    }

    char line[1024];
    int exe_count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "EXE\t", 4) == 0) {
            exe_count++;
            if (exe_count <= top_n) {
                int seq, update_time, time, expansion;
                char path[512];
                if (sscanf(line, "EXE\t%d\t%d\t%d\t%d\t%511s",
                           &seq, &update_time, &time, &expansion, path) >= 5) {
                    printf("%2d. %s (run time: %d sec)\n", exe_count, path, time);
                }
            }
        }
    }
    fclose(f);

    if (exe_count == 0) {
        printf("No tracked applications yet.\n");
        printf("The daemon is still learning usage patterns.\n");
    } else {
        printf("\nTotal tracked: %d applications\n", exe_count);
    }

    return 0;
}

/**
 * Command: promote - Add app to priority pool
 */
int
cmd_promote(const char *app_name)
{
    char resolved[PATH_MAX];
    const char *final_name;
    
    if (!app_name || !*app_name) {
        fprintf(stderr, "Error: Missing application name\n");
        fprintf(stderr, "Usage: preheat-ctl promote APP\n");
        return 1;
    }

    final_name = resolve_app_name(app_name, resolved, sizeof(resolved));
    if (final_name != app_name) {
        printf("Resolved '%s' to '%s'\n", app_name, final_name);
    }

    if (add_to_config_file(SYSCONFDIR "/preheat.d/apps.list", final_name) != 0)
        return 1;

    printf("Promoted '%s' to priority pool\n", final_name);
    
    int pid = get_daemon_pid(0);
    if (pid > 0) {
        send_signal(pid, SIGHUP, "configuration reloaded");
    } else {
        printf("Note: Daemon not running. Changes will apply on next start.\n");
    }
    return 0;
}

/**
 * Command: demote - Add app to observation pool
 */
int
cmd_demote(const char *app_name)
{
    char resolved[PATH_MAX];
    const char *final_name;
    
    if (!app_name || !*app_name) {
        fprintf(stderr, "Error: Missing application name\n");
        fprintf(stderr, "Usage: preheat-ctl demote APP\n");
        return 1;
    }

    final_name = resolve_app_name(app_name, resolved, sizeof(resolved));
    if (final_name != app_name) {
        printf("Resolved '%s' to '%s'\n", app_name, final_name);
    }

    if (add_to_config_file(SYSCONFDIR "/preheat.d/blacklist", final_name) != 0)
        return 1;

    printf("Demoted '%s' to observation pool\n", final_name);
    
    int pid = get_daemon_pid(0);
    if (pid > 0) {
        send_signal(pid, SIGHUP, "configuration reloaded");
    } else {
        printf("Note: Daemon not running. Changes will apply on next start.\n");
    }
    return 0;
}

/**
 * Command: reset - Remove manual override
 */
int
cmd_reset(const char *app_name)
{
    if (!app_name || !*app_name) {
        fprintf(stderr, "Error: Missing application name\n");
        fprintf(stderr, "Usage: preheat-ctl reset APP\n");
        return 1;
    }

    remove_from_config_file(SYSCONFDIR "/preheat.d/apps.list", app_name);
    remove_from_config_file(SYSCONFDIR "/preheat.d/blacklist", app_name);

    printf("Reset '%s' to automatic classification\n", app_name);
    
    int pid = get_daemon_pid(0);
    if (pid > 0) {
        send_signal(pid, SIGHUP, "configuration reloaded");
    } else {
        printf("Note: Daemon not running. Changes will apply on next start.\n");
    }
    return 0;
}

/**
 * Command: show-hidden - Display observation pool apps
 */
int
cmd_show_hidden(void)
{
    printf("Observation Pool Apps (hidden from stats):\n");
    printf("==========================================\n\n");
    
    FILE *f = fopen(STATEFILE, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open state file\n");
        return 1;
    }

    char line[1024];
    int count = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "EXE\t", 4) == 0) {
            int seq, update_time, run_time, expansion, pool;
            double weighted_launches;
            unsigned long raw_launches, total_duration;
            char path[512];

            if (sscanf(line, "EXE\t%d\t%d\t%d\t%d\t%d\t%lf\t%lu\t%lu\t%511s",
                       &seq, &update_time, &run_time, &expansion, &pool,
                       &weighted_launches, &raw_launches, &total_duration, path) >= 9) {
                if (pool == 1) {
                    const char *display_path = path;
                    if (strncmp(path, "file://", 7) == 0) display_path = path + 7;
                    printf("  %s\n", display_path);
                    count++;
                }
            }
        }
    }

    fclose(f);

    if (count == 0) printf("  (no apps in observation pool yet)\n");
    printf("\nTotal: %d apps\n", count);
    return 0;
}
