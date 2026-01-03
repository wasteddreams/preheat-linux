/* ctl_cmd_stats.c - Statistics and monitoring commands
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Commands: stats, stats_verbose, health, mem
 */

#define _DEFAULT_SOURCE  /* For usleep() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "ctl_commands.h"
#include "ctl_daemon.h"
#include "ctl_display.h"

/* File paths */
#define STATSFILE "/run/preheat.stats"
#define PACKAGE "preheat"

/**
 * Command: stats - Display preload statistics
 */
int
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

    usleep(200000);  /* Wait for stats file */

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

    int hours = uptime / 3600;
    int mins = (uptime % 3600) / 60;

    printf("  Uptime:       %dh %dm\n", hours, mins);
    printf("  Apps tracked: %d\n\n", apps);

    printf("  Preload Events:\n");
    printf("    Total:   %lu\n", preloads);
    printf("    Hits:    %lu\n", hits);
    printf("    Misses:  %lu\n\n", misses);

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
 * Command: stats --verbose - Display detailed statistics
 */
int
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

    if (kill(pid, SIGUSR1) < 0) {
        if (errno == EPERM) {
            fprintf(stderr, "Error: Permission denied\n");
            fprintf(stderr, "Hint: Try with sudo\n");
        } else {
            fprintf(stderr, "Error: %s\n", strerror(errno));
        }
        return 1;
    }

    usleep(200000);

    f = fopen(STATSFILE, "r");
    if (!f) {
        fprintf(stderr, "Error: Stats file not available yet\n");
        return 1;
    }

    /* Parse all metrics */
    char version[64] = "unknown";
    unsigned long hits = 0, misses = 0, preloads = 0, mem_pressure = 0;
    int uptime = 0, apps = 0, priority_pool = 0, observation_pool = 0;
    size_t total_mb = 0;
    double hit_rate = 0;
    
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
        
        /* Parse top apps */
        if (strncmp(line, "top_app_", 8) == 0 && num_top_apps < 20) {
            char *eq = strchr(line, '=');
            if (eq) {
                eq++;
                char *name = eq;
                char *colon1 = strchr(name, ':');
                if (colon1) {
                    *colon1 = '\0';
                    if (strlen(name) >= sizeof(top_apps[0].name)) continue;
                    
                    strncpy(top_apps[num_top_apps].name, name, sizeof(top_apps[0].name) - 1);
                    top_apps[num_top_apps].name[sizeof(top_apps[0].name) - 1] = '\0';
                    
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
                                char *newline = strchr(pool_str, '\n');
                                if (newline) *newline = '\0';
                                
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

    printf("\n  Preheat Statistics (Verbose)\n");
    printf("  ==============================\n\n");

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

    char preloads_fmt[32], hits_fmt[32], misses_fmt[32];
    format_number(preloads_fmt, preloads);
    format_number(hits_fmt, hits);
    format_number(misses_fmt, misses);
    
    printf("  Performance:\n");
    printf("    Preloads:     %s total\n", preloads_fmt);
    
    if (hits + misses > 0) {
        printf("    Hits:         %s (%.1f%%)\n", hits_fmt, hit_rate);
        printf("    Misses:       %s (%.1f%%)\n", misses_fmt, 100.0 - hit_rate);
        printf("    Efficiency:   ");
        if (hit_rate >= 70.0) printf("EXCELLENT\n\n");
        else if (hit_rate >= 50.0) printf("GOOD\n\n");
        else if (hit_rate >= 30.0) printf("LEARNING\n\n");
        else printf("EARLY STAGE\n\n");
    } else {
        printf("    Hits:         %s (N/A)\n", hits_fmt);
        printf("    Misses:       %s (N/A)\n", misses_fmt);
        printf("    Efficiency:   NO DATA (launch apps to collect stats)\n\n");
    }

    printf("  Memory:\n");
    printf("    Total Preloaded:  %zu MB\n", total_mb);
    if (num_top_apps > 0 && total_mb > 0) {
        printf("    Avg Size:         %zu MB per app\n", total_mb / (num_top_apps > 0 ? num_top_apps : 1));
    }
    printf("    Pressure Events:  %lu", mem_pressure);
    if (mem_pressure > 0) printf(" (skipped due to low memory)\n\n");
    else printf("\n\n");

    printf("  Pool Breakdown:\n");
    printf("    Priority:     %d apps (actively preloaded)\n", priority_pool);
    printf("    Observation:  %d apps (tracked only)\n\n", observation_pool);

    if (num_top_apps > 0) {
        printf("  Top Apps by Activity:\n");
        printf("    Rank  %-20s  Weighted  Raw    Pool\n", "App");
        printf("    ────  ────────────────────  ────────  ─────  ────────\n");
        
        for (int i = 0; i < num_top_apps && i < 20; i++) {
            printf("    %-4d  %-20s  %8.1f  %5lu  %s\n",
                   i + 1, top_apps[i].name, top_apps[i].weighted,
                   top_apps[i].raw, top_apps[i].pool);
        }
    } else {
        printf("  No apps tracked yet\n");
    }

    printf("\n");
    return 0;
}

/**
 * Command: health - Quick system health check
 */
int
cmd_health(void)
{
    int pid = read_pid();
    FILE *f;
    char line[512];
    int health_score = 0;
    int issues = 0;
    
    if (pid > 0 && check_running(pid)) {
        health_score += 40;
    } else {
        printf("❌ CRITICAL - Preheat daemon is not running\n\n");
        printf("  Daemon:       Not Running\n");
        printf("  Status:       Service is down\n\n");
        printf("  Action Required:\n");
        printf("    sudo systemctl start preheat\n\n");
        return 2;
    }
    
    kill(pid, SIGUSR1);
    usleep(200000);
    
    f = fopen(STATSFILE, "r");
    if (!f) {
        printf("⚠️  DEGRADED - Preheat is running but stats unavailable\n\n");
        printf("  Daemon:       Running (PID %d)\n", pid);
        printf("  Hit Rate:     Unknown (stats file missing)\n");
        printf("  Status:       Degraded\n\n");
        return 1;
    }
    
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
    
    int days_running = uptime / 86400;
    if (days_running >= 1 && (hits + misses) > 10) {
        if (hit_rate >= 70.0) health_score += 40;
        else if (hit_rate >= 50.0) health_score += 30;
        else if (hit_rate >= 30.0) { health_score += 20; issues++; }
        else { health_score += 10; issues++; }
    } else {
        health_score += 25;
    }
    
    if (mem_pressure == 0) health_score += 10;
    else if (mem_pressure < 10) health_score += 5;
    else issues++;
    
    struct stat st;
    if (stat("/usr/local/var/lib/preheat/preheat.state", &st) == 0) {  /* BUG 2 FIX */
        time_t now = time(NULL);
        time_t age_minutes = (now - st.st_mtime) / 60;
        if (age_minutes < 60) health_score += 10;
        else if (age_minutes < 1440) health_score += 5;
        else issues++;
    }
    
    const char *status;
    const char *emoji;
    int exit_code;
    
    if (health_score >= 90) { status = "EXCELLENT"; emoji = "✅"; exit_code = 0; }
    else if (health_score >= 70) { status = "GOOD"; emoji = "✅"; exit_code = 0; }
    else if (health_score >= 50) { status = "DEGRADED"; emoji = "⚠️ "; exit_code = 1; }
    else { status = "CRITICAL"; emoji = "❌"; exit_code = 2; }
    
    printf("%s %s - Preheat is %s\n\n", emoji, status, 
           exit_code == 0 ? "operating optimally" : 
           exit_code == 1 ? "experiencing issues" : "critically degraded");
    
    printf("  Daemon:       Running (PID %d)\n", pid);
    
    if (hits + misses > 0) {
        printf("  Hit Rate:     %.1f%%", hit_rate);
        if (hit_rate >= 70.0) printf(" (excellent)\n");
        else if (hit_rate >= 50.0) printf(" (good)\n");
        else if (hit_rate >= 30.0) printf(" (learning)\n");
        else printf(" (needs improvement)\n");
    } else {
        printf("  Hit Rate:     No data yet\n");
    }
    
    if (mem_pressure > 0) printf("  Memory:       %lu pressure events\n", mem_pressure);
    
    printf("  Uptime:       ");
    if (days_running > 0) printf("%dd %dh\n", days_running, (uptime % 86400) / 3600);
    else printf("%dh %dm\n", uptime / 3600, (uptime % 3600) / 60);
    
    if (exit_code == 0) {
        printf("\n  Status: All systems operational\n");
    } else if (exit_code == 1) {
        printf("\n  Issues Detected: %d\n", issues);
        if (hit_rate < 30.0 && days_running >= 7)
            printf("    - Hit rate below optimal (check configuration)\n");
        if (mem_pressure > 10)
            printf("    - Frequent memory pressure (consider increasing available memory)\n");
    }
    
    printf("\n");
    return exit_code;
}

/**
 * Command: mem - Display memory statistics
 */
int
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

    long usable = mem_available > 0 ? mem_available : (mem_free + mem_buffers + mem_cached);
    printf("Usable for preloading: %ld MB\n", usable / 1024);

    return 0;
}
