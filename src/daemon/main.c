/* main.c - Preheat daemon entry point
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * =============================================================================
 * MODULE OVERVIEW: Daemon Entry Point
 * =============================================================================
 *
 * This is the main entry point for the preheat daemon. It handles:
 *
 * COMMAND-LINE OPTIONS:
 *   -c, --conffile FILE    Configuration file
 *   -s, --statefile FILE   Learned state persistence file
 *   -l, --logfile FILE     Log output file
 *   -n, --nice LEVEL       Process priority (default: 15)
 *   -f, --foreground       Don't daemonize (for systemd Type=simple)
 *   -t, --self-test        Run diagnostics and exit
 *
 * STARTUP SEQUENCE:
 *   1. parse_cmdline()     → Process command-line arguments
 *   2. kp_log_init()       → Set up logging
 *   3. kp_config_load()    → Load configuration from INI file
 *   4. kp_blacklist_init() → Load application blacklist
 *   5. kp_session_init()   → Initialize session detection
 *   6. kp_signals_init()   → Set up signal handlers
 *   7. kp_daemonize()      → Fork to background (unless -f)
 *   8. kp_state_load()     → Load learned state from disk
 *   9. kp_daemon_run()     → Enter main event loop
 *
 * SHUTDOWN SEQUENCE:
 *   1. kp_state_save()     → Persist learned state
 *   2. kp_state_free()     → Release memory
 *   3. exit(0)
 *
 * SELF-TEST MODE (-t):
 *   Runs diagnostics without starting daemon:
 *   - /proc filesystem availability
 *   - readahead() system call support
 *   - Memory availability
 *   - Competing daemon detection
 *
 * =============================================================================
 */

#include "common.h"
#include "../utils/logging.h"
#include "../config/config.h"
#include "../config/blacklist.h"
#include "../utils/desktop.h"
#include "daemon.h"
#include "signals.h"
#include "session.h"
#include "stats.h"
#include "../state/state.h"

#include <getopt.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>     /* For strcspn() */
#include <sys/file.h>   /* For flock() */

/* Default file paths */
#define DEFAULT_CONFFILE SYSCONFDIR "/" PACKAGE ".conf"
#define DEFAULT_STATEFILE PKGLOCALSTATEDIR "/" PACKAGE ".state"
#define DEFAULT_LOGFILE LOGDIR "/" PACKAGE ".log"
#define DEFAULT_PIDFILE "/var/run/" PACKAGE ".pid"
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
extern void kp_state_register_manual_apps(void);

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
    printf("Report bugs to: https://github.com/wasteddreams/preheat-linux/issues\n");
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
 * Check if a process with the given name is running
 * Scans /proc directly instead of using popen() for security
 */
static gboolean
is_process_running(const char *process_name)
{
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        return FALSE;
    }

    struct dirent *entry;
    gboolean found = FALSE;

    while ((entry = readdir(proc_dir)) != NULL) {
        /* Skip non-numeric entries (only PIDs are numeric) */
        if (!isdigit(entry->d_name[0])) {
            continue;
        }

        /* Read the process name from /proc/PID/comm */
        char comm_path[512];
        snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", entry->d_name);

        FILE *comm_file = fopen(comm_path, "r");
        if (comm_file) {
            char comm[256];
            if (fgets(comm, sizeof(comm), comm_file)) {
                /* Remove trailing newline */
                comm[strcspn(comm, "\n")] = '\0';

                if (strcmp(comm, process_name) == 0) {
                    found = TRUE;
                    fclose(comm_file);
                    break;
                }
            }
            fclose(comm_file);
        }
    }

    closedir(proc_dir);
    return found;
}

/**
 * Acquire exclusive lock on PID file to ensure single instance
 * 
 * @param pidfile Path to PID file (e.g., /var/run/preheat.pid)
 * @return File descriptor (keep open for duration of daemon), or -1 on failure
 * 
 * The lock is automatically released when the process exits or the fd is closed.
 * If another instance is running, flock() will fail with EWOULDBLOCK.
 */
static int pidfile_fd = -1;  /* Global to keep lock held for daemon lifetime */

static gboolean
acquire_pidfile_lock(const char *pidfile)
{
    pidfile_fd = open(pidfile, O_RDWR | O_CREAT, 0644);
    if (pidfile_fd < 0) {
        /* Try /run as fallback (some systems use /run instead of /var/run) */
        if (errno == EACCES || errno == ENOENT) {
            g_warning("Cannot open PID file %s: %s (continuing without lock)", 
                      pidfile, strerror(errno));
            return TRUE;  /* Allow startup without lock on permission issues */
        }
        return FALSE;
    }
    
    /* Try exclusive non-blocking lock */
    if (flock(pidfile_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            /* Another instance is running */
            char buf[32] = {0};
            ssize_t n = read(pidfile_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                /* Trim trailing whitespace/newline */
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) {
                    buf[--n] = '\0';
                }
                fprintf(stderr, "Error: Another instance is already running (PID: %s)\n", buf);
            } else {
                fprintf(stderr, "Error: Another instance is already running\n");
            }
            close(pidfile_fd);
            pidfile_fd = -1;
            return FALSE;
        }
        /* Other flock errors - log but continue */
        g_warning("flock() failed: %s (continuing)", strerror(errno));
    }
    
    /* Write our PID to the file */
    if (ftruncate(pidfile_fd, 0) == 0) {
        char pidbuf[32];
        int len = snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
        if (write(pidfile_fd, pidbuf, len) < 0) {
            g_warning("Failed to write PID file: %s", strerror(errno));
        }
    }
    
    /* Keep fd open - lock is held until process exits */
    g_debug("PID file lock acquired: %s", pidfile);
    return TRUE;
}

/**
 * Release PID file lock and remove the file
 */
static void
release_pidfile_lock(void)
{
    if (pidfile_fd >= 0) {
        close(pidfile_fd);
        pidfile_fd = -1;
        unlink(DEFAULT_PIDFILE);  /* Clean up PID file */
        g_debug("PID file lock released");
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
        if (is_process_running("ureadahead")) {
            conflicts++;
            printf("\n   WARNING: ureadahead daemon is running");
        }
    }
    /* Check for preload (original) */
    if (is_process_running("preload")) {
        conflicts++;
        printf("\n   WARNING: preload daemon is running");
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

    /* Acquire PID file lock - ensures single instance */
    if (!acquire_pidfile_lock(DEFAULT_PIDFILE)) {
        g_critical("Cannot start: another instance is already running");
        return EXIT_FAILURE;
    }

    /* Load configuration */
    kp_config_load(conffile, TRUE);

    /* Initialize blacklist */
    kp_blacklist_init();
    
    /* Initialize desktop file scanner for GUI app discovery */
    kp_desktop_init();

    /* Initialize session detection */
    kp_session_init();

    /* Initialize statistics */
    kp_stats_init();

    kp_signals_init();

    if (!foreground)
        kp_daemonize();

    if (0 > nice(nicelevel))
        g_warning("nice: %s", strerror(errno));

    g_debug("starting up");

    /* Load state from file */
    kp_state_load(statefile);

    /* Reclassify all loaded apps (fixes cached pool values) */
    kp_stats_reclassify_all();

    /* Build Markov chains between priority apps (needed for prediction) */
    kp_markov_build_priority_mesh();

    /* Register manual apps that aren't already tracked */
    kp_state_register_manual_apps();

    /* Save state immediately so preheat-ctl commands work right away */
    kp_state->dirty = TRUE;  /* Ensure save actually writes */
    kp_state_save(statefile);

    g_message("%s %s started", PACKAGE, VERSION);

    /* Main loop */
    kp_daemon_run(statefile);

    /* Clean up */
    kp_state_save(statefile);
    kp_state_free();

    /* Release PID file lock */
    release_pidfile_lock();

    g_debug("exiting");
    return EXIT_SUCCESS;
}
