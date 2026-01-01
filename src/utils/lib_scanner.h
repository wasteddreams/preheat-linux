/* lib_scanner.h - Shared library discovery for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 */

#ifndef LIB_SCANNER_H
#define LIB_SCANNER_H

#include <limits.h>

/**
 * Scan executable for shared library dependencies using ldd
 * @param exe_path Path to executable
 * @return NULL-terminated array of library paths, or NULL on error
 *         Caller must free with kp_free_library_list()
 */
char **kp_scan_libraries(const char *exe_path);

/**
 * Free library list returned by kp_scan_libraries
 */
void kp_free_library_list(char **libs);

#endif /* LIB_SCANNER_H */
