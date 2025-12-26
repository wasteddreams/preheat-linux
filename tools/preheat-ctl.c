/* preheat-ctl.c - CLI control tool for Preheat daemon
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: CLI Control Tool (preheat-ctl)
 * =============================================================================
 *
 * Provides command-line interface for monitoring, controlling, and debugging
 * the preheat daemon. Does NOT link against the daemon - communicates via:
 *   - PID file (/var/run/preheat.pid) for process identification
 *   - Signals (SIGHUP, SIGUSR1, SIGUSR2, SIGTERM) for commands
 *   - Pause file (/run/preheat.pause) for pause state
 *   - Stats file (/run/preheat.stats) for statistics
 *   - State file (preheat.state) for reading learned patterns
 *
 * AVAILABLE COMMANDS:
 *
 *   status    │ Check if daemon is running (reads PID file, checks /proc)
 *   stats     │ Show hit rate and preload statistics (sends SIGUSR1)
 *   mem       │ Display memory available for preloading (/proc/meminfo)
 *   predict   │ List top predicted applications (reads state file)
 *   pause     │ Temporarily disable preloading (creates pause file)
 *   resume    │ Re-enable preloading (removes pause file)
 *   export    │ Export learned patterns to JSON (reads state file)
 *   import    │ Validate JSON import file (informational only)
 *   reload    │ Reload configuration (sends SIGHUP)
 *   dump      │ Dump state to log (sends SIGUSR1)
 *   save      │ Save state immediately (sends SIGUSR2)
 *   stop      │ Graceful shutdown (sends SIGTERM, waits up to 5s)
 *   update    │ Run update script (requires root)
 *
 * DURATION PARSING (pause command):
 *   "30m"          → 30 minutes
 *   "2h"           → 2 hours
 *   "1h30m"        → 1 hour 30 minutes
 *   "until-reboot" → Pause until system restart
 *
 * =============================================================================
 */

#define _DEFAULT_SOURCE  /* For usleep() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>  /* For PATH_MAX */
#include <sys/stat.h>
#include <math.h>    /* For exp() */
#include <libgen.h>  /* For basename() */
#include <glib.h>    /* For GLib utilities */

/* File paths for daemon communication */
#define PIDFILE "/var/run/preheat.pid"
#define PAUSEFILE "/run/preheat.pause"
#define STATEFILE "/usr/local/var/lib/preheat/preheat.state"
#define PACKAGE "preheat"
#define STATSFILE "/run/preheat.stats"
#define DEFAULT_EXPORT "preheat-profile.json"

/* Forward declarations */
static const char *resolve_app_name(const char *name, char *buffer, size_t bufsize);
static char *uri_to_path(const char *uri);
static gboolean is_uri(const char *str);
static gboolean paths_match(const char *search_path, const char *state_path);

/**
 * Print usage information and available commands
 *
 * @param prog  Program name (argv[0]) for usage line
 */
static void
print_usage(const char *prog)
{
    printf("Usage: %s COMMAND [OPTIONS]\n\n", prog);
    printf("Control the %s daemon\n\n", PACKAGE);
    printf("Commands:\n");
    printf("  status      Check if daemon is running\n");
    printf("  stats       Show preload statistics and hit rate\n");
    printf("  mem         Show memory statistics\n");
    printf("  predict     Show top predicted applications\n");
    printf("  pause       Pause preloading temporarily\n");
    printf("  resume      Resume preloading\n");
    printf("  export      Export learned patterns to JSON file\n");
    printf("  import      Import patterns from JSON file\n");
    printf("  reload      Reload configuration (send SIGHUP)\n");
    printf("  dump        Dump state to log (send SIGUSR1)\n");
    printf("  save        Save state immediately (send SIGUSR2)\n");
    printf("  stop        Stop daemon gracefully (send SIGTERM)\n");
    printf("  update      Update preheat to latest version\n");
    printf("  promote     Add app to priority pool (always show in stats)\n");
    printf("  demote      Add app to observation pool (hide from stats)\n");
    printf("  show-hidden Show apps in observation pool\n");
    printf("  reset       Remove manual override for an app\n");
    printf("  explain     Explain why an app is/isn't preloaded\n");
    printf("  health      Quick system health check (exit codes: 0/1/2)\n");
    printf("  help        Show this help message\n");
    printf("\nOptions for stats:\n");
    printf("  --verbose   Show detailed statistics with top 20 apps\n");
    printf("  -v          Short for --verbose\n");
    printf("\nOptions for predict:\n");
    printf("  --top N     Show top N predictions (default: 10)\n");
    printf("\nOptions for pause:\n");
    printf("  DURATION    Time to pause: 30m, 2h, 1h30m, until-reboot (default: 1h)\n");
    printf("\nOptions for export/import:\n");
    printf("  FILE        Path to JSON file (default: %s)\n", DEFAULT_EXPORT);
    printf("\nOptions for promote/demote/reset/explain:\n");
    printf("  APP         Application name or path (e.g., firefox, /usr/bin/code)\n");
    printf("\n");
}

/**
 * Read daemon PID from PID file (internal, does not print errors)
 *
 * @return  PID from file on success, -1 if not found or error
 */
static int
read_pid_file(void)
{
    FILE *f;
    int pid = -1;

    f = fopen(PIDFILE, "r");
    if (!f) {
        return -1;
    }

    if (fscanf(f, "%d", &pid) != 1) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return pid;
}

/**
 * Check if process with given PID is a preheat process
 *
 * Verifies both that the process exists and that it's actually
 * the preheat daemon (not a recycled PID).
 *
 * @param pid  Process ID to check
 * @return     1 if running preheat, 0 if not
 */
static int
check_running(int pid)
{
    char proc_path[128];
    char exe_buffer[256];
    ssize_t len;

    /* First check if process exists */
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    if (access(proc_path, F_OK) != 0) {
        return 0;  /* Process doesn't exist */
    }

    /* Verify it's actually preheat by checking /proc/PID/exe */
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", pid);
    len = readlink(proc_path, exe_buffer, sizeof(exe_buffer) - 1);
    if (len > 0) {
        exe_buffer[len] = '\0';
        /* Check if the executable name contains "preheat" */
        if (strstr(exe_buffer, "preheat") != NULL) {
            return 1;  /* Running and is preheat */
        }
    }

    /* If we can't read exe (permission denied), just check if process exists */
    /* This happens when running without root */
    if (errno == EACCES) {
        return 1;  /* Assume it's preheat if we can't verify */
    }

    return 0;  /* Not preheat */
}

/**
 * Find running preheat daemon using pgrep
 *
 * Fallback when PID file is stale or missing. Uses pgrep to scan
 * for a process named "preheat".
 *
 * @return  PID of running daemon, -1 if not found
 */
static int
find_running_daemon(void)
{
    FILE *pf;
    char buf[32];
    int pid = -1;

    pf = popen("pgrep -x preheat 2>/dev/null", "r");
    if (pf) {
        if (fgets(buf, sizeof(buf), pf)) {
            pid = atoi(buf);
        }
        pclose(pf);
    }

    return pid;
}

/**
 * Get daemon PID with fallback to process scanning
 *
 * First tries the PID file, then falls back to pgrep if:
 *   - PID file doesn't exist
 *   - PID file contains a stale PID
 *
 * @param verbose  If true, print error messages
 * @return         PID of daemon on success, -1 if not found
 */
static int
get_daemon_pid(int verbose)
{
    int pid;

    /* Try PID file first */
    pid = read_pid_file();
    if (pid > 0 && check_running(pid)) {
        return pid;  /* PID file is valid */
    }

    /* PID file missing or stale, try pgrep */
    pid = find_running_daemon();
    if (pid > 0) {
        /* Found via pgrep - PID file was stale */
        return pid;
    }

    /* Daemon not found */
    if (verbose) {
        fprintf(stderr, "Error: %s is not running\n", PACKAGE);
        fprintf(stderr, "Hint: Start with 'sudo systemctl start preheat'\n");
    }
    return -1;
}

/**
 * Read daemon PID from PID file (legacy wrapper for compatibility)
 *
 * @return  PID of daemon on success, -1 if not found or error
 */
static int
read_pid(void)
{
    return get_daemon_pid(1);  /* verbose mode */
}

/**
 * Send a signal to the daemon process
 *
 * Wrapper around kill() with user-friendly error messages and
 * permission hints.
 *
 * @param pid     Process ID of daemon
 * @param sig     Signal number (SIGHUP, SIGUSR1, etc.)
 * @param action  Human-readable description for success message
 * @return        0 on success, 1 on error
 */
static int
send_signal(int pid, int sig, const char *action)
{
    if (kill(pid, sig) < 0) {
        fprintf(stderr, "Error: Failed to send signal to %s (PID %d): %s\n",
                PACKAGE, pid, strerror(errno));
        if (errno == EPERM) {
            fprintf(stderr, "Hint: Try with sudo\n");
        }
        return 1;
    }

    printf("%s: %s\n", PACKAGE, action);
    return 0;
}

/**
 * Command: status - Check daemon running state
 *
 * Displays:
 *   - Whether daemon is running
 *   - Current PID
 *   - Pause state (if paused, with remaining time)
 *
 * @return  0 if running, 1 if not running or error
 */
static int
cmd_status(void)
{
    int pid = read_pid();
    if (pid < 0)
        return 1;

    int status = check_running(pid);
    if (status == 1) {
        /* Check pause state */
        FILE *pf = fopen(PAUSEFILE, "r");
        if (pf) {
            long expiry = 0;
            if (fscanf(pf, "%ld", &expiry) == 1) {
                time_t now = time(NULL);
                if (expiry == 0) {
                    printf("%s is running (PID %d) - PAUSED (until reboot)\n", PACKAGE, pid);
                } else if (expiry > now) {
                    int remaining = (int)(expiry - now);
                    int hours = remaining / 3600;
                    int mins = (remaining % 3600) / 60;
                    printf("%s is running (PID %d) - PAUSED (%dh %dm remaining)\n",
                           PACKAGE, pid, hours, mins);
                } else {
                    printf("%s is running (PID %d)\n", PACKAGE, pid);
                }
            } else {
                printf("%s is running (PID %d)\n", PACKAGE, pid);
            }
            fclose(pf);
        } else {
            printf("%s is running (PID %d)\n", PACKAGE, pid);
        }
        return 0;
    } else if (status == 0) {
        fprintf(stderr, "%s is not running (stale PID file?)\n", PACKAGE);
        return 1;
    } else {
        fprintf(stderr, "%s status unknown\n", PACKAGE);
        return 1;
    }
}

/**
 * Parse duration string like "30m", "2h", "1h30m", "until-reboot"
 * Returns seconds, 0 for until-reboot, -1 on error
 */
static int
parse_duration(const char *str)
{
    if (!str || !*str) {
        return 3600;  /* Default: 1 hour */
    }

    if (strcmp(str, "until-reboot") == 0) {
        return 0;
    }

    int total = 0;
    int num = 0;
    const char *p = str;

    while (*p) {
        if (*p >= '0' && *p <= '9') {
            num = num * 10 + (*p - '0');
        } else if (*p == 'h' || *p == 'H') {
            total += num * 3600;
            num = 0;
        } else if (*p == 'm' || *p == 'M') {
            total += num * 60;
            num = 0;
        } else if (*p == 's' || *p == 'S') {
            total += num;
            num = 0;
        } else {
            return -1;  /* Invalid character */
        }
        p++;
    }

    /* Handle trailing number without unit (treat as minutes) */
    if (num > 0) {
        total += num * 60;
    }

    return total > 0 ? total : -1;
}

/**
 * Command: pause - Temporarily disable preloading
 *
 * Creates /run/preheat.pause with expiry timestamp. The daemon
 * checks this file and skips prediction/readahead while paused.
 *
 * @param duration  Duration string ("30m", "2h", "until-reboot")
 * @return          0 on success, 1 on error
 */
static int
cmd_pause(const char *duration)
{
    int seconds = parse_duration(duration);

    if (seconds < 0) {
        fprintf(stderr, "Error: Invalid duration '%s'\n", duration);
        fprintf(stderr, "Examples: 30m, 2h, 1h30m, until-reboot\n");
        return 1;
    }

    /* Write pause file */
    FILE *f = fopen(PAUSEFILE, "w");
    if (!f) {
        fprintf(stderr, "Error: Cannot create pause file: %s\n", strerror(errno));
        fprintf(stderr, "Hint: Try with sudo\n");
        return 1;
    }

    time_t expiry = (seconds == 0) ? 0 : time(NULL) + seconds;
    fprintf(f, "%ld\n", expiry);
    fclose(f);

    if (seconds == 0) {
        printf("Preloading paused until reboot\n");
    } else {
        int hours = seconds / 3600;
        int mins = (seconds % 3600) / 60;
        if (hours > 0 && mins > 0) {
            printf("Preloading paused for %dh %dm\n", hours, mins);
        } else if (hours > 0) {
            printf("Preloading paused for %d hour(s)\n", hours);
        } else {
            printf("Preloading paused for %d minute(s)\n", mins);
        }
    }

    return 0;
}

/**
 * Command: resume - Re-enable preloading
 *
 * Removes /run/preheat.pause to allow preloading to resume.
 *
 * @return  0 on success (including if not paused), 1 on error
 */
static int
cmd_resume(void)
{
    if (unlink(PAUSEFILE) == 0) {
        printf("Preloading resumed\n");
        return 0;
    } else if (errno == ENOENT) {
        printf("Preloading was not paused\n");
        return 0;
    } else {
        fprintf(stderr, "Error: Cannot remove pause file: %s\n", strerror(errno));
        fprintf(stderr, "Hint: Try with sudo\n");
        return 1;
    }
}

/**
 * Command: stats - Display preload statistics
 *
 * Sends SIGUSR1 to daemon to trigger stats dump, then reads
 * /run/preheat.stats and displays formatted output including:
 *   - Uptime, apps tracked
 *   - Preload hits/misses
 *   - Hit rate percentage with quality indicator
 *
 * @return  0 on success, 1 on error
 */
static int
cmd_stats(void)
{
    int pid = read_pid();
    FILE *f;
    char line[256];

    if (pid < 0)
        return 1;

    if (!check_running(pid)) {
        fprintf(stderr, "Error: %s is not running\n", PACKAGE);
        return 1;
    }

    /* Send SIGUSR1 to dump stats */
    if (kill(pid, SIGUSR1) < 0) {
        if (errno == EPERM) {
            fprintf(stderr, "Error: Permission denied\n");
            fprintf(stderr, "Hint: Try with sudo\n");
        } else {
            fprintf(stderr, "Error: %s\n", strerror(errno));
        }
        return 1;
    }

    /* Wait for stats file to be written */
    usleep(200000);  /* 200ms */

    /* Read and display stats */
    f = fopen(STATSFILE, "r");
    if (!f) {
        fprintf(stderr, "Error: Stats file not available yet\n");
        fprintf(stderr, "Try again in a moment.\n");
        return 1;
    }

    printf("\n  Preheat Statistics\n");
    printf("  ==================\n\n");

    unsigned long hits = 0, misses = 0, preloads = 0;
    int uptime = 0, apps = 0;
    double hit_rate = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;

        if (sscanf(line, "uptime_seconds=%d", &uptime) == 1) continue;
        if (sscanf(line, "preloads_total=%lu", &preloads) == 1) continue;
        if (sscanf(line, "hits=%lu", &hits) == 1) continue;
        if (sscanf(line, "misses=%lu", &misses) == 1) continue;
        if (sscanf(line, "hit_rate=%lf", &hit_rate) == 1) continue;
        if (sscanf(line, "apps_tracked=%d", &apps) == 1) continue;
    }
    fclose(f);

    /* Display summary */
    int hours = uptime / 3600;
    int mins = (uptime % 3600) / 60;

    printf("  Uptime:       %dh %dm\n", hours, mins);
    printf("  Apps tracked: %d\n\n", apps);

    printf("  Preload Events:\n");
    printf("    Total:   %lu\n", preloads);
    printf("    Hits:    %lu\n", hits);
    printf("    Misses:  %lu\n\n", misses);

    /* Color-code hit rate */
    if (hit_rate >= 70.0) {
        printf("  Hit Rate:  %.1f%% (excellent)\n", hit_rate);
    } else if (hit_rate >= 50.0) {
        printf("  Hit Rate:  %.1f%% (good)\n", hit_rate);
    } else if (hit_rate >= 30.0) {
        printf("  Hit Rate:  %.1f%% (learning)\n", hit_rate);
    } else if (hits + misses > 0) {
        printf("  Hit Rate:  %.1f%% (early stage)\n", hit_rate);
    } else {
        printf("  Hit Rate:  - (no data yet)\n");
    }

    printf("\n");
    return 0;
}

/**
 * Format number with commas for readability
 * Example: 1234567 -> "1,234,567"
 */
static void
format_number(char *buf, unsigned long num)
{
    char temp[64];
    snprintf(temp, sizeof(temp), "%lu", num);
    
    int len = strlen(temp);
    int pos = 0;
    
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            buf[pos++] = ',';
        }
        buf[pos++] = temp[i];
    }
    buf[pos] = '\0';
}

/**
 * Command: stats --verbose - Display detailed statistics
 *
 * Enhancement #5: Extended stats display with:
 *   - Pool breakdown (priority vs observation)
 *   - Memory metrics (total preloaded, pressure events)
 *   - Top 20 apps table with weighted launches
 *
 * @return  0 on success, 1 on error
 */
static int
cmd_stats_verbose(void)
{
    int pid = read_pid();
    FILE *f;
    char line[512];

    if (pid < 0)
        return 1;

    if (!check_running(pid)) {
        fprintf(stderr, "Error: %s is not running\n", PACKAGE);
        return 1;
    }

    /* Send SIGUSR1 to dump stats */
    if (kill(pid, SIGUSR1) < 0) {
        if (errno == EPERM) {
            fprintf(stderr, "Error: Permission denied\n");
            fprintf(stderr, "Hint: Try with sudo\n");
        } else {
            fprintf(stderr, "Error: %s\n", strerror(errno));
        }
        return 1;
    }

    /* Wait for stats file to be written */
    usleep(200000);  /* 200ms */

    /* Read and display stats */
    f = fopen(STATSFILE, "r");
    if (!f) {
        fprintf(stderr, "Error: Stats file not available yet\n");
        fprintf(stderr, "Try again in a moment.\n");
        return 1;
    }

    /* Parse all metrics */
    char version[64] = "unknown";
    unsigned long hits = 0, misses = 0, preloads = 0, mem_pressure = 0;
    int uptime = 0, apps = 0, priority_pool = 0, observation_pool = 0;
    size_t total_mb = 0;
    double hit_rate = 0;
    
    /* Top apps data */
    struct {
        char name[128];
        double weighted;
        unsigned long raw;
        int preloaded;
        char pool[16];
    } top_apps[20];
    int num_top_apps = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;

        sscanf(line, "version=%63s", version);
        sscanf(line, "uptime_seconds=%d", &uptime);
        sscanf(line, "preloads_total=%lu", &preloads);
        sscanf(line, "hits=%lu", &hits);
        sscanf(line, "misses=%lu", &misses);
        sscanf(line, "hit_rate=%lf", &hit_rate);
        sscanf(line, "apps_tracked=%d", &apps);
        sscanf(line, "priority_pool=%d", &priority_pool);
        sscanf(line, "observation_pool=%d", &observation_pool);
        sscanf(line, "total_preloaded_mb=%zu", &total_mb);
        sscanf(line, "memory_pressure_events=%lu", &mem_pressure);
        
        /* Parse top apps: top_app_N=name:weighted:raw:preloaded:pool */
        if (strncmp(line, "top_app_", 8) == 0 && num_top_apps < 20) {
            char *eq = strchr(line, '=');
            if (eq) {
                eq++; /* Skip '=' */
                char *name = eq;
                char *colon1 = strchr(name, ':');
                if (colon1) {
                    *colon1 = '\0';
                    
                    /* Ensure name fits in buffer */
                    if (strlen(name) >= sizeof(top_apps[0].name)) {
                        continue;  /* Skip if name too long */
                    }
                    
                    strncpy(top_apps[num_top_apps].name, name, sizeof(top_apps[0].name) - 1);
                    top_apps[num_top_apps].name[sizeof(top_apps[0].name) - 1] = '\0';  /* Ensure null termination */
                    
                    char *weighted_str = colon1 + 1;
                    char *colon2 = strchr(weighted_str, ':');
                    if (colon2) {
                        *colon2 = '\0';
                        top_apps[num_top_apps].weighted = atof(weighted_str);
                        
                        char *raw_str = colon2 + 1;
                        char *colon3 = strchr(raw_str, ':');
                        if (colon3) {
                            *colon3 = '\0';
                            top_apps[num_top_apps].raw = atol(raw_str);
                            
                            char *preloaded_str = colon3 + 1;
                            char *colon4 = strchr(preloaded_str, ':');
                            if (colon4) {
                                *colon4 = '\0';
                                top_apps[num_top_apps].preloaded = atoi(preloaded_str);
                                
                                char *pool_str = colon4 + 1;
                                /* Remove newline */
                                char *newline = strchr(pool_str, '\n');
                                if (newline) *newline = '\0';
                                
                                /* Ensure pool string fits */
                                if (strlen(pool_str) < sizeof(top_apps[0].pool)) {
                                    strncpy(top_apps[num_top_apps].pool, pool_str, 
                                            sizeof(top_apps[0].pool) - 1);
                                    top_apps[num_top_apps].pool[sizeof(top_apps[0].pool) - 1] = '\0';
                                    
                                    num_top_apps++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    fclose(f);

    /* Display verbose output */
    printf("\n  Preheat Statistics (Verbose)\n");
    printf("  ==============================\n\n");

    /* Daemon section */
    int days = uptime / 86400;
    int hours = (uptime % 86400) / 3600;
    int mins = (uptime % 3600) / 60;
    
    printf("  Daemon:\n");
    printf("    Version:      %s\n", version);
    if (days > 0) {
        printf("    Uptime:       %dd %dh %dm\n", days, hours, mins);
    } else {
        printf("    Uptime:       %dh %dm\n", hours, mins);
    }
    printf("    PID:          %d\n\n", pid);

    /* Performance section */
    char preloads_fmt[32], hits_fmt[32], misses_fmt[32];
    format_number(preloads_fmt, preloads);
    format_number(hits_fmt, hits);
    format_number(misses_fmt, misses);
    
    printf("  Performance:\n");
    printf("    Preloads:     %s total\n", preloads_fmt);
    printf("    Hits:         %s (%.1f%%)\n", hits_fmt, hit_rate);
    printf("    Misses:       %s (%.1f%%)\n", misses_fmt, 100.0 - hit_rate);
    printf("    Efficiency:   ");
    
    if (hit_rate >= 70.0) {
        printf("EXCELLENT\n\n");
    } else if (hit_rate >= 50.0) {
        printf("GOOD\n\n");
    } else if (hit_rate >= 30.0) {
        printf("LEARNING\n\n");
    } else if (hits + misses > 0) {
        printf("EARLY STAGE\n\n");
    } else {
        printf("NO DATA\n\n");
    }

    /* Memory section */
    printf("  Memory:\n");
    printf("    Total Preloaded:  %zu MB\n", total_mb);
    if (num_top_apps > 0 && total_mb > 0) {
        printf("    Avg Size:         %zu MB per app\n", total_mb / (num_top_apps > 0 ? num_top_apps : 1));
    }
    printf("    Pressure Events:  %lu", mem_pressure);
    if (mem_pressure > 0) {
        printf(" (skipped due to low memory)\n\n");
    } else {
        printf("\n\n");
    }

    /* Pool breakdown */
    printf("  Pool Breakdown:\n");
    printf("    Priority:     %d apps (actively preloaded)\n", priority_pool);
    printf("    Observation:  %d apps (tracked only)\n\n", observation_pool);

    /* Top apps table */
    if (num_top_apps > 0) {
        printf("  Top Apps by Activity:\n");
        printf("    Rank  %-20s  Weighted  Raw    Pool\n", "App");
        printf("    ────  ────────────────────  ────────  ─────  ────────\n");
        
        for (int i = 0; i < num_top_apps && i < 20; i++) {
            printf("    %-4d  %-20s  %8.1f  %5lu  %s\n",
                   i + 1,
                   top_apps[i].name,
                   top_apps[i].weighted,
                   top_apps[i].raw,
                   top_apps[i].pool);
        }
    } else {
        printf("  No apps tracked yet\n");
    }

    printf("\n");
    return 0;
}

/**
 * Command: explain - Explain why an app is/isn't preloaded
 *
 * Enhancement #5: Diagnostic tool to understand preload decisions.
 * Reads state file and calculates scores to show reasoning.
 *
 * @param app_name  Application name or path
 * @return          0 on success, 1 on error
 */
static int
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
    
    /* Resolve app name to full path */
    final_name = resolve_app_name(app_name, resolved, sizeof(resolved));
    
    /* Open state file */
    f = fopen(STATEFILE, "r");
    if (!f) {
        /* Try actual state file location */
        f = fopen("/var/lib/preheat/preheat.state", "r");
        if (!f) {
            fprintf(stderr, "Error: Cannot read state file\n");
            
            /* Check if it's a permissions issue */
            if (access(STATEFILE, F_OK) == 0 || access("/var/lib/preheat/preheat.state", F_OK) == 0) {
                fprintf(stderr, "\n");
                fprintf(stderr, "The state file exists but you don't have permission to read it.\n");
                fprintf(stderr, "Try running with sudo:\n\n");
                fprintf(stderr, "    sudo preheat-ctl explain %s\n\n", app_name);
            } else {
                fprintf(stderr, "The daemon may not be running or state file doesn't exist.\n");
            }
            return 1;
        }
    }
    
    /* Search for app in state file (simplified - reads EXE lines) */
    int found = 0;
    double weighted_launches = 0.0;
    unsigned long raw_launches = 0;
    time_t last_seen = 0, first_seen = 0;
    unsigned long total_runtime = 0;
    char pool_str[32] = "unknown";
    
    while (fgets(line, sizeof(line), f)) {
        /* Look for EXE lines that match our app */
        if (strncmp(line, "EXE\t", 4) == 0) {
            char path[512];
            int seq, update_time, run_time, expansion, pool_val;
            unsigned long raw, duration;
            double weighted;
            
            /* Try 9-field format: seq update_time time expansion pool weighted raw duration path */
            if (sscanf(line, "EXE\t%d\t%d\t%d\t%d\t%d\t%lf\t%lu\t%lu\t%511s",
                       &seq, &update_time, &run_time, &expansion, &pool_val,
                       &weighted, &raw, &duration, path) >= 9) {
                
                /* Check if this is our app using robust path matching */
                if (paths_match(final_name, path)) {
                    found = 1;
                    weighted_launches = weighted;
                    raw_launches = raw;
                    last_seen = update_time;
                    first_seen = seq;  /* Approximation */
                    total_runtime = run_time;
                    strcpy(pool_str, pool_val == 1 ? "priority" : "observation");
                    break;
                }
            }
        }
    }
    fclose(f);
    
    if (!found) {
        /* P1 Feature: Fuzzy search for similar apps */
        GPtrArray *similar = g_ptr_array_new_with_free_func(g_free);
        char *search_copy = g_strdup(final_name);
        char *search_basename = g_strdup(basename(search_copy));
        g_free(search_copy);
        
        /* Second pass: find apps with matching basename */
        f = fopen(STATEFILE, "r");
        if (!f) {
            f = fopen("/var/lib/preheat/preheat.state", "r");
        }
        
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
                        
                        /* Convert URI if needed */
                        char *path_plain = is_uri(path) ? uri_to_path(path) : g_strdup(path);
                        if (path_plain) {
                            char *path_copy = g_strdup(path_plain);
                            char *path_basename = g_strdup(basename(path_copy));
                            g_free(path_copy);
                            
                            /* Check if basenames are similar (substring match) */
                            if (strstr(path_basename, search_basename) || 
                                strstr(search_basename, path_basename)) {
                                /* Add to suggestions if not already there */
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
        
        /* Display "not found" message */
        printf("\n  App: %s\n", final_name);
        printf("  %s\n\n", "═══════════════════════════════════════");
        printf("  Status:  ❌ NOT TRACKED\n\n");
        printf("  This application has never been launched while\n");
        printf("  the preheat daemon was running.\n\n");
        
        /* Show suggestions if we found similar apps */
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
    
    /* Calculate scores (simplified scoring logic) */
    /* NOTE: update_time and seq are daemon-relative timestamps (seconds since daemon start),
     * not Unix timestamps, so we can't calculate absolute "days ago". Instead, we show
     * the daemon-time values and relative differences. */
    
    /* Frequency score: normalized weighted launches (assume max is 600) */
    double freq_score = (weighted_launches / 600.0);
    if (freq_score > 1.0) freq_score = 1.0;
    
    /* Recency score: We can't calculate real recency without daemon start time,
     * so we use a simplified heuristic based on launch count instead */
    double recency_score = (raw_launches > 0) ? 0.5 : 0.0;
    
    /* Combined score (simplified - real daemon uses more complex formula) */
    double combined = (0.6 * freq_score) + (0.4 * recency_score);
    
    /* Display explanation */
    printf("\n  App: %s\n", final_name);
    printf("  %s\n\n", "═══════════════════════════════════════");
    
    /* Status */
    int is_priority = (strcmp(pool_str, "priority") == 0);
    int should_preload = (combined > 0.30 && is_priority);
    
    if (should_preload) {
        printf("  Status:  ✅ PRELOADED\n");
    } else if (!is_priority) {
        printf("  Status:  ⚠️  OBSERVATION POOL\n");
    } else {
        printf("  Status:  ❌ NOT PRELOADED\n");
    }
    printf("  Pool:    %s\n\n", pool_str);
    
    /* Statistics */
    printf("  Statistics:\n");
    printf("    Weighted Launches:  %.2f\n", weighted_launches);
    printf("    Raw Launches:       %lu\n", raw_launches);
    
    int runtime_hours = total_runtime / 3600;
    int runtime_mins = (total_runtime % 3600) / 60;
    printf("    Total Runtime:      %dh %dm\n", runtime_hours, runtime_mins);
    
    /* Show daemon-relative time difference instead of absolute time */
    int time_since_update = last_seen - first_seen;  /* Both are daemon-relative */
    if (time_since_update > 0) {
        int days_diff = time_since_update / 86400;
        int hours_diff = (time_since_update % 86400) / 3600;
        if (days_diff > 0) {
            printf("    Activity Span:      %dd %dh (in daemon time)\n", days_diff, hours_diff);
        } else if (hours_diff > 0) {
            printf("    Activity Span:      %dh (in daemon time)\n", hours_diff);
        } else {
            printf("    Activity Span:      Recently started\n");
        }
    } else {
        printf("    Activity Span:      Single session\n");
    }
    
    /* Prediction Scores */
    printf("\n  Prediction Scores:\n");
    printf("    Frequency:   %.2f ", freq_score);
    if (freq_score > 0.7) {
        printf("(very frequently used)\n");
    } else if (freq_score > 0.4) {
        printf("(moderately used)\n");
    } else {
        printf("(infrequently used)\n");
    }
    
    printf("    Recency:     %.2f ", recency_score);
    if (recency_score > 0.7) {
        printf("(used very recently)\n");
    } else if (recency_score > 0.4) {
        printf("(used recently)\n");
    } else {
        printf("(not used recently)\n");
    }
    
    printf("    ──────────────────────────────────────\n");
    printf("    Combined:    %.2f ", combined);
    if (combined > 0.6) {
        printf("(HIGH PRIORITY)\n");
    } else if ( combined > 0.3) {
        printf("(MEDIUM PRIORITY)\n");
    } else {
        printf("(LOW PRIORITY)\n");
    }    printf("\n  Decision: ");
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
        if (hours_ago > 48.0) {
            printf("    Reason: Not used recently (%.0f hours ago)\n", hours_ago);
        } else {
            printf("    Reason: Insufficient usage frequency\n");
        }
        printf("\n  Recommendation:\n");
        printf("    Launch this app more frequently to increase its priority.\n");
    }
    
    printf("\n");
    return 0;
}

/**
 * Command: health - Quick system health check
 *
 * Enhancement #5: Monitoring-friendly health check with exit codes:
 *   0 = Healthy (EXCELLENT or GOOD)
 *   1 = Degraded
 *   2 = Critical (daemon not running, or severe issues)
 *
 * @return  Exit code indicating health status
 */
static int
cmd_health(void)
{
    int pid = read_pid();
    FILE *f;
    char line[512];
    int health_score = 0;
    int issues = 0;
    
    /* Check 1: Daemon running? (CRITICAL if not) */
    if (pid > 0 && check_running(pid)) {
        health_score += 40;  /* 40 points for being alive */
    } else {
        printf("❌ CRITICAL - Preheat daemon is not running\n\n");
        printf("  Daemon:       Not Running\n");
        printf("  Status:       Service is down\n\n");
        printf("  Action Required:\n");
        printf("    sudo systemctl start preheat\n\n");
        return 2;  /* Critical */
    }
    
    /* If daemon is running, gather metrics */
    /* Send SIGUSR1 to get fresh stats */
    kill(pid, SIGUSR1);
    usleep(200000);  /* Wait for stats file */
    
    f = fopen(STATSFILE, "r");
    if (!f) {
        printf("⚠️  DEGRADED - Preheat is running but stats unavailable\n\n");
        printf("  Daemon:       Running (PID %d)\n", pid);
        printf("  Hit Rate:     Unknown (stats file missing)\n");
        printf("  Status:       Degraded\n\n");
        return 1;  /* Degraded */
    }
    
    /* Parse stats */
    unsigned long hits = 0, misses = 0, mem_pressure = 0;
    int uptime = 0;
    double hit_rate = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        sscanf(line, "uptime_seconds=%d", &uptime);
        sscanf(line, "hits=%lu", &hits);
        sscanf(line, "misses=%lu", &misses);
        sscanf(line, "hit_rate=%lf", &hit_rate);
        sscanf(line, "memory_pressure_events=%lu", &mem_pressure);
    }
    fclose(f);
    
    /* Check 2: Hit rate (if we have enough data) */
    int days_running = uptime / 86400;
    if (days_running >= 1 && (hits + misses) > 10) {
        if (hit_rate >= 70.0) {
            health_score += 40;  /* Excellent hit rate */
        } else if (hit_rate >= 50.0) {
            health_score += 30;  /* Good hit rate */
        } else if (hit_rate >= 30.0) {
            health_score += 20;  /* Learning phase */
            issues++;
        } else {
            health_score += 10;  /* Poor hit rate */
            issues++;
        }
    } else {
        /* Not enough data yet - neutral score */
        health_score += 25;
    }
    
    /* Check 3: Memory pressure */
    if (mem_pressure == 0) {
        health_score += 10;  /* No memory issues */
    } else if (mem_pressure < 10) {
        health_score += 5;   /* Occasional pressure */
    } else {
        issues++;  /* Frequent memory pressure */
    }
    
    /* Check 4: State file recency */
    struct stat st;
    if (stat("/var/lib/preheat/preheat.state", &st) == 0) {
        time_t now = time(NULL);
        time_t age_minutes = (now - st.st_mtime) / 60;
        
        if (age_minutes < 60) {
            health_score += 10;  /* Recent state save */
        } else if (age_minutes < 1440) {
            health_score += 5;   /* Saved within last day */
        } else {
            issues++;  /* State file very old */
        }
    }
    
    /* Determine overall health */
    const char *status;
    const char *emoji;
    int exit_code;
    
    if (health_score >= 90) {
        status = "EXCELLENT";
        emoji = "✅";
        exit_code = 0;
    } else if (health_score >= 70) {
        status = "GOOD";
        emoji = "✅";
        exit_code = 0;
    } else if (health_score >= 50) {
        status = "DEGRADED";
        emoji = "⚠️ ";
        exit_code = 1;
    } else {
        status = "CRITICAL";
        emoji = "❌";
        exit_code = 2;
    }
    
    /* Display health report */
    printf("%s %s - Preheat is %s\n\n", emoji, status, 
           exit_code == 0 ? "operating optimally" : 
           exit_code == 1 ? "experiencing issues" : "critically degraded");
    
    printf("  Daemon:       Running (PID %d)\n", pid);
    
    if (hits + misses > 0) {
        printf("  Hit Rate:     %.1f%%", hit_rate);
        if (hit_rate >= 70.0) {
            printf(" (excellent)\n");
        } else if (hit_rate >= 50.0) {
            printf(" (good)\n");
        } else if (hit_rate >= 30.0) {
            printf(" (learning)\n");
        } else {
            printf(" (needs improvement)\n");
        }
    } else {
        printf("  Hit Rate:     No data yet\n");
    }
    
    if (mem_pressure > 0) {
        printf("  Memory:       %lu pressure events\n", mem_pressure);
    }
    
    printf("  Uptime:       ");
    if (days_running > 0) {
        printf("%dd %dh\n", days_running, (uptime % 86400) / 3600);
    } else {
        printf("%dh %dm\n", uptime / 3600, (uptime % 3600) / 60);
    }
    
    if (exit_code == 0) {
        printf("\n  Status: All systems operational\n");
    } else if (exit_code == 1) {
        printf("\n  Issues Detected: %d\n", issues);
        if (hit_rate < 30.0 && days_running >= 7) {
            printf("    - Hit rate below optimal (check configuration)\n");
        }
        if (mem_pressure > 10) {
            printf("    - Frequent memory pressure (consider increasing available memory)\n");
        }
    }
    
    printf("\n");
    return exit_code;
}

/**
 * Command: export - Export learned patterns to JSON
 *
 * Reads the binary state file and converts to portable JSON format.
 * Can be used for:
 *   - Backup before system reinstall
 *   - Sharing profiles between machines
 *   - Debugging learned patterns
 *
 * @param filepath  Output file path (default: preheat-profile.json)
 * @return          0 on success, 1 on error
 */
static int
cmd_export(const char *filepath)
{
    FILE *state_f, *export_f;
    char line[1024];
    const char *outpath = filepath ? filepath : DEFAULT_EXPORT;
    time_t now = time(NULL);
    int apps_exported = 0;

    /* Open state file */
    state_f = fopen(STATEFILE, "r");
    if (!state_f) {
        if (errno == EACCES || errno == EPERM) {
            fprintf(stderr, "Error: Permission denied reading state file\n");
            fprintf(stderr, "Hint: Try with sudo\n");
        } else {
            fprintf(stderr, "Error: Cannot open state file %s: %s\n", STATEFILE, strerror(errno));
        }
        return 1;
    }

    /* Create export file */
    export_f = fopen(outpath, "w");
    if (!export_f) {
        fprintf(stderr, "Error: Cannot create export file %s: %s\n", outpath, strerror(errno));
        fclose(state_f);
        return 1;
    }

    /* Write JSON header */
    fprintf(export_f, "{\n");
    fprintf(export_f, "  \"preheat_export_version\": \"1.0\",\n");
    fprintf(export_f, "  \"exported_at\": %ld,\n", now);
    fprintf(export_f, "  \"apps\": [\n");

    /* Parse EXE entries from state file */
    int first = 1;
    while (fgets(line, sizeof(line), state_f)) {
        if (strncmp(line, "EXE\t", 4) == 0) {
            int seq, update_time, run_time, expansion;
            char path[512];

            if (sscanf(line, "EXE\t%d\t%d\t%d\t%d\t%511s",
                       &seq, &update_time, &run_time, &expansion, path) >= 5) {
                if (!first) fprintf(export_f, ",\n");
                fprintf(export_f, "    {\"path\": \"%s\", \"run_time\": %d}", path, run_time);
                apps_exported++;
                first = 0;
            }
        }
    }

    fprintf(export_f, "\n  ]\n");
    fprintf(export_f, "}\n");

    fclose(state_f);
    fclose(export_f);

    printf("Exported %d apps to %s\n", apps_exported, outpath);
    return 0;
}

/**
 * Command: import - Validate JSON import file
 *
 * Currently only validates the file format. To actually import,
 * users should copy apps to /etc/preheat.d/apps.list and reload.
 *
 * @param filepath  Input file path (default: preheat-profile.json)
 * @return          0 on success, 1 on error
 */
static int
cmd_import(const char *filepath)
{
    FILE *f;
    char line[2048];
    const char *inpath = filepath ? filepath : DEFAULT_EXPORT;
    int apps_found = 0;

    f = fopen(inpath, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open import file %s: %s\n", inpath, strerror(errno));
        return 1;
    }

    /* Simple validation: check for preheat_export_version */
    int valid = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "preheat_export_version")) {
            valid = 1;
        }
        if (strstr(line, "\"path\"")) {
            apps_found++;
        }
    }
    fclose(f);

    if (!valid) {
        fprintf(stderr, "Error: Invalid export file format\n");
        return 1;
    }

    printf("Found %d apps in %s\n", apps_found, inpath);
    printf("\nNote: Import currently validates the file only.\n");
    printf("To apply: copy the apps to your whitelist file at:\n");
    printf("  /etc/preheat.d/apps.list\n");
    printf("Then run: sudo preheat-ctl reload\n");

    return 0;
}

/**
 * Command: reload - Reload daemon configuration
 *
 * Sends SIGHUP to daemon, which triggers:
 *   - Re-read /etc/preheat.conf
 *   - Reload /etc/preheat.d/blacklist
 *   - Reopen log file (for log rotation)
 *
 * @return  0 on success, 1 on error
 */
static int
cmd_reload(void)
{
    int pid = read_pid();
    if (pid < 0)
        return 1;

    if (!check_running(pid)) {
        fprintf(stderr, "Error: %s is not running\n", PACKAGE);
        return 1;
    }

    return send_signal(pid, SIGHUP, "configuration reload requested");
}

/**
 * Command: dump - Dump state to log file
 *
 * Sends SIGUSR1 to daemon, which writes current state summary
 * to /var/log/preheat.log for debugging.
 *
 * @return  0 on success, 1 on error
 */
static int
cmd_dump(void)
{
    int pid = read_pid();
    if (pid < 0)
        return 1;

    if (!check_running(pid)) {
        fprintf(stderr, "Error: %s is not running\n", PACKAGE);
        return 1;
    }

    return send_signal(pid, SIGUSR1, "state dump requested");
}

/**
 * Command: save - Save state immediately
 *
 * Sends SIGUSR2 to daemon, which writes learned patterns to
 * state file immediately (normally happens on autosave timer).
 *
 * @return  0 on success, 1 on error
 */
static int
cmd_save(void)
{
    int pid = read_pid();
    if (pid < 0)
        return 1;

    if (!check_running(pid)) {
        fprintf(stderr, "Error: %s is not running\n", PACKAGE);
        return 1;
    }

    return send_signal(pid, SIGUSR2, "immediate save requested");
}

/**
 * Command: stop - Gracefully stop daemon
 *
 * Sends SIGTERM and waits up to 5 seconds for clean shutdown.
 * Daemon will save state before exiting.
 *
 * @return  0 on success, 1 if daemon didn't stop or error
 */
static int
cmd_stop(void)
{
    int pid = read_pid();
    if (pid < 0)
        return 1;

    if (!check_running(pid)) {
        fprintf(stderr, "Error: %s is not running\n", PACKAGE);
        return 1;
    }

    int ret = send_signal(pid, SIGTERM, "stop requested");
    if (ret == 0) {
        printf("Waiting for daemon to stop...\n");

        /* Wait up to 5 seconds */
        int i;
        for (i = 0; i < 50; i++) {
            usleep(100000);  /* 100ms */
            if (!check_running(pid)) {
                printf("%s stopped\n", PACKAGE);
                return 0;
            }
        }

        fprintf(stderr, "Warning: Daemon did not stop after 5 seconds\n");
        return 1;
    }

    return ret;
}

/**
 * Command: mem - Display memory statistics
 *
 * Reads /proc/meminfo and displays memory available for preloading.
 * Helps users understand why preloading might be skipped.
 *
 * @return  0 on success, 1 on error
 */
static int
cmd_mem(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot read /proc/meminfo: %s\n", strerror(errno));
        return 1;
    }

    char line[256];
    long mem_total = 0, mem_free = 0, mem_available = 0, mem_buffers = 0, mem_cached = 0;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemFree: %ld kB", &mem_free) == 1) continue;
        if (sscanf(line, "MemAvailable: %ld kB", &mem_available) == 1) continue;
        if (sscanf(line, "Buffers: %ld kB", &mem_buffers) == 1) continue;
        if (sscanf(line, "Cached: %ld kB", &mem_cached) == 1) continue;
    }
    fclose(f);

    printf("Memory Statistics\n");
    printf("=================\n");
    printf("Total:     %7ld MB\n", mem_total / 1024);
    printf("Free:      %7ld MB\n", mem_free / 1024);
    printf("Available: %7ld MB\n", mem_available / 1024);
    printf("Buffers:   %7ld MB\n", mem_buffers / 1024);
    printf("Cached:    %7ld MB\n", mem_cached / 1024);
    printf("\n");

    /* Calculate usable for preloading */
    long usable = mem_available > 0 ? mem_available : (mem_free + mem_buffers + mem_cached);
    printf("Usable for preloading: %ld MB\n", usable / 1024);

    return 0;
}

/**
 * Command: predict - Show top predicted applications
 *
 * Reads state file and displays apps sorted by usage time.
 * Shows which applications preheat has learned about.
 *
 * @param top_n  Maximum number of apps to display (default: 10)
 * @return       0 on success, 1 on error
 */
static int
cmd_predict(int top_n)
{
    /* Read state file to show predicted apps */
    /* For now, show message about how to use */
    printf("Top %d Predicted Applications\n", top_n);
    printf("=============================\n\n");

    /* Check if state file exists */
    FILE *f = fopen(STATEFILE, "r");
    int first_errno = errno;  /* Save errno before alternate attempt */
    if (!f) {
        /* Try alternate location */
        f = fopen("/var/lib/preheat/preheat.state", "r");
    }

    if (!f) {
        /* Check if first attempt was permission denied */
        if (first_errno == EACCES || first_errno == EPERM) {
            fprintf(stderr, "Error: Permission denied reading state file\n");
            fprintf(stderr, "Hint: Try with sudo\n");
        } else {
            printf("State file not found.\n");
            printf("The daemon needs to run and collect data first.\n");
            printf("\nHint: Start the daemon with 'systemctl start preheat'\n");
            printf("      or check state file location in config.\n");
        }
        return 1;
    }

    /* Count EXE entries in state file */
    char line[1024];
    int exe_count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "EXE\t", 4) == 0) {
            exe_count++;
            if (exe_count <= top_n) {
                /* Parse and display: EXE seq update_time time expansion path */
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
 * Helper: Add entry to configuration file with deduplication
 *
 * @param filepath  Path to config file
 * @param entry     Entry to add
 * @return          0 on success, 1 on error
 */
static int
add_to_config_file(const char *filepath, const char *entry)
{
    FILE *f;
    char line[512];
    int found = 0;

    /* Check if already exists */
    f = fopen(filepath, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            if (line[0] == '#' || line[0] == '\0') continue;
            if (strcmp(line, entry) == 0) {
                found = 1;
                break;
            }
        }
        fclose(f);
    }

    if (found) {
        printf("Entry already exists: %s\n", entry);
        return 0;
    }

    /* Append entry */
    f = fopen(filepath, "a");
    if (!f) {
        fprintf(stderr, "Error: Cannot write to %s: %s\n", filepath, strerror(errno));
        fprintf(stderr, "Hint: Try with sudo\n");
        return 1;
    }

    fprintf(f, "%s\n", entry);
    fclose(f);
    return 0;
}

/**
 * Helper: Remove entry from configuration file
 *
 * @param filepath  Path to config file
 * @param entry     Entry to remove
 * @return          0 on success, 1 on error
 */
static int
remove_from_config_file(const char *filepath, const char *entry)
{
    FILE *f_in, *f_out;
    char line[512], tmpfile[PATH_MAX];
    int removed = 0;

    f_in = fopen(filepath, "r");
    if (!f_in) {
        if (errno == ENOENT) {
            printf("File doesn't exist: %s\n", filepath);
            return 0;
        }
        fprintf(stderr, "Error: Cannot read %s: %s\n", filepath, strerror(errno));
        return 1;
    }

    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", filepath);
    f_out = fopen(tmpfile, "w");
    if (!f_out) {
        fprintf(stderr, "Error: Cannot create temp file: %s\n", strerror(errno));
        fprintf(stderr, "Hint: Try with sudo\n");
        fclose(f_in);
        return 1;
    }

    while (fgets(line, sizeof(line), f_in)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#' || strcmp(line, entry) != 0) {
            fprintf(f_out, "%s\n", line);
        } else {
            removed = 1;
        }
    }

    fclose(f_in);
    fclose(f_out);

    if (!removed) {
        unlink(tmpfile);
        printf("Entry not found: %s\n", entry);
        return 0;
    }

    if (rename(tmpfile, filepath) < 0) {
        fprintf(stderr, "Error: Cannot update file: %s\n", strerror(errno));
        unlink(tmpfile);
        return 1;
    }

    return 0;
}

/* ========================================================================
 * PATH MATCHING UTILITIES
 * ========================================================================
 * These functions handle the complexity of matching user-provided app names
 * against state file entries, which are stored as file:// URIs.
 * ======================================================================== */

/**
 * Convert file:// URI to plain filesystem path
 * Returns newly allocated string (caller must free) or NULL on error
 */
static char *
uri_to_path(const char *uri)
{
    if (!uri) return NULL;
    
    /* GLib's g_filename_from_uri handles URL decoding and validation */
    return g_filename_from_uri(uri, NULL, NULL);
}

/**
 * Check if string is a file:// URI
 */
static gboolean
is_uri(const char *str)
{
    return str && g_str_has_prefix(str, "file://");
}

/**
 * Check if two paths match, handling URIs, basenames, and partial paths
 * 
 * Matching layers (in order):
 *  1. Exact match - fastest path
 *  2. Substring match - handles partial paths
 *  3. Basename match - handles different install locations
 *
 * @param search_path  Path user is searching for (plain path)
 * @param state_path   Path from state file (might be URI)
 * @return TRUE if paths refer to same file
 */
static gboolean
paths_match(const char *search_path, const char *state_path)
{
    char *state_plain = NULL;
    char *search_basename = NULL;
    char *state_basename = NULL;
    gboolean match = FALSE;
    
    if (!search_path || !state_path) {
        return FALSE;
    }
    
    /* Convert URI to plain path if needed */
    if (is_uri(state_path)) {
        state_plain = uri_to_path(state_path);
        if (!state_plain) return FALSE;
    } else {
        state_plain = g_strdup(state_path);
    }
    
    /* Layer 1: Exact match (fastest) */
    if (strcmp(search_path, state_plain) == 0) {
        match = TRUE;
        goto cleanup;
    }
    
    /* Layer 2: Substring match (for partial paths like "bin/antigravity") */
    if (strstr(state_plain, search_path) || strstr(search_path, state_plain)) {
        match = TRUE;
        goto cleanup;
    }
    
    /* Layer 3: Basename match (handles "firefox" vs "/usr/lib/firefox") */
    {
        char *search_copy = g_strdup(search_path);
        char *state_copy = g_strdup(state_plain);
        
        search_basename = g_strdup(basename(search_copy));
        state_basename = g_strdup(basename(state_copy));
        
        g_free(search_copy);
        g_free(state_copy);
    }
    
    if (strcmp(search_basename, state_basename) == 0) {
        match = TRUE;
        goto cleanup;
    }
    
cleanup:
    g_free(state_plain);
    g_free(search_basename);
    g_free(state_basename);
    return match;
}

/**
 * Try to resolve app name to full path
 * Returns resolved path, or original name if not found
 * Now follows symlinks to find the actual binary
 */
static const char *
resolve_app_name(const char *name, char *buffer, size_t bufsize __attribute__((unused)))
{
    char temp_path[PATH_MAX];
    char *resolved = NULL;
    
    if (!name) {
        return name;
    }
    
    /* If already absolute, try to resolve it */
    if (name[0] == '/') {
        resolved = realpath(name, buffer);
        return resolved ? resolved : name;
    }
    
    /* Try common paths */
    snprintf(temp_path, sizeof(temp_path), "/usr/bin/%s", name);
    if (access(temp_path, F_OK) == 0) {
        resolved = realpath(temp_path, buffer);
        return resolved ? resolved : temp_path;
    }
    
    snprintf(temp_path, sizeof(temp_path), "/bin/%s", name);
    if (access(temp_path, F_OK) == 0) {
        resolved = realpath(temp_path, buffer);
        return resolved ? resolved : temp_path;
    }
    
    snprintf(temp_path, sizeof(temp_path), "/usr/local/bin/%s", name);
    if (access(temp_path, F_OK) == 0) {
        resolved = realpath(temp_path, buffer);
        return resolved ? resolved : temp_path;
    }
    
    return name;  /* Not found, use original */
}

/**
 * Command: promote - Add app to priority pool
 */
static int
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

    if (add_to_config_file("/etc/preheat.d/apps.list", final_name) != 0)
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
static int
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

    if (add_to_config_file("/etc/preheat.d/blacklist", final_name) != 0)
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
static int
cmd_reset(const char *app_name)
{
    if (!app_name || !*app_name) {
        fprintf(stderr, "Error: Missing application name\n");
        fprintf(stderr, "Usage: preheat-ctl reset APP\n");
        return 1;
    }

    remove_from_config_file("/etc/preheat.d/apps.list", app_name);
    remove_from_config_file("/etc/preheat.d/blacklist", app_name);

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
static int
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

            /* Parse new 9-field format:
             * EXE seq update_time time expansion pool weighted raw duration path */
            if (sscanf(line, "EXE\t%d\t%d\t%d\t%d\t%d\t%lf\t%lu\t%lu\t%511s",
                       &seq, &update_time, &run_time, &expansion, &pool,
                       &weighted_launches, &raw_launches, &total_duration, path) >= 9) {
                if (pool == 0) {  /* Observation pool (POOL_OBSERVATION) */
                    /* Convert URI to path for display */
                    const char *display_path = path;
                    if (strncmp(path, "file://", 7) == 0) {
                        display_path = path + 7;
                    }
                    printf("  %s\n", display_path);
                    count++;
                }
            }
        }
    }

    fclose(f);

    if (count == 0) {
        printf("  (no apps in observation pool yet)\n");
    }
    printf("\nTotal: %d apps\n", count);
    return 0;
}

/**
 * Main entry point - parse command and dispatch
 *
 * Each command maps to a cmd_* function that handles the specific
 * operation. Most commands require the daemon to be running.
 */
int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Error: No command specified\n\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "status") == 0) {
        return cmd_status();
    } else if (strcmp(cmd, "mem") == 0) {
        return cmd_mem();
    } else if (strcmp(cmd, "stats") == 0) {
        /* Check for --verbose flag */
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
                return cmd_stats_verbose();
            }
        }
        return cmd_stats();
    } else if (strcmp(cmd, "predict") == 0) {
        int top_n = 10;
        /* Parse --top N option */
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
                top_n = atoi(argv[i + 1]);
                if (top_n <= 0) top_n = 10;
                i++;
            }
        }
        return cmd_predict(top_n);
    } else if (strcmp(cmd, "reload") == 0) {
        return cmd_reload();
    } else if (strcmp(cmd, "dump") == 0) {
        return cmd_dump();
    } else if (strcmp(cmd, "save") == 0) {
        return cmd_save();
    } else if (strcmp(cmd, "stop") == 0) {
        return cmd_stop();
    } else if (strcmp(cmd, "pause") == 0) {
        const char *duration = (argc > 2) ? argv[2] : NULL;
        return cmd_pause(duration);
    } else if (strcmp(cmd, "resume") == 0) {
        return cmd_resume();
    } else if (strcmp(cmd, "export") == 0) {
        const char *filepath = (argc > 2) ? argv[2] : NULL;
        return cmd_export(filepath);
    } else if (strcmp(cmd, "import") == 0) {
        const char *filepath = (argc > 2) ? argv[2] : NULL;
        return cmd_import(filepath);
    } else if (strcmp(cmd, "update") == 0) {
        /* Execute update script */
        if (geteuid() != 0) {
            fprintf(stderr, "Error: Update requires root privileges\n");
            fprintf(stderr, "Try: sudo %s update\n", argv[0]);
            return 1;
        }

        /* Try multiple possible script locations */
        const char *script_locations[] = {
            "/usr/local/share/preheat/update.sh",  /* Installed location */
            "./scripts/update.sh",                  /* Development/source tree */
            NULL
        };

        for (int i = 0; script_locations[i] != NULL; i++) {
            if (access(script_locations[i], X_OK) == 0) {
                execl("/bin/bash", "bash", script_locations[i], NULL);
                /* If execl returns, it failed */
                perror("Failed to execute update script");
                return 1;
            }
        }

        /* No script found */
        fprintf(stderr, "Error: Update script not found\n");
        fprintf(stderr, "\nManual update procedure:\n");
        fprintf(stderr, "  1. cd /path/to/preheat-linux\n");
        fprintf(stderr, "  2. git pull\n");
        fprintf(stderr, "  3. autoreconf --install --force\n");
        fprintf(stderr, "  4. ./configure\n");
        fprintf(stderr, "  5. make\n");
        fprintf(stderr, "  6. sudo make install\n");
        fprintf(stderr, "  7. sudo systemctl restart preheat\n");
        return 1;
    } else if (strcmp(cmd, "promote") == 0) {
        const char *app_name = (argc > 2) ? argv[2] : NULL;
        return cmd_promote(app_name);
    } else if (strcmp(cmd, "demote") == 0) {
        const char *app_name = (argc > 2) ? argv[2] : NULL;
        return cmd_demote(app_name);
    } else if (strcmp(cmd, "reset") == 0) {
        const char *app_name = (argc > 2) ? argv[2] : NULL;
        return cmd_reset(app_name);
    } else if (strcmp(cmd, "show-hidden") == 0) {
        return cmd_show_hidden();
    } else if (strcmp(cmd, "explain") == 0) {
        const char *app_name = (argc > 2) ? argv[2] : NULL;
        return cmd_explain(app_name);
    } else if (strcmp(cmd, "health") == 0) {
        return cmd_health();
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}
