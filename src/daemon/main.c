/* main.c - Preheat daemon entry point
 *
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
#include "../config/config.h"
#include "daemon.h"
#include "signals.h"

#include <getopt.h>
#include <dirent.h>
#include <fcntl.h>

/* Default file paths */
#define DEFAULT_CONFFILE SYSCONFDIR "/" PACKAGE ".conf"
#define DEFAULT_STATEFILE PKGLOCALSTATEDIR "/" PACKAGE ".state"
#define DEFAULT_LOGFILE LOGDIR "/" PACKAGE ".log"
#define DEFAULT_NICELEVEL 15

/* Global variables (accessed by other modules) */
const char *conffile = DEFAULT_CONFFILE;
const char *statefile = DEFAULT_STATEFILE;
const char *logfile = DEFAULT_LOGFILE;
int nicelevel = DEFAULT_NICELEVEL;
int foreground = 0;
int selftest = 0;

/* Forward declarations for functions to be implemented */
extern void kp_config_load(const char *conffile, gboolean is_startup);
extern void kp_state_load(const char *statefile);
extern void kp_state_save(const char *statefile);
extern void kp_state_free(void);

static void
print_version(void)
{
    printf("%s %s\n", PACKAGE, VERSION);
    printf("Adaptive readahead daemon for Debian-based distributions\n");
    printf("Based on the preload daemon\n\n");
    printf("Copyright (C) 2025 Preheat Contributors\n");
    printf("This is free software; see the source for copying conditions.\n");
}

static void
print_help(void)
{
    printf("Usage: %s [OPTIONS]\n\n", PACKAGE);
    printf("Adaptive readahead daemon for Debian-based distributions\n\n");
    printf("Options:\n");
    printf("  -c, --conffile FILE    Configuration file (default: %s)\n", DEFAULT_CONFFILE);
    printf("  -s, --statefile FILE   State file (default: %s)\n", DEFAULT_STATEFILE);
    printf("  -l, --logfile FILE     Log file (default: %s)\n", DEFAULT_LOGFILE);
    printf("  -n, --nice LEVEL       Nice level (default: %d)\n", DEFAULT_NICELEVEL);
    printf("  -f, --foreground       Run in foreground (don't daemonize)\n");
    printf("  -t, --self-test        Run self-diagnostics and exit\n");
    printf("  -h, --help             Show this help message\n");
    printf("  -v, --version          Show version information\n");
    printf("\n");
    printf("Signals:\n");
    printf("  SIGHUP                 Reload configuration and reopen log\n");
    printf("  SIGUSR1                Dump current state to log\n");
    printf("  SIGUSR2                Save state immediately\n");
    printf("  SIGTERM, SIGINT        Graceful shutdown\n");
    printf("\n");
    printf("Report bugs to: https://github.com/wasteddreams/preheat/issues\n");
}

static void
parse_cmdline(int *argc, char ***argv)
{
    static struct option long_options[] = {
        {"conffile",   required_argument, NULL, 'c'},
        {"statefile",  required_argument, NULL, 's'},
        {"logfile",    required_argument, NULL, 'l'},
        {"nice",       required_argument, NULL, 'n'},
        {"foreground", no_argument,       NULL, 'f'},
        {"self-test",  no_argument,       NULL, 't'},
        {"help",       no_argument,       NULL, 'h'},
        {"version",    no_argument,       NULL, 'v'},
        {NULL,         0,                 NULL,  0 }
    };

    int c;
    while ((c = getopt_long(*argc, *argv, "c:s:l:n:fthv", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                conffile = optarg;
                break;
            case 's':
                statefile = optarg;
                break;
            case 'l':
                logfile = optarg;
                break;
            case 'n':
                nicelevel = atoi(optarg);
                break;
            case 'f':
                foreground = 1;
                break;
            case 't':
                selftest = 1;
                break;
            case 'h':
                print_help();
                exit(EXIT_SUCCESS);
            case 'v':
                print_version();
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", PACKAGE);
                exit(EXIT_FAILURE);
        }
    }
}

/**
 * Run self-diagnostics
 * Checks system requirements without starting daemon
 */
static int
run_self_test(void)
{
    int passed = 0;
    int failed = 0;
    
    printf("Preheat Self-Test Diagnostics\n");
    printf("=============================\n\n");
    
    /* Check 1: /proc availability */
    printf("1. /proc filesystem... ");
    DIR *proc = opendir("/proc");
    if (proc) {
        closedir(proc);
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL (/proc not accessible: %s)\n", strerror(errno));
        printf("   Remedy: Ensure /proc is mounted\n");
        failed++;
    }
    
    /* Check 2: readahead() syscall (try on ourself) */
    printf("2. readahead() system call... ");
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd >= 0) {
        /* readahead returns 0 on success, -1 on error */
        if (readahead(fd, 0, 1024) >= 0 || errno == EINVAL) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL (%s)\n", strerror(errno));
            printf("   Remedy: Kernel may not support readahead\n");
            failed++;
        }
        close(fd);
    } else {
        printf("FAIL (cannot open test file)\n");
        failed++;
    }
    
    /* Check 3: Memory thresholds */
    printf("3. Memory availability... ");
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo) {
        char line[256];
        long mem_total = 0, mem_available = 0;
        while (fgets(line, sizeof(line), meminfo)) {
            if (sscanf(line, "MemTotal: %ld kB", &mem_total) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld kB", &mem_available) == 1) break;
        }
        fclose(meminfo);
        
        if (mem_available > 0) {
            printf("PASS (%ld MB available)\n", mem_available / 1024);
            passed++;
        } else if (mem_total > 0) {
            printf("PASS (total: %ld MB, available unknown)\n", mem_total / 1024);
            passed++;
        } else {
            printf("FAIL (cannot read memory info)\n");
            failed++;
        }
    } else {
        printf("FAIL (/proc/meminfo not accessible)\n");
        failed++;
    }
    
    /* Check 4: Competing daemons */
    printf("4. Competing preload daemons... ");
    int conflicts = 0;
    /* Check for systemd-readahead */
    if (access("/run/systemd/readahead/", F_OK) == 0) {
        conflicts++;
        printf("\n   WARNING: systemd-readahead detected\n");
    }
    /* Check for ureadahead */
    if (access("/sbin/ureadahead", F_OK) == 0) {
        FILE *pf = popen("pgrep -x ureadahead 2>/dev/null", "r");
        if (pf) {
            char buf[32];
            if (fgets(buf, sizeof(buf), pf)) {
                conflicts++;
                printf("\n   WARNING: ureadahead running (PID %s)", buf);
            }
            pclose(pf);
        }
    }
    /* Check for preload (original) */
    {
        FILE *pf = popen("pgrep -x preload 2>/dev/null", "r");
        if (pf) {
            char buf[32];
            if (fgets(buf, sizeof(buf), pf)) {
                conflicts++;
                printf("\n   WARNING: preload daemon running (PID %s)", buf);
            }
            pclose(pf);
        }
    }
    if (conflicts == 0) {
        printf("PASS (no conflicts detected)\n");
        passed++;
    } else {
        printf("   %d potential conflict(s) found\n", conflicts);
        printf("   Remedy: Disable conflicting daemons to avoid interference\n");
        /* Still count as pass but with warnings */
        passed++;
    }
    
    /* Summary */
    printf("\n=============================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    
    if (failed == 0) {
        printf("\nAll checks passed. Preheat is ready to run.\n");
        return 0;
    } else {
        printf("\nSome checks failed. Please address the issues above.\n");
        return 1;
    }
}

/**
 * Main entry point
 * (Structure from upstream preload main)
 */
int
main(int argc, char **argv)
{
    /* Initialize */
    parse_cmdline(&argc, &argv);
    
    /* Self-test mode: run diagnostics and exit */
    if (selftest) {
        return run_self_test();
    }
    
    kp_log_init(logfile);
    
    /* Load configuration */
    kp_config_load(conffile, TRUE);
    
    kp_signals_init();
    
    if (!foreground)
        kp_daemonize();
    
    if (0 > nice(nicelevel))
        g_warning("nice: %s", strerror(errno));
    
    g_debug("starting up");
    
    /* Load state from file */
    kp_state_load(statefile);
    
    g_message("%s %s started", PACKAGE, VERSION);
    
    /* Main loop */
    kp_daemon_run(statefile);
    
    /* Clean up */
    kp_state_save(statefile);
    kp_state_free();
    
    g_debug("exiting");
    return EXIT_SUCCESS;
}
