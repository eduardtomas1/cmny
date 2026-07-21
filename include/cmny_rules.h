#ifndef CMNY_RULES_H
#define CMNY_RULES_H

#include "cmny.h"
#include "cmny_bank_csv.h"

#define CMNY_RULE_NAME_MAX 64U
#define CMNY_RULE_PAYEE_PATTERN_MAX (CMNY_BANK_PAYEE_CAP - 1U)
#define CMNY_RULE_NOTE_PATTERN_MAX (CMNY_BANK_NOTE_CAP - 1U)
#define CMNY_RULE_LIST_LIMIT 256U

typedef enum {
    CMNY_RULE_TEXT_ANY = 0,
    CMNY_RULE_TEXT_EXACT,
    CMNY_RULE_TEXT_CONTAINS
} CmnyRuleTextMode;

typedef struct {
    int64_t account_id; /* Zero matches every account. */
    int64_t category_id;
    int64_t tag_id; /* Zero applies no tag. */
    int64_t minimum_amount_minor;
    int64_t maximum_amount_minor;
    int sort_order;
    CmnyRuleTextMode payee_mode;
    CmnyRuleTextMode note_mode;
    bool enabled;
    bool has_minimum_amount;
    bool has_maximum_amount;
    const char *name;
    const char *payee_pattern;
    const char *note_pattern;
} CmnyRuleDraft;

typedef struct {
    int64_t id;
    int64_t account_id;
    int64_t category_id;
    int64_t tag_id;
    int64_t minimum_amount_minor;
    int64_t maximum_amount_minor;
    int64_t revision;
    int64_t created_at;
    int64_t updated_at;
    int sort_order;
    CmnyRuleTextMode payee_mode;
    CmnyRuleTextMode note_mode;
    bool enabled;
    bool has_minimum_amount;
    bool has_maximum_amount;
    char name[CMNY_RULE_NAME_MAX + 1U];
    char payee_pattern[CMNY_RULE_PAYEE_PATTERN_MAX + 1U];
    char note_pattern[CMNY_RULE_NOTE_PATTERN_MAX + 1U];
} CmnyRule;

typedef struct {
    int64_t rule_id;
    int64_t category_id;
    int64_t tag_id;
    bool matched;
} CmnyRuleMatch;

bool cmny_rule_create(CmnyDb *db, const CmnyRuleDraft *draft, int64_t *new_id,
                      char *err, size_t err_size);
bool cmny_rule_get(CmnyDb *db, int64_t id, CmnyRule *out,
                   char *err, size_t err_size);
bool cmny_rule_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                      const CmnyRuleDraft *draft, char *err, size_t err_size);
bool cmny_rule_delete(CmnyDb *db, int64_t id, int64_t expected_revision,
                      char *err, size_t err_size);
bool cmny_rule_list(CmnyDb *db, size_t offset, CmnyRule *out, size_t cap,
                    size_t *count, char *err, size_t err_size);

/* Matching is bytewise and case-sensitive. Lowest sort_order, then ID, wins. */
bool cmny_rule_match(CmnyDb *db, int64_t account_id, const CmnyBankCsvRow *row,
                     CmnyRuleMatch *out, char *err, size_t err_size);

#endif
