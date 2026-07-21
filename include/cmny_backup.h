#ifndef CMNY_BACKUP_H
#define CMNY_BACKUP_H

#include "cmny.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CMNY_BACKUP_MAX_RETENTION 100U

/*
 * Create a complete backup under an exact CMNY-generated name, then retain at
 * most retention valid CMNY backups for this database basename. The result
 * buffer is cleared on failure and receives the published path on success.
 */
bool cmny_backup_create_rotating(CmnyDb *db, const char *database_path,
                                 int64_t timestamp, size_t retention,
                                 char *created_path, size_t created_path_size,
                                 char *err, size_t err_size);

/* A successful not-due result sets *created false and clears created_path. */
bool cmny_backup_run_if_due(CmnyDb *db, const char *database_path,
                            int64_t timestamp, unsigned interval_hours,
                            size_t retention, bool *created,
                            char *created_path, size_t created_path_size,
                            char *err, size_t err_size);

#endif
