/* logging.h - Preheat logging system
 *
 * Based on preload 0.6.4 log.h
 * Based on the preload daemon
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef LOGGING_H
#define LOGGING_H

#include <glib.h>

/**
 * Current log level
 * Higher values = more verbose
 * Default: 4 (standard messages)
 */
extern int kp_log_level;

/**
 * Initialize logging system
 * Opens log file and redirects stdout/stderr
 * 
 * @param logfile Path to log file, or NULL/empty for stderr only
 */
void kp_log_init(const char *logfile);

/**
 * Reopen log file
 * Used after log rotation (SIGHUP handler)
 * 
 * @param logfile Path to log file
 */
void kp_log_reopen(const char *logfile);

/**
 * Check if debug logging is enabled
 * @return TRUE if debug messages will be logged
 */
#define kp_is_debugging() (kp_log_level >= G_LOG_LEVEL_DEBUG)

#endif /* LOGGING_H */
