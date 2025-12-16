/* daemon.c - Daemon core implementation
 *
 * Based on preload 0.6.4 (VERBATIM daemonize logic)
 * Based on the preload daemon
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.h"
#include "daemon.h"
#include "../utils/logging.h"

#include <sys/types.h>
#include <sys/stat.h>

/* Global main loop */
GMainLoop *main_loop = NULL;

/* PID file path - use /run if available, fallback to localstatedir */
#ifndef RUNDIR
#define RUNDIR "/run"
#endif
#define PIDFILE RUNDIR "/" PACKAGE ".pid"

/* Forward declaration for state run function (to be implemented) */
extern void kp_state_run(const char *statefile);

/**
 * Daemonize the process
 * (VERBATIM from upstream preload daemonize)
 */
void
kp_daemonize(void)
{
    switch (fork()) {
        case -1:
            g_error("fork failed, exiting: %s", strerror(errno));
            exit(EXIT_FAILURE);
            break;
            
        case 0:
            /* child - continue */
            break;
            
        default:
            /* parent - exit */
            if (getpid() == 1) {
                /* Chain to /sbin/init if we are called as init! */
                execl("/sbin/init", "init", NULL);
                execl("/bin/init", "init", NULL);
            }
            exit(EXIT_SUCCESS);
    }

    /* Disconnect from controlling terminal */
    setsid();
    
    /* Set safe umask */
    umask(0007);

    /* Change to root directory to not block unmounts */
    (void) chdir("/");
    
    g_debug("daemonized successfully");
}

/**
 * Write PID file
 */
static void
kp_write_pidfile(void)
{
    FILE *f;
    
    f = fopen(PIDFILE, "w");
    if (!f) {
        g_warning("failed to create PID file %s: %s", PIDFILE, strerror(errno));
        return;
    }
    
    fprintf(f, "%d\n", getpid());
    fclose(f);
    
    /* Make PID file world-readable so preheat-ctl can read it */
    chmod(PIDFILE, 0644);
    
    g_debug("PID file created: %s", PIDFILE);
}

/**
 * Remove PID file
 */
static void
kp_remove_pidfile(void)
{
    if (unlink(PIDFILE) < 0) {
        if (errno != ENOENT)
            g_warning("failed to remove PID file %s: %s", PIDFILE, strerror(errno));
    } else {
        g_debug("PID file removed");
    }
}

/**
 * Check for competing preload daemons
 * Logs warnings if conflicts are detected
 */
static void
kp_check_competing_daemons(void)
{
    int conflicts = 0;
    
    /* Check for systemd-readahead */
    if (access("/run/systemd/readahead/", F_OK) == 0) {
        g_warning("Competing daemon detected: systemd-readahead is active");
        g_warning("  Remedy: Run 'systemctl disable systemd-readahead-collect systemd-readahead-replay'");
        conflicts++;
    }
    
    /* Check for ureadahead (Ubuntu) */
    FILE *pf = popen("pgrep -x ureadahead 2>/dev/null", "r");
    if (pf) {
        char buf[32];
        if (fgets(buf, sizeof(buf), pf)) {
            g_warning("Competing daemon detected: ureadahead (PID %s)", g_strstrip(buf));
            g_warning("  Remedy: Run 'systemctl disable ureadahead'");
            conflicts++;
        }
        pclose(pf);
    }
    
    /* Check for original preload daemon */
    pf = popen("pgrep -x preload 2>/dev/null", "r");
    if (pf) {
        char buf[32];
        if (fgets(buf, sizeof(buf), pf)) {
            g_warning("Competing daemon detected: preload (PID %s)", g_strstrip(buf));
            g_warning("  Remedy: Run 'systemctl disable preload' or 'apt remove preload'");
            conflicts++;
        }
        pclose(pf);
    }
    
    if (conflicts > 0) {
        g_warning("Found %d competing preload daemon(s). Performance may be affected.", conflicts);
        g_warning("Preheat will continue, but consider disabling conflicting services.");
    }
}

/**
 * Run main event loop
 * (Based on upstream preload main loop structure)
 */
void
kp_daemon_run(const char *statefile)
{
    g_debug("starting main event loop");
    
    /* Create PID file */
    kp_write_pidfile();
    
    /* Create main loop */
    main_loop = g_main_loop_new(NULL, FALSE);
    
    if (!main_loop) {
        g_error("failed to create main loop");
        kp_remove_pidfile();
        return;
    }
    
    /* Check for competing daemons at startup */
    kp_check_competing_daemons();
    
    /* Start state management (sets up periodic tasks) */
    kp_state_run(statefile);
    
    /* Run the loop - blocks until g_main_loop_quit() is called */
    g_main_loop_run(main_loop);
    
    g_debug("main loop exited");
    
    /* Cleanup */
    if (main_loop) {
        g_main_loop_unref(main_loop);
        main_loop = NULL;
    }
    
    /* Remove PID file */
    kp_remove_pidfile();
}
