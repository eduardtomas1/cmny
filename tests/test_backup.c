#include "cmny_backup.h"
#include "test.h"

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_PATH_CAP 4600

static int expected_application_id = 0;

static void make_path(const char *directory, const char *name,
                      char out[TEST_PATH_CAP]) {
    int written = snprintf(out, TEST_PATH_CAP, "%s/%s", directory, name);
    ASSERT_TRUE(written > 0 && (size_t)written < TEST_PATH_CAP);
}

static void generated_path(const char *database, int64_t timestamp, unsigned sequence,
                           char out[TEST_PATH_CAP]) {
    int written = snprintf(out, TEST_PATH_CAP, "%s.auto-backup-%020lld-%03u",
                           database, (long long)timestamp, sequence);
    ASSERT_TRUE(written > 0 && (size_t)written < TEST_PATH_CAP);
}

static void write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb");
    ASSERT_TRUE(file != NULL);
    ASSERT_TRUE(fputs(contents, file) >= 0);
    ASSERT_TRUE(fclose(file) == 0);
}

static void create_foreign_sqlite(const char *path) {
    sqlite3 *handle = NULL;
    ASSERT_TRUE(sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(handle,
                            "PRAGMA application_id=1234; PRAGMA user_version=1;"
                            "CREATE TABLE foreign_data(value TEXT);",
                            NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(handle) == SQLITE_OK);
}

static bool canonical_generated_name(const char *name) {
    static const char prefix[] = "ledger.db.auto-backup-";
    size_t prefix_length = sizeof(prefix) - 1;
    if (strlen(name) != prefix_length + 24 ||
        strncmp(name, prefix, prefix_length) != 0) {
        return false;
    }
    const char *suffix = name + prefix_length;
    if (suffix[0] != '0' || suffix[20] != '-') return false;
    uint64_t timestamp = 0;
    for (size_t i = 0; i < 20; i++) {
        if (suffix[i] < '0' || suffix[i] > '9') return false;
        unsigned digit = (unsigned)(suffix[i] - '0');
        if (timestamp > ((uint64_t)INT64_MAX - digit) / 10U) return false;
        timestamp = timestamp * 10U + digit;
    }
    for (size_t i = 21; i < 24; i++) {
        if (suffix[i] < '0' || suffix[i] > '9') return false;
    }
    return true;
}

static bool valid_backup_file(const char *path) {
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    sqlite3 *handle = NULL;
    int rc = sqlite3_open_v2(path, &handle, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *statement = NULL;
    if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(handle, "PRAGMA application_id", -1, &statement, NULL);
    }
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    bool ok = rc == SQLITE_ROW &&
              sqlite3_column_int(statement, 0) == expected_application_id;
    (void)sqlite3_finalize(statement);
    if (handle != NULL) (void)sqlite3_close(handle);
    return ok;
}

static size_t generated_count(const char *directory) {
    DIR *stream = opendir(directory);
    ASSERT_TRUE(stream != NULL);
    size_t count = 0;
    struct dirent *item = NULL;
    while ((item = readdir(stream)) != NULL) {
        if (!canonical_generated_name(item->d_name)) continue;
        char path[TEST_PATH_CAP];
        make_path(directory, item->d_name, path);
        if (valid_backup_file(path)) count++;
    }
    ASSERT_TRUE(closedir(stream) == 0);
    return count;
}

static size_t partial_count(const char *directory) {
    DIR *stream = opendir(directory);
    ASSERT_TRUE(stream != NULL);
    size_t count = 0;
    struct dirent *item = NULL;
    while ((item = readdir(stream)) != NULL) {
        if (strstr(item->d_name, ".auto-backup-partial-") != NULL) count++;
    }
    ASSERT_TRUE(closedir(stream) == 0);
    return count;
}

static void assert_backup_ok(const char *path) {
    sqlite3 *backup = NULL;
    ASSERT_TRUE(sqlite3_open_v2(path, &backup, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *check = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(backup, "PRAGMA quick_check", -1, &check, NULL) ==
                SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(check) == SQLITE_ROW);
    ASSERT_TRUE(strcmp((const char *)sqlite3_column_text(check, 0), "ok") == 0);
    ASSERT_TRUE(sqlite3_finalize(check) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(backup) == SQLITE_OK);
}

static void assert_private_regular_file(const char *path) {
    struct stat info;
    ASSERT_TRUE(lstat(path, &info) == 0);
    ASSERT_TRUE(S_ISREG(info.st_mode));
    ASSERT_EQ_I64(0600, info.st_mode & 0777);
}

static void cleanup_directory(const char *directory) {
    DIR *stream = opendir(directory);
    ASSERT_TRUE(stream != NULL);
    struct dirent *item = NULL;
    while ((item = readdir(stream)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
        char path[TEST_PATH_CAP];
        make_path(directory, item->d_name, path);
        struct stat info;
        ASSERT_TRUE(lstat(path, &info) == 0);
        if (S_ISDIR(info.st_mode)) ASSERT_TRUE(rmdir(path) == 0);
        else ASSERT_TRUE(unlink(path) == 0);
    }
    ASSERT_TRUE(closedir(stream) == 0);
    ASSERT_TRUE(rmdir(directory) == 0);
}

static void test_invalid_arguments(CmnyDb *db, const char *database,
                                   const char *directory) {
    char created[TEST_PATH_CAP] = "stale path";
    char err[256] = "stale error";
    ASSERT_TRUE(!cmny_backup_create_rotating(db, database, -1, 2,
                                             created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(created[0] == '\0');
    ASSERT_TRUE(err[0] != '\0');

    (void)snprintf(created, sizeof(created), "stale path");
    ASSERT_TRUE(!cmny_backup_create_rotating(db, database, 1, 0,
                                             created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(created[0] == '\0');
    ASSERT_TRUE(!cmny_backup_create_rotating(db, database, 1,
                                             CMNY_BACKUP_MAX_RETENTION + 1U,
                                             created, sizeof(created), err, sizeof(err)));

    char tiny[8] = "stale";
    size_t before = generated_count(directory);
    ASSERT_TRUE(!cmny_backup_create_rotating(db, database, 1, 2,
                                             tiny, sizeof(tiny), err, sizeof(err)));
    ASSERT_TRUE(tiny[0] == '\0');
    ASSERT_EQ_I64(before, generated_count(directory));

    char trailing[TEST_PATH_CAP];
    int written = snprintf(trailing, sizeof(trailing), "%s/", database);
    ASSERT_TRUE(written > 0 && (size_t)written < sizeof(trailing));
    ASSERT_TRUE(!cmny_backup_create_rotating(db, trailing, 1, 2,
                                             created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(created[0] == '\0');

    CmnyDb closed = {0};
    ASSERT_TRUE(!cmny_backup_create_rotating(&closed, database, 1, 2,
                                             created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(!cmny_backup_create_rotating(NULL, database, 1, 2,
                                             created, sizeof(created), err, sizeof(err)));

    CmnyDb unidentified = {0};
    ASSERT_TRUE(sqlite3_open(":memory:", &unidentified.handle) == SQLITE_OK);
    (void)snprintf(created, sizeof(created), "stale path");
    ASSERT_TRUE(!cmny_backup_create_rotating(&unidentified, database, 1, 2,
                                             created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(created[0] == '\0');
    ASSERT_TRUE(sqlite3_close(unidentified.handle) == SQLITE_OK);
}

int main(void) {
    char directory[] = "build/cmny-backup-service-XXXXXX";
    int placeholder = mkstemp(directory);
    ASSERT_TRUE(placeholder >= 0);
    ASSERT_TRUE(close(placeholder) == 0);
    ASSERT_TRUE(unlink(directory) == 0);
    ASSERT_TRUE(mkdir(directory, 0700) == 0);

    char database[TEST_PATH_CAP];
    make_path(directory, "ledger.db", database);
    char decoy[TEST_PATH_CAP];
    make_path(directory, "ledger.db.auto-backup-decoy", decoy);
    write_file(decoy, "keep");

    char exact_decoy[TEST_PATH_CAP];
    generated_path(database, 1, 0, exact_decoy);
    write_file(exact_decoy, "not a SQLite database");

    char symlink_decoy[TEST_PATH_CAP];
    generated_path(database, 2, 0, symlink_decoy);
    ASSERT_TRUE(symlink("ledger.db.auto-backup-decoy", symlink_decoy) == 0);

    char directory_decoy[TEST_PATH_CAP];
    generated_path(database, 3, 0, directory_decoy);
    ASSERT_TRUE(mkdir(directory_decoy, 0700) == 0);

    char foreign_database[TEST_PATH_CAP];
    generated_path(database, 4, 0, foreign_database);
    create_foreign_sqlite(foreign_database);

    CmnyDb db = {0};
    char currency[4] = {0};
    char err[256] = {0};
    ASSERT_TRUE(cmny_db_open(&db, database, false, "EUR", currency, err, sizeof(err)));
    sqlite3_stmt *identity = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle, "PRAGMA application_id", -1, &identity, NULL) ==
                SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(identity) == SQLITE_ROW);
    expected_application_id = sqlite3_column_int(identity, 0);
    ASSERT_TRUE(expected_application_id != 0);
    ASSERT_TRUE(sqlite3_finalize(identity) == SQLITE_OK);

    test_invalid_arguments(&db, database, directory);
    ASSERT_EQ_I64(0, partial_count(directory));

    char created[TEST_PATH_CAP];
    char expected_100[TEST_PATH_CAP];
    generated_path(database, 100, 0, expected_100);
    ASSERT_TRUE(cmny_backup_create_rotating(&db, database, 100, 2,
                                            created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(strcmp(created, expected_100) == 0);
    assert_private_regular_file(created);
    assert_backup_ok(created);
    ASSERT_EQ_I64(0, partial_count(directory));

    char expected_101_0[TEST_PATH_CAP];
    char expected_101_1[TEST_PATH_CAP];
    generated_path(database, 101, 0, expected_101_0);
    generated_path(database, 101, 1, expected_101_1);
    ASSERT_TRUE(cmny_backup_create_rotating(&db, database, 101, 2,
                                            created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(strcmp(created, expected_101_0) == 0);
    ASSERT_TRUE(cmny_backup_create_rotating(&db, database, 101, 2,
                                            created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(strcmp(created, expected_101_1) == 0);
    ASSERT_TRUE(access(expected_100, F_OK) != 0);
    ASSERT_TRUE(access(expected_101_0, F_OK) == 0);
    ASSERT_TRUE(access(expected_101_1, F_OK) == 0);
    ASSERT_EQ_I64(2, generated_count(directory));

    char malformed_range[TEST_PATH_CAP];
    make_path(directory,
              "ledger.db.auto-backup-99999999999999999999-000",
              malformed_range);
    ASSERT_TRUE(link(expected_101_1, malformed_range) == 0);

    char collision[TEST_PATH_CAP];
    generated_path(database, 200, 0, collision);
    ASSERT_TRUE(symlink("ledger.db.auto-backup-decoy", collision) == 0);
    char expected_200_1[TEST_PATH_CAP];
    generated_path(database, 200, 1, expected_200_1);
    ASSERT_TRUE(cmny_backup_create_rotating(&db, database, 200, 2,
                                            created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(strcmp(created, expected_200_1) == 0);
    ASSERT_TRUE(lstat(collision, &(struct stat){0}) == 0);

    char expected_50[TEST_PATH_CAP];
    generated_path(database, 50, 0, expected_50);
    ASSERT_TRUE(cmny_backup_create_rotating(&db, database, 50, 2,
                                            created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(strcmp(created, expected_50) == 0);
    ASSERT_TRUE(access(expected_50, F_OK) == 0);
    ASSERT_TRUE(access(expected_200_1, F_OK) == 0);
    ASSERT_EQ_I64(2, generated_count(directory));

    ASSERT_TRUE(access(decoy, F_OK) == 0);
    ASSERT_TRUE(access(exact_decoy, F_OK) == 0);
    struct stat decoy_info;
    ASSERT_TRUE(lstat(symlink_decoy, &decoy_info) == 0 && S_ISLNK(decoy_info.st_mode));
    ASSERT_TRUE(lstat(directory_decoy, &decoy_info) == 0 && S_ISDIR(decoy_info.st_mode));
    ASSERT_TRUE(access(foreign_database, F_OK) == 0);
    ASSERT_TRUE(access(malformed_range, F_OK) == 0);
    ASSERT_EQ_I64(0, partial_count(directory));

    bool automatic = false;
    ASSERT_TRUE(cmny_backup_run_if_due(&db, database, 500, 24, 2, &automatic,
                                       created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(automatic);
    ASSERT_TRUE(created[0] != '\0');

    (void)snprintf(created, sizeof(created), "stale path");
    (void)snprintf(err, sizeof(err), "stale error");
    ASSERT_TRUE(cmny_backup_run_if_due(&db, database, 501, 24, 2, &automatic,
                                       created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(!automatic);
    ASSERT_TRUE(created[0] == '\0');
    ASSERT_TRUE(err[0] == '\0');

    ASSERT_TRUE(cmny_backup_run_if_due(&db, database, 400, 24, 2, &automatic,
                                       created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(!automatic);
    ASSERT_TRUE(created[0] == '\0');

    ASSERT_TRUE(cmny_backup_run_if_due(&db, database, 500 + 86400, 24, 2, &automatic,
                                       created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(automatic);
    ASSERT_EQ_I64(2, generated_count(directory));

    (void)snprintf(created, sizeof(created), "stale path");
    ASSERT_TRUE(!cmny_backup_run_if_due(&db, database, 501, 24, 0, &automatic,
                                        created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(!automatic);
    ASSERT_TRUE(created[0] == '\0');
    ASSERT_TRUE(!cmny_backup_run_if_due(&db, database, 501, 24U * 365U + 1U, 2,
                                        &automatic, created, sizeof(created), err, sizeof(err)));

    ASSERT_TRUE(cmny_db_setting_set(&db, "last_auto_backup", "9223372036854775807",
                                    err, sizeof(err)));
    ASSERT_TRUE(cmny_backup_run_if_due(&db, database, INT64_MAX - 1, 24, 2, &automatic,
                                       created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(!automatic);
    ASSERT_TRUE(cmny_db_setting_set(&db, "last_auto_backup", "invalid",
                                    err, sizeof(err)));
    ASSERT_TRUE(!cmny_backup_run_if_due(&db, database, INT64_MAX, 24, 2, &automatic,
                                        created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(!automatic);
    ASSERT_TRUE(created[0] == '\0');

    ASSERT_TRUE(cmny_backup_create_rotating(&db, database, INT64_MAX, 2,
                                            created, sizeof(created), err, sizeof(err)));
    ASSERT_TRUE(access(created, F_OK) == 0);
    ASSERT_EQ_I64(2, generated_count(directory));
    ASSERT_EQ_I64(0, partial_count(directory));
    assert_private_regular_file(created);
    assert_backup_ok(created);

    ASSERT_TRUE(access(decoy, F_OK) == 0);
    ASSERT_TRUE(access(exact_decoy, F_OK) == 0);
    ASSERT_TRUE(lstat(symlink_decoy, &decoy_info) == 0 && S_ISLNK(decoy_info.st_mode));
    ASSERT_TRUE(lstat(directory_decoy, &decoy_info) == 0 && S_ISDIR(decoy_info.st_mode));
    ASSERT_TRUE(access(foreign_database, F_OK) == 0);
    ASSERT_TRUE(access(malformed_range, F_OK) == 0);

    cmny_db_close(&db);
    cleanup_directory(directory);
    (void)printf("ok - rotating backup tests\n");
    return 0;
}
