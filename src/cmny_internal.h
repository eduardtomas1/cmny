#ifndef CMNY_INTERNAL_H
#define CMNY_INTERNAL_H

#include "cmny.h"
#include "cmny_history.h"

bool cmny_migrate_v3(sqlite3 *handle, char *err, size_t err_size);
bool cmny_migrate_v4(sqlite3 *handle, char *err, size_t err_size);
bool cmny_migrate_v5(sqlite3 *handle, char *err, size_t err_size);
bool cmny_ledger_integrity_check(CmnyDb *db, char *err, size_t err_size);
bool cmny_history_integrity_check(CmnyDb *db, char *err, size_t err_size);
bool cmny_reconcile_integrity_check(CmnyDb *db, char *err, size_t err_size);
bool cmny_rules_integrity_check(CmnyDb *db, char *err, size_t err_size);
bool cmny_import_integrity_check(CmnyDb *db, char *err, size_t err_size);

bool cmny_history_action_begin(CmnyDb *db, CmnyHistoryActionType type, int64_t entry_id,
                               bool capture_before, int64_t *action_id,
                               char *err, size_t err_size);
bool cmny_history_action_finish(CmnyDb *db, int64_t action_id, int64_t entry_id,
                                char *err, size_t err_size);

bool cmny_legacy_transaction_add(CmnyDb *db, const CmnyTransaction *tx, int64_t *new_id,
                                 char *err, size_t err_size);
bool cmny_legacy_transaction_update(CmnyDb *db, const CmnyTransaction *tx,
                                    char *err, size_t err_size);
bool cmny_legacy_transaction_delete(CmnyDb *db, int64_t id,
                                    char *err, size_t err_size);
bool cmny_internal_category_ensure(CmnyDb *db, const char *name, int kind_mask,
                                   int64_t *category_id, char *err, size_t err_size);

#endif
