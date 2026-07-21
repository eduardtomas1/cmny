#ifndef CMNY_RECONCILE_H
#define CMNY_RECONCILE_H

#include "cmny.h"

#define CMNY_RECONCILE_LIST_LIMIT 256U

typedef enum {
    CMNY_RECONCILE_OPEN = 1,
    CMNY_RECONCILE_FINALIZED,
    CMNY_RECONCILE_CANCELLED
} CmnyReconcileStatus;

typedef enum {
    CMNY_POSTING_UNCLEARED = 0,
    CMNY_POSTING_CLEARED,
    CMNY_POSTING_RECONCILED
} CmnyPostingClearState;

typedef struct {
    int64_t id;
    int64_t account_id;
    int64_t statement_balance_minor;
    int64_t account_balance_minor;
    int64_t cleared_balance_minor;
    int64_t discrepancy_minor;
    int64_t revision;
    int64_t created_at;
    int64_t finalized_at;
    CmnyReconcileStatus status;
    char statement_on[11];
} CmnyReconcileSession;

typedef struct {
    int64_t posting_id;
    int64_t entry_id;
    int64_t amount_minor;
    int sort_order;
    CmnyPostingClearState clear_state;
    bool selected;
    char occurred_on[11];
    char payee[CMNY_PAYEE_MAX + 1];
} CmnyReconcilePosting;

bool cmny_reconcile_start(CmnyDb *db, int64_t account_id, const char *statement_on,
                          int64_t statement_balance_minor, int64_t *session_id,
                          char *err, size_t err_size);
bool cmny_reconcile_get(CmnyDb *db, int64_t session_id, CmnyReconcileSession *out,
                        char *err, size_t err_size);
bool cmny_reconcile_list(CmnyDb *db, bool include_closed, size_t offset,
                         CmnyReconcileSession *out, size_t cap, size_t *count,
                         char *err, size_t err_size);
/* The caller owns the output array; rows contain no borrowed pointers. */
bool cmny_reconcile_postings(CmnyDb *db, int64_t session_id, size_t offset,
                             CmnyReconcilePosting *out, size_t cap, size_t *count,
                             char *err, size_t err_size);
bool cmny_reconcile_set_cleared(CmnyDb *db, int64_t session_id,
                                int64_t expected_session_revision, int64_t posting_id,
                                bool cleared, int64_t *new_session_revision,
                                char *err, size_t err_size);
bool cmny_reconcile_finalize(CmnyDb *db, int64_t session_id,
                             int64_t expected_session_revision,
                             char *err, size_t err_size);
bool cmny_reconcile_cancel(CmnyDb *db, int64_t session_id,
                           int64_t expected_session_revision,
                           char *err, size_t err_size);

#endif
