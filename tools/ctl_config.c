/* ctl_config.c - Config file manipulation utilities for preheat-ctl
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * =============================================================================
 * MODULE: Configuration Utilities
 * =============================================================================
 *
 * Provides utilities for manipulating configuration files:
 *   - Duration string parsing (30m, 2h, 1h30m, until-reboot)
 *   - Adding entries to config files with deduplication
 *   - Removing entries from config files
 *
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <libgen.h>

#include "ctl_config.h"

/**
 * Parse duration string like "30m", "2h", "1h30m", "until-reboot"
 * Returns seconds, 0 for until-reboot, -1 on error
 */
int
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
 * Helper: Add entry to configuration file with deduplication
 *
 * @param filepath  Path to config file
 * @param entry     Entry to add
 * @return          0 on success, 1 on error
 */
int
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

    /* Create parent directory if it doesn't exist */
    char *path_copy = strdup(filepath);
    if (path_copy) {
        char *dir = dirname(path_copy);
        if (access(dir, F_OK) != 0) {
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "Error: Cannot create directory %s: %s\n", dir, strerror(errno));
                fprintf(stderr, "Hint: Try with sudo\n");
                free(path_copy);
                return 1;
            }
        }
        free(path_copy);
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
int
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
