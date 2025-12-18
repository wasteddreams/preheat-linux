/* preheat-ctl.c - CLI control tool for Preheat daemon
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _DEFAULT_SOURCE  /* For usleep() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#define PIDFILE "/var/run/preheat.pid"
#define STATEFILE "/usr/local/var/lib/preheat/preheat.state"
#define PACKAGE "preheat"

static void
print_usage(const char *prog)
{
    printf("Usage: %s COMMAND [OPTIONS]\n\n", prog);
    printf("Control the %s daemon\n\n", PACKAGE);
    printf("Commands:\n");
    printf("  status      Check if daemon is running\n");
    printf("  mem         Show memory statistics\n");
    printf("  predict     Show top predicted applications\n");
    printf("  reload      Reload configuration (send SIGHUP)\n");
    printf("  dump        Dump state to log (send SIGUSR1)\n");
    printf("  save        Save state immediately (send SIGUSR2)\n");
    printf("  stop        Stop daemon gracefully (send SIGTERM)\n");
    printf("  update      Update preheat to latest version\n");
    printf("  help        Show this help message\n");
    printf("\nOptions for predict:\n");
    printf("  --top N     Show top N predictions (default: 10)\n");
    printf("\n");
}

static int
read_pid(void)
{
    FILE *f;
    int pid = -1;
    
    f = fopen(PIDFILE, "r");
    if (!f) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: PID file %s not found\n", PIDFILE);
            fprintf(stderr, "Is %s running?\n", PACKAGE);
        } else {
            fprintf(stderr, "Error: Cannot read PID file %s: %s\n", 
                    PIDFILE, strerror(errno));
        }
        return -1;
    }
    
    if (fscanf(f, "%d", &pid) != 1) {
        fprintf(stderr, "Error: Invalid PID file format\n");
        fclose(f);
        return -1;
    }
    
    fclose(f);
    return pid;
}

static int
check_running(int pid)
{
    /* Use /proc/PID to check if process exists (works without root) */
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    
    if (access(proc_path, F_OK) == 0) {
        return 1;  /* Running */
    } else {
        return 0;  /* Not running */
    }
}

static int
send_signal(int pid, int sig, const char *action)
{
    if (kill(pid, sig) < 0) {
        fprintf(stderr, "Error: Failed to send signal to %s (PID %d): %s\n",
                PACKAGE, pid, strerror(errno));
        return 1;
    }
    
    printf("%s: %s\n", PACKAGE, action);
    return 0;
}

static int
cmd_status(void)
{
    int pid = read_pid();
    if (pid < 0)
        return 1;
    
    int status = check_running(pid);
    if (status == 1) {
        printf("%s is running (PID %d)\n", PACKAGE, pid);
        return 0;
    } else if (status == 0) {
        fprintf(stderr, "%s is not running (stale PID file?)\n", PACKAGE);
        return 1;
    } else {
        fprintf(stderr, "%s status unknown\n", PACKAGE);
        return 1;
    }
}

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

static int
cmd_predict(int top_n)
{
    /* Read state file to show predicted apps */
    /* For now, show message about how to use */
    printf("Top %d Predicted Applications\n", top_n);
    printf("=============================\n\n");
    
    /* Check if state file exists */
    FILE *f = fopen(STATEFILE, "r");
    if (!f) {
        /* Try alternate location */
        f = fopen("/var/lib/preheat/preheat.state", "r");
    }
    
    if (!f) {
        printf("State file not found.\n");
        printf("The daemon needs to run and collect data first.\n");
        printf("\nHint: Start the daemon with 'systemctl start preheat'\n");
        printf("      or check state file location in config.\n");
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
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}
