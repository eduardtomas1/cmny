#ifndef CMNY_HISTORY_H
#define CMNY_HISTORY_H

#include "cmny.h"

#define CMNY_HISTORY_LIST_LIMIT 256U
#define CMNY_HISTORY_KEEP_LIMIT 100000U

typedef enum {
    CMNY_HISTORY_CREATE = 1,
    CMNY_HISTORY_UPDATE,
    CMNY_HISTORY_DELETE,
    CMNY_HISTORY_TRANSFER
} CmnyHistoryActionType;

typedef struct {
    int64_t id;
    int64_t entry_id;
    int64_t before_revision;
    int64_t after_revision;
    int64_t created_at;
    int64_t undone_at;
    CmnyHistoryActionType type;
    CmnyEntryType entry_type;
    bool undone;
} CmnyHistoryAction;

/* The caller owns the output array; history rows contain no borrowed pointers. */
bool cmny_history_list(CmnyDb *db, bool include_undone, size_t offset,
                       CmnyHistoryAction *out, size_t cap, size_t *count,
                       char *err, size_t err_size);
bool cmny_history_undo(CmnyDb *db, int64_t action_id, int64_t expected_after_revision,
                       int64_t *new_entry_revision, char *err, size_t err_size);
bool cmny_history_undo_latest(CmnyDb *db, int64_t *undone_action_id,
                              int64_t *new_entry_revision, char *err, size_t err_size);
bool cmny_history_prune(CmnyDb *db, size_t keep_newest, size_t *removed,
                        char *err, size_t err_size);

#endif
