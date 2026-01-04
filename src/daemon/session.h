/* session.h - Session-aware preloading for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef SESSION_H
#define SESSION_H

#include <glib.h>
#include <time.h>

/**
 * Initialize session detection subsystem
 */
void kp_session_init(void);

/**
 * Check for user session start
 * Call this periodically to detect login events
 * @return TRUE if session just started, FALSE otherwise
 */
gboolean kp_session_check(void);

/**
 * Check if currently in boot/login window
 * @return TRUE if aggressive preloading should occur
 */
gboolean kp_session_in_boot_window(void);

/**
 * Get remaining seconds in boot window
 * @return Seconds remaining, 0 if not in window
 */
int kp_session_window_remaining(void);

/**
 * Trigger aggressive preload of top N apps (includes map loading)
 * @param max_apps Maximum apps to preload
 */
void kp_session_preload_top_apps(int max_apps);

/**
 * Boost top N apps with high priority lnprob for prediction
 * Called from kp_prophet_predict() during boot window
 * @param max_apps Maximum apps to boost
 */
void kp_session_boost_top_apps(int max_apps);

/**
 * Free session resources
 */
void kp_session_free(void);

#endif /* SESSION_H */
