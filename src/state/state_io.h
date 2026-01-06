/* state_io.h - State file I/O for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * =============================================================================
 * MODULE: State File I/O
 * =============================================================================
 *
 * This module handles reading and writing the persistent state file.
 *
 * STATE FILE FORMAT:
 *   Text-based, line-oriented format with tags:
 *   - PRELOAD <version> <time>  - Header with format version
 *   - MAP <seq> <path> <offset> <length> - Memory map region
 *   - BADEXE <time> <size> <path> - Blacklisted small executable
 *   - EXE <seq> <time> <run_time> <path> - Tracked executable
 *   - EXEMAP <exe_seq> <map_seq> <prob> - Exe-to-map association
 *   - MARKOV <exe_a_seq> <exe_b_seq> <time> <prob_matrix> - Correlation
 *   - CRC32 <checksum> - Integrity verification footer
 *
 * =============================================================================
 */

#ifndef STATE_IO_H
#define STATE_IO_H

#include "state.h"

/* Internal read function - called from kp_state_load */
char *kp_state_read_from_channel(GIOChannel *f);

/* Internal write function - called from kp_state_save */
char *kp_state_write_to_channel(GIOChannel *f, int fd);

/* Handle corrupt state file */
gboolean kp_state_handle_corrupt_file(const char *statefile, const char *reason);

/* Clean up old broken state files (older than max_age_hours) */
void kp_state_cleanup_old_broken_files(const char *statefile, int max_age_hours);

#endif /* STATE_IO_H */
