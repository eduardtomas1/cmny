#include "cmny_rules.h"
#include "test.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(_Alignof(CmnyRule) >= _Alignof(int64_t),
               "categorization rules must preserve int64 alignment");
_Static_assert(offsetof(CmnyRule, sort_order) > offsetof(CmnyRule, updated_at),
               "narrow rule fields belong after int64 fields");
_Static_assert(offsetof(CmnyRuleMatch, matched) > offsetof(CmnyRuleMatch, tag_id),
               "rule match flags belong after int64 fields");

static int64_t first_account(CmnyDb *db, char *err, size_t err_size) {
    CmnyAccount accounts[4];
    size_t count = 0;
    ASSERT_TRUE(cmny_account_list(db, false, accounts, 4, &count, err, err_size));
    ASSERT_TRUE(count > 0);
    return accounts[0].id;
}

static CmnyBankCsvRow bank_row(const char *payee, const char *note, int64_t amount) {
    CmnyBankCsvRow row = {.amount_cents = amount, .physical_line = 2, .record_number = 2};
    (void)snprintf(row.date, sizeof(row.date), "2026-11-01");
    (void)snprintf(row.payee, sizeof(row.payee), "%s", payee);
    (void)snprintf(row.note, sizeof(row.note), "%s", note);
    (void)snprintf(row.identity, sizeof(row.identity), "test-identity");
    return row;
}

int main(void) {
    char path[] = "build/cmny-rules-XXXXXX";
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    ASSERT_TRUE(close(descriptor) == 0);
    CmnyDb db = {0};
    char currency[4] = {0};
    char err[256] = {0};
    ASSERT_TRUE(cmny_db_open(&db, path, false, "EUR", currency, err, sizeof(err)));
    int64_t cash_id = first_account(&db, err, sizeof(err));
    int64_t card_id = 0;
    ASSERT_TRUE(cmny_account_create(&db, "Rules card", CMNY_ACCOUNT_CREDIT, "",
                                    &card_id, err, sizeof(err)));
    int64_t general_id = 0;
    int64_t precise_id = 0;
    int64_t income_id = 0;
    int64_t tag_id = 0;
    ASSERT_TRUE(cmny_category_create(&db, "Rule general", CMNY_CATEGORY_EXPENSE, 0,
                                     &general_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_category_create(&db, "Rule precise", CMNY_CATEGORY_EXPENSE, 0,
                                     &precise_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_category_create(&db, "Rule income", CMNY_CATEGORY_INCOME, 0,
                                     &income_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_tag_create(&db, "Rule tag", &tag_id, err, sizeof(err)));

    CmnyRuleDraft contains = {
        .category_id = general_id,
        .sort_order = 20,
        .payee_mode = CMNY_RULE_TEXT_CONTAINS,
        .note_mode = CMNY_RULE_TEXT_ANY,
        .enabled = true,
        .name = "Shop contains",
        .payee_pattern = "Shop",
        .note_pattern = "",
    };
    int64_t contains_id = 0;
    ASSERT_TRUE(cmny_rule_create(&db, &contains, &contains_id, err, sizeof(err)));
    CmnyRuleDraft exact = {
        .account_id = cash_id,
        .category_id = precise_id,
        .tag_id = tag_id,
        .sort_order = 10,
        .payee_mode = CMNY_RULE_TEXT_EXACT,
        .note_mode = CMNY_RULE_TEXT_ANY,
        .enabled = true,
        .name = "Exact cash shop",
        .payee_pattern = "Shop One",
        .note_pattern = "",
    };
    int64_t exact_id = 0;
    ASSERT_TRUE(cmny_rule_create(&db, &exact, &exact_id, err, sizeof(err)));
    CmnyRuleDraft salary = {
        .category_id = income_id,
        .minimum_amount_minor = 1,
        .maximum_amount_minor = 500000,
        .sort_order = 5,
        .payee_mode = CMNY_RULE_TEXT_ANY,
        .note_mode = CMNY_RULE_TEXT_EXACT,
        .enabled = true,
        .has_minimum_amount = true,
        .has_maximum_amount = true,
        .name = "Payroll note",
        .payee_pattern = "",
        .note_pattern = "Payroll",
    };
    int64_t salary_id = 0;
    ASSERT_TRUE(cmny_rule_create(&db, &salary, &salary_id, err, sizeof(err)));

    CmnyBankCsvRow row = bank_row("Shop One", "card", -1000);
    CmnyRuleMatch match = {0};
    ASSERT_TRUE(cmny_rule_match(&db, cash_id, &row, &match, err, sizeof(err)));
    ASSERT_TRUE(match.matched);
    ASSERT_EQ_I64(exact_id, match.rule_id);
    ASSERT_EQ_I64(precise_id, match.category_id);
    ASSERT_EQ_I64(tag_id, match.tag_id);
    ASSERT_TRUE(cmny_rule_match(&db, card_id, &row, &match, err, sizeof(err)));
    ASSERT_EQ_I64(contains_id, match.rule_id);
    ASSERT_EQ_I64(general_id, match.category_id);

    CmnyRule exact_row = {0};
    ASSERT_TRUE(cmny_rule_get(&db, exact_id, &exact_row, err, sizeof(err)));
    exact.enabled = false;
    ASSERT_TRUE(cmny_rule_update(&db, exact_id, exact_row.revision, &exact, err, sizeof(err)));
    ASSERT_TRUE(!cmny_rule_update(&db, exact_id, exact_row.revision, &exact, err, sizeof(err)));
    ASSERT_TRUE(cmny_rule_match(&db, cash_id, &row, &match, err, sizeof(err)));
    ASSERT_EQ_I64(contains_id, match.rule_id);

    row = bank_row("Employer", "Payroll", 250000);
    ASSERT_TRUE(cmny_rule_match(&db, cash_id, &row, &match, err, sizeof(err)));
    ASSERT_EQ_I64(salary_id, match.rule_id);
    row.amount_cents = -250000;
    ASSERT_TRUE(cmny_rule_match(&db, cash_id, &row, &match, err, sizeof(err)));
    ASSERT_TRUE(!match.matched);

    CmnyRule rules[8];
    size_t count = 0;
    ASSERT_TRUE(cmny_rule_list(&db, 0, rules, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(3, count);
    ASSERT_EQ_I64(salary_id, rules[0].id);
    ASSERT_EQ_I64(exact_id, rules[1].id);
    ASSERT_EQ_I64(contains_id, rules[2].id);
    ASSERT_TRUE(!cmny_rule_list(&db, 0, rules, CMNY_RULE_LIST_LIMIT + 1U,
                                &count, err, sizeof(err)));

    CmnyRuleDraft invalid = contains;
    invalid.payee_pattern = "";
    ASSERT_TRUE(!cmny_rule_create(&db, &invalid, NULL, err, sizeof(err)));
    invalid = salary;
    invalid.minimum_amount_minor = 100;
    invalid.maximum_amount_minor = -100;
    ASSERT_TRUE(!cmny_rule_create(&db, &invalid, NULL, err, sizeof(err)));
    invalid = contains;
    invalid.account_id = 999999;
    ASSERT_TRUE(!cmny_rule_create(&db, &invalid, NULL, err, sizeof(err)));

    cmny_db_close(&db);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    row = bank_row("Corner Shop", "", -500);
    ASSERT_TRUE(cmny_rule_match(&db, cash_id, &row, &match, err, sizeof(err)));
    ASSERT_EQ_I64(contains_id, match.rule_id);

    ASSERT_TRUE(sqlite3_exec(db.handle,
        "PRAGMA ignore_check_constraints=ON;"
        "UPDATE categorization_rules SET payee_mode=2,payee_pattern='' WHERE id=1;"
        "PRAGMA ignore_check_constraints=OFF", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "UPDATE categorization_rules SET payee_pattern='Shop' WHERE id=1",
        NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));

    ASSERT_TRUE(cmny_rule_get(&db, exact_id, &exact_row, err, sizeof(err)));
    ASSERT_TRUE(!cmny_rule_delete(&db, exact_id, exact_row.revision - 1, err, sizeof(err)));
    ASSERT_TRUE(cmny_rule_delete(&db, exact_id, exact_row.revision, err, sizeof(err)));
    ASSERT_TRUE(!cmny_rule_get(&db, exact_id, &exact_row, err, sizeof(err)));
    cmny_db_close(&db);

    ASSERT_TRUE(unlink(path) == 0);
    char sidecar[4200];
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)unlink(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)unlink(sidecar);
    (void)printf("ok - categorization rule tests\n");
    return 0;
}
