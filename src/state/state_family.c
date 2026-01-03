/* state_family.c - Application family management for Preheat
 *
 * Copyright (C) 2025 Preheat Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * =============================================================================
 * MODULE: Application Families
 * =============================================================================
 *
 * Application families group related executables for better stat aggregation:
 *
 *   firefox-family: /usr/bin/firefox + /usr/bin/firefox-esr
 *   vscode-family:  /usr/bin/code + /usr/bin/code-insiders
 *
 * DISCOVERY METHODS:
 *   - CONFIG: User-defined in preheat.conf
 *   - AUTO: Detected via naming patterns (app-beta, app-dev, etc.)
 *   - MANUAL: Created via CLI command
 *
 * AGGREGATION:
 *   total_weighted_launches = sum(exe->weighted_launches for all members)
 *   last_used = max(exe->last_seen for all members)
 *
 * =============================================================================
 */

#include "common.h"
#include "state.h"
#include "state_family.h"

/**
 * Create new application family
 *
 * @param family_id  Unique identifier (e.g., "firefox")
 * @param method     How this family was discovered
 * @return           New family object (not yet registered)
 */
kp_app_family_t *
kp_family_new(const char *family_id, discovery_method_t method)
{
    kp_app_family_t *family;

    g_return_val_if_fail(family_id, NULL);

    family = g_new0(kp_app_family_t, 1);
    family->family_id = g_strdup(family_id);
    family->member_paths = g_ptr_array_new_with_free_func(g_free);
    family->method = method;
    
    /* Stats will be computed on demand */
    family->total_weighted_launches = 0.0;
    family->total_raw_launches = 0;
    family->last_used = 0;

    return family;
}

/**
 * Free application family
 */
void
kp_family_free(kp_app_family_t *family)
{
    g_return_if_fail(family);

    g_free(family->family_id);
    g_ptr_array_free(family->member_paths, TRUE);
    g_free(family);
}

/**
 * Add member to family
 *
 * @param family   Family to add to
 * @param exe_path Executable path to add as member
 */
void
kp_family_add_member(kp_app_family_t *family, const char *exe_path)
{
    g_return_if_fail(family);
    g_return_if_fail(exe_path);

    /* Check for duplicates */
    for (guint i = 0; i < family->member_paths->len; i++) {
        if (strcmp(g_ptr_array_index(family->member_paths, i), exe_path) == 0) {
            return;  /* Already a member */
        }
    }

    g_ptr_array_add(family->member_paths, g_strdup(exe_path));
    
    /* Register reverse mapping */
    if (kp_state->exe_to_family) {
        g_hash_table_insert(kp_state->exe_to_family, 
                            g_strdup(exe_path), 
                            g_strdup(family->family_id));
    }
}

/**
 * Update family statistics by aggregating from all members
 */
void
kp_family_update_stats(kp_app_family_t *family)
{
    g_return_if_fail(family);

    family->total_weighted_launches = 0.0;
    family->total_raw_launches = 0;
    family->last_used = 0;

    for (guint i = 0; i < family->member_paths->len; i++) {
        const char *exe_path = g_ptr_array_index(family->member_paths, i);
        kp_exe_t *exe = g_hash_table_lookup(kp_state->exes, exe_path);
        
        if (exe) {
            family->total_weighted_launches += exe->weighted_launches;
            family->total_raw_launches += exe->raw_launches;
            
            /* Track most recent usage - BUG 4 FIX: proper time_t comparison */
            if ((time_t)exe->running_timestamp > family->last_used) {
                family->last_used = exe->running_timestamp;
            }
        }
    }
}

/**
 * Lookup family by ID
 *
 * @param family_id  Family identifier
 * @return           Family object or NULL if not found
 */
kp_app_family_t *
kp_family_lookup(const char *family_id)
{
    g_return_val_if_fail(family_id, NULL);
    g_return_val_if_fail(kp_state->app_families, NULL);

    return g_hash_table_lookup(kp_state->app_families, family_id);
}

/**
 * Lookup family ID by executable path
 *
 * @param exe_path  Executable path
 * @return          Family ID or NULL if exe not in any family
 */
const char *
kp_family_lookup_by_exe(const char *exe_path)
{
    g_return_val_if_fail(exe_path, NULL);
    /* Note: kp_state is a static array, no NULL check needed */
    g_return_val_if_fail(kp_state->exe_to_family, NULL);

    return g_hash_table_lookup(kp_state->exe_to_family, exe_path);
}
