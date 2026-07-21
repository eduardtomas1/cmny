#include "cmny_backup.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#define CMNY_BACKUP_PATH_CAP 4200
#define CMNY_BACKUP_ATTEMPTS 1000U
#define CMNY_BACKUP_TIMESTAMP_DIGITS 20U
#define CMNY_BACKUP_SEQUENCE_DIGITS 3U

static const char generated_marker[] = ".auto-backup-";
static const char partial_marker[] = ".auto-backup-partial-";

typedef enum {
    PUBLISH_OK,
    PUBLISH_COLLISION,
    PUBLISH_ERROR
} PublishResult;

static void clear_text(char *text, size_t size) {
    if (text != NULL && size > 0) text[0] = '\0';
}

static void backup_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void backup_errno_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", message, strerror(errno));
    }
}

#ifdef _WIN32
static void backup_windows_error(char *err, size_t err_size, const char *message,
                                 DWORD code) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s (Windows error %lu)", message,
                       (unsigned long)code);
    }
}
#endif

static bool ascii_digit(char value) {
    return value >= '0' && value <= '9';
}

static bool separator(char ch) {
#ifdef _WIN32
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

static bool split_database_path(const char *path, char directory[CMNY_BACKUP_PATH_CAP],
                                const char **basename, char *err, size_t err_size) {
    if (path == NULL || *path == '\0' || strlen(path) >= CMNY_BACKUP_PATH_CAP) {
        backup_error(err, err_size, "invalid database path for backup");
        return false;
    }
    const char *last = NULL;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (separator(*cursor)) last = cursor;
    }
    *basename = last != NULL ? last + 1 : path;
#ifdef _WIN32
    if (last == NULL && strlen(path) >= 2 && path[1] == ':') *basename = path + 2;
#endif
    if (**basename == '\0' || strcmp(*basename, ".") == 0 || strcmp(*basename, "..") == 0) {
        backup_error(err, err_size, "database path has no filename");
        return false;
    }
    if (last == NULL) {
#ifdef _WIN32
        if (strlen(path) >= 2 && path[1] == ':') {
            directory[0] = path[0];
            directory[1] = ':';
            directory[2] = '.';
            directory[3] = '\0';
        } else {
            (void)snprintf(directory, CMNY_BACKUP_PATH_CAP, ".");
        }
#else
        (void)snprintf(directory, CMNY_BACKUP_PATH_CAP, ".");
#endif
    } else {
        size_t length = (size_t)(last - path);
        if (length == 0) length = 1;
#ifndef _WIN32
        if (length == 1 && path[0] == '/' && path[1] == '/') length = 2;
#endif
        memcpy(directory, path, length);
        directory[length] = '\0';
    }
    return true;
}

static const char *path_name(const char *path) {
    const char *name = path;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (separator(*cursor)) name = cursor + 1;
    }
    return name;
}

static bool join_path(const char *directory, const char *name,
                      char out[CMNY_BACKUP_PATH_CAP]) {
    size_t directory_length = strlen(directory);
    const char *between = directory_length > 0 && separator(directory[directory_length - 1])
                              ? "" :
#ifdef _WIN32
                                "\\";
#else
                                "/";
#endif
    int written = snprintf(out, CMNY_BACKUP_PATH_CAP, "%s%s%s", directory, between, name);
    return written >= 0 && (size_t)written < CMNY_BACKUP_PATH_CAP;
}

static bool parse_fixed_timestamp(const char *text, int64_t *value) {
    uint64_t result = 0;
    for (size_t i = 0; i < CMNY_BACKUP_TIMESTAMP_DIGITS; i++) {
        if (!ascii_digit(text[i])) return false;
        unsigned digit = (unsigned)(text[i] - '0');
        if (result > ((uint64_t)INT64_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    *value = (int64_t)result;
    return true;
}

static bool generated_name(const char *name, const char *database_name) {
    size_t base_length = strlen(database_name);
    size_t marker_length = sizeof(generated_marker) - 1;
    size_t suffix_length = CMNY_BACKUP_TIMESTAMP_DIGITS + 1U +
                           CMNY_BACKUP_SEQUENCE_DIGITS;
    if (strlen(name) != base_length + marker_length + suffix_length ||
        strncmp(name, database_name, base_length) != 0 ||
        strncmp(name + base_length, generated_marker, marker_length) != 0) {
        return false;
    }
    const char *suffix = name + base_length + marker_length;
    int64_t ignored_timestamp = 0;
    if (suffix[0] != '0' || !parse_fixed_timestamp(suffix, &ignored_timestamp) ||
        suffix[CMNY_BACKUP_TIMESTAMP_DIGITS] != '-') {
        return false;
    }
    for (size_t i = CMNY_BACKUP_TIMESTAMP_DIGITS + 1U; i < suffix_length; i++) {
        if (!ascii_digit(suffix[i])) return false;
    }
    return suffix[suffix_length] == '\0';
}

static bool format_generated_path(const char *database_path, int64_t timestamp,
                                  unsigned sequence,
                                  char out[CMNY_BACKUP_PATH_CAP]) {
    int written = snprintf(out, CMNY_BACKUP_PATH_CAP, "%s%s%020lld-%03u",
                           database_path, generated_marker, (long long)timestamp, sequence);
    return written >= 0 && (size_t)written < CMNY_BACKUP_PATH_CAP;
}

static unsigned long long process_identifier(void) {
#ifdef _WIN32
    return (unsigned long long)(unsigned int)_getpid();
#else
    return (unsigned long long)(unsigned int)getpid();
#endif
}

static bool format_partial_path(const char *database_path, int64_t timestamp,
                                unsigned attempt,
                                char out[CMNY_BACKUP_PATH_CAP]) {
    int written = snprintf(out, CMNY_BACKUP_PATH_CAP, "%s%s%020lld-%020llu-%03u",
                           database_path, partial_marker, (long long)timestamp,
                           process_identifier(), attempt);
    return written >= 0 && (size_t)written < CMNY_BACKUP_PATH_CAP;
}

static int path_entry_state(const char *path, char *err, size_t err_size) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) return 1;
    DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return 0;
    backup_windows_error(err, err_size, "cannot inspect backup path", code);
    return -1;
#else
    struct stat info;
    if (lstat(path, &info) == 0) return 1;
    if (errno == ENOENT || errno == ENOTDIR) return 0;
    backup_errno_error(err, err_size, "cannot inspect backup path");
    return -1;
#endif
}

static bool regular_file_nofollow(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
#else
    struct stat info;
    return lstat(path, &info) == 0 && S_ISREG(info.st_mode);
#endif
}

static bool remove_regular_file(const char *path, char *err, size_t err_size) {
    if (!regular_file_nofollow(path)) {
        backup_error(err, err_size, "refusing to remove a non-regular backup path");
        return false;
    }
#ifdef _WIN32
    if (DeleteFileA(path) == 0) {
        backup_windows_error(err, err_size, "cannot remove backup file", GetLastError());
        return false;
    }
#else
    if (unlink(path) != 0) {
        backup_errno_error(err, err_size, "cannot remove backup file");
        return false;
    }
#endif
    return true;
}

static bool read_pragma_integer(sqlite3 *handle, const char *sql, int *value) {
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    bool ok = rc == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER;
    if (ok) *value = sqlite3_column_int(statement, 0);
    (void)sqlite3_finalize(statement);
    return ok;
}

static bool live_application_id(CmnyDb *db, int *application_id,
                                char *err, size_t err_size) {
    if (!read_pragma_integer(db->handle, "PRAGMA application_id", application_id) ||
        *application_id == 0) {
        backup_error(err, err_size, "cannot identify the CMNY database for backup rotation");
        return false;
    }
    return true;
}

static bool candidate_is_cmny(const char *path, int expected_application_id) {
    if (!regular_file_nofollow(path)) return false;
    int flags = SQLITE_OPEN_READONLY;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    sqlite3 *handle = NULL;
    int rc = sqlite3_open_v2(path, &handle, flags, NULL);
    int application_id = 0;
    int schema_version = 0;
    bool ok = rc == SQLITE_OK &&
              read_pragma_integer(handle, "PRAGMA application_id", &application_id) &&
              read_pragma_integer(handle, "PRAGMA user_version", &schema_version) &&
              application_id == expected_application_id && schema_version > 0;
    if (handle != NULL) (void)sqlite3_close(handle);
    return ok;
}

static bool consider_candidate(const char *directory, const char *database_name,
                               const char *name, const char *protected_name,
                               int expected_application_id, size_t *count,
                               char oldest[CMNY_BACKUP_PATH_CAP]) {
    if (!generated_name(name, database_name)) return true;
    char full_path[CMNY_BACKUP_PATH_CAP];
    if (!join_path(directory, name, full_path)) return false;
    if (!candidate_is_cmny(full_path, expected_application_id)) return true;
    if (*count == SIZE_MAX) return false;
    (*count)++;
    if (protected_name != NULL && strcmp(name, protected_name) == 0) return true;
    if (oldest[0] == '\0' || strcmp(name, path_name(oldest)) < 0) {
        (void)snprintf(oldest, CMNY_BACKUP_PATH_CAP, "%s", full_path);
    }
    return true;
}

static bool scan_backups(const char *database_path, const char *protected_path,
                         int expected_application_id, size_t *count,
                         char oldest[CMNY_BACKUP_PATH_CAP],
                         char *err, size_t err_size) {
    char directory[CMNY_BACKUP_PATH_CAP];
    const char *database_name = NULL;
    if (!split_database_path(database_path, directory, &database_name, err, err_size)) return false;
    const char *protected_name = NULL;
    char protected_directory[CMNY_BACKUP_PATH_CAP];
    if (protected_path != NULL &&
        !split_database_path(protected_path, protected_directory, &protected_name,
                             err, err_size)) {
        return false;
    }
    *count = 0;
    oldest[0] = '\0';
#ifdef _WIN32
    char pattern[CMNY_BACKUP_PATH_CAP];
    if (!join_path(directory, "*", pattern)) {
        backup_error(err, err_size, "backup directory path is too long");
        return false;
    }
    struct _finddata_t item;
    intptr_t search = _findfirst(pattern, &item);
    if (search == -1) {
        if (errno == ENOENT) return true;
        backup_errno_error(err, err_size, "cannot inspect backups");
        return false;
    }
    bool ok = true;
    do {
        if (!consider_candidate(directory, database_name, item.name, protected_name,
                                expected_application_id, count, oldest)) {
            ok = false;
            break;
        }
    } while (_findnext(search, &item) == 0);
    if (ok && errno != ENOENT) ok = false;
    if (_findclose(search) != 0) ok = false;
#else
    DIR *stream = opendir(directory);
    if (stream == NULL) {
        backup_errno_error(err, err_size, "cannot inspect backups");
        return false;
    }
    bool ok = true;
    struct dirent *item = NULL;
    for (;;) {
        errno = 0;
        item = readdir(stream);
        if (item == NULL) {
            if (errno != 0) ok = false;
            break;
        }
        if (!consider_candidate(directory, database_name, item->d_name, protected_name,
                                expected_application_id, count, oldest)) {
            ok = false;
            break;
        }
    }
    if (closedir(stream) != 0) ok = false;
#endif
    if (!ok) backup_error(err, err_size, "cannot inspect generated backup names");
    return ok;
}

static bool sync_backup_directory(const char *database_path, char *err, size_t err_size) {
#ifdef _WIN32
    (void)database_path;
    (void)err;
    (void)err_size;
    return true;
#else
    char directory[CMNY_BACKUP_PATH_CAP];
    const char *database_name = NULL;
    if (!split_database_path(database_path, directory, &database_name, err, err_size)) return false;
    (void)database_name;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int descriptor = open(directory, flags);
    if (descriptor < 0) {
        backup_errno_error(err, err_size, "cannot open backup directory for synchronization");
        return false;
    }
    bool ok = fsync(descriptor) == 0;
    int saved_errno = errno;
    if (close(descriptor) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (!ok) {
        errno = saved_errno;
        backup_errno_error(err, err_size, "cannot synchronize backup directory");
    }
    return ok;
#endif
}

static bool delete_generated_backup(const char *database_path, const char *path,
                                    int expected_application_id,
                                    char *err, size_t err_size) {
    char database_directory[CMNY_BACKUP_PATH_CAP];
    char candidate_directory[CMNY_BACKUP_PATH_CAP];
    const char *database_name = NULL;
    const char *candidate_name = NULL;
    if (!split_database_path(database_path, database_directory, &database_name, err, err_size) ||
        !split_database_path(path, candidate_directory, &candidate_name, err, err_size) ||
        !generated_name(candidate_name, database_name) ||
        !candidate_is_cmny(path, expected_application_id)) {
        backup_error(err, err_size, "refusing to prune a non-CMNY backup path");
        return false;
    }
    return remove_regular_file(path, err, err_size);
}

static bool prune_backups(const char *database_path, const char *protected_path,
                          size_t retention, int expected_application_id,
                          char *err, size_t err_size) {
    bool removed = false;
    for (;;) {
        size_t count = 0;
        char oldest[CMNY_BACKUP_PATH_CAP];
        if (!scan_backups(database_path, protected_path, expected_application_id,
                          &count, oldest, err, err_size)) {
            return false;
        }
        if (count <= retention) {
            return !removed || sync_backup_directory(database_path, err, err_size);
        }
        if (oldest[0] == '\0') {
            backup_error(err, err_size, "cannot prune backups without removing the new backup");
            return false;
        }
        if (!delete_generated_backup(database_path, oldest, expected_application_id,
                                     err, err_size)) {
            return false;
        }
        removed = true;
    }
}

static PublishResult publish_backup(const char *database_path, const char *partial_path,
                                    const char *final_path, char *err, size_t err_size) {
#ifdef _WIN32
    (void)database_path;
    if (MoveFileExA(partial_path, final_path, MOVEFILE_WRITE_THROUGH) != 0) return PUBLISH_OK;
    DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) return PUBLISH_COLLISION;
    backup_windows_error(err, err_size, "cannot publish automatic backup", code);
    return PUBLISH_ERROR;
#else
    if (link(partial_path, final_path) != 0) {
        if (errno == EEXIST) return PUBLISH_COLLISION;
        backup_errno_error(err, err_size, "cannot publish automatic backup");
        return PUBLISH_ERROR;
    }
    if (unlink(partial_path) != 0) {
        int saved_errno = errno;
        (void)unlink(final_path);
        errno = saved_errno;
        backup_errno_error(err, err_size, "cannot remove automatic backup temporary name");
        return PUBLISH_ERROR;
    }
    if (!sync_backup_directory(database_path, err, err_size)) {
        (void)unlink(final_path);
        return PUBLISH_ERROR;
    }
    return PUBLISH_OK;
#endif
}

static bool rotating_configuration_valid(CmnyDb *db, const char *database_path,
                                         int64_t timestamp, size_t retention,
                                         char *created_path, size_t created_path_size,
                                         char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || database_path == NULL ||
        *database_path == '\0' || timestamp < 0 || retention == 0 ||
        retention > CMNY_BACKUP_MAX_RETENTION || created_path == NULL ||
        created_path_size == 0) {
        backup_error(err, err_size, "invalid rotating backup configuration");
        return false;
    }
    char directory[CMNY_BACKUP_PATH_CAP];
    const char *database_name = NULL;
    char generated[CMNY_BACKUP_PATH_CAP];
    char partial[CMNY_BACKUP_PATH_CAP];
    if (!split_database_path(database_path, directory, &database_name, err, err_size) ||
        !format_generated_path(database_path, timestamp, 0, generated) ||
        !format_partial_path(database_path, timestamp, 0, partial)) {
        backup_error(err, err_size, "generated backup path is too long");
        return false;
    }
    (void)directory;
    (void)database_name;
    if (strlen(generated) >= created_path_size) {
        backup_error(err, err_size, "created backup path does not fit the result buffer");
        return false;
    }
    return true;
}

bool cmny_backup_create_rotating(CmnyDb *db, const char *database_path,
                                 int64_t timestamp, size_t retention,
                                 char *created_path, size_t created_path_size,
                                 char *err, size_t err_size) {
    clear_text(created_path, created_path_size);
    clear_text(err, err_size);
    if (!rotating_configuration_valid(db, database_path, timestamp, retention,
                                      created_path, created_path_size, err, err_size)) {
        return false;
    }
    int expected_application_id = 0;
    if (!live_application_id(db, &expected_application_id, err, err_size)) return false;

    char partial[CMNY_BACKUP_PATH_CAP];
    bool backup_created = false;
    for (unsigned attempt = 0; attempt < CMNY_BACKUP_ATTEMPTS; attempt++) {
        if (!format_partial_path(database_path, timestamp, attempt, partial)) {
            backup_error(err, err_size, "generated backup path is too long");
            return false;
        }
        int state = path_entry_state(partial, err, err_size);
        if (state < 0) return false;
        if (state > 0) continue;
        if (!cmny_db_backup(db, partial, err, err_size)) return false;
        backup_created = true;
        break;
    }
    if (!backup_created) {
        backup_error(err, err_size, "too many automatic backup temporary files exist");
        return false;
    }
    if (!candidate_is_cmny(partial, expected_application_id)) {
        (void)remove_regular_file(partial, NULL, 0);
        backup_error(err, err_size, "automatic backup did not produce a valid CMNY database");
        return false;
    }

    char candidate[CMNY_BACKUP_PATH_CAP];
    bool published = false;
    for (unsigned sequence = 0; sequence < CMNY_BACKUP_ATTEMPTS; sequence++) {
        if (!format_generated_path(database_path, timestamp, sequence, candidate)) {
            (void)remove_regular_file(partial, NULL, 0);
            backup_error(err, err_size, "generated backup path is too long");
            return false;
        }
        PublishResult result = publish_backup(database_path, partial, candidate, err, err_size);
        if (result == PUBLISH_OK) {
            published = true;
            break;
        }
        if (result == PUBLISH_ERROR) {
            (void)remove_regular_file(partial, NULL, 0);
            return false;
        }
    }
    if (!published) {
        (void)remove_regular_file(partial, NULL, 0);
        backup_error(err, err_size, "too many backups share this timestamp");
        return false;
    }
    if (!candidate_is_cmny(candidate, expected_application_id)) {
        backup_error(err, err_size, "published automatic backup cannot be verified");
        return false;
    }
    if (!prune_backups(database_path, candidate, retention, expected_application_id,
                       err, err_size)) {
        if (delete_generated_backup(database_path, candidate, expected_application_id,
                                    NULL, 0)) {
            (void)sync_backup_directory(database_path, NULL, 0);
        }
        return false;
    }
    if (!candidate_is_cmny(candidate, expected_application_id)) {
        backup_error(err, err_size, "published automatic backup disappeared during rotation");
        return false;
    }
    (void)snprintf(created_path, created_path_size, "%s", candidate);
    return true;
}

static bool parse_timestamp(const char *text, int64_t *value) {
    if (text == NULL || *text == '\0' || value == NULL) return false;
    uint64_t result = 0;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (!ascii_digit(*cursor)) return false;
        unsigned digit = (unsigned)(*cursor - '0');
        if (result > ((uint64_t)INT64_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    *value = (int64_t)result;
    return true;
}

bool cmny_backup_run_if_due(CmnyDb *db, const char *database_path,
                            int64_t timestamp, unsigned interval_hours,
                            size_t retention, bool *created,
                            char *created_path, size_t created_path_size,
                            char *err, size_t err_size) {
    if (created != NULL) *created = false;
    clear_text(created_path, created_path_size);
    clear_text(err, err_size);
    if (created == NULL || interval_hours == 0 || interval_hours > 24U * 365U ||
        !rotating_configuration_valid(db, database_path, timestamp, retention,
                                      created_path, created_path_size, err, err_size)) {
        if (err != NULL && err_size > 0 && err[0] == '\0') {
            backup_error(err, err_size, "invalid automatic backup schedule");
        }
        return false;
    }
    char stored[32] = {0};
    int64_t previous = 0;
    bool has_previous = cmny_db_setting_get(db, "last_auto_backup", stored, sizeof(stored));
    if (has_previous && !parse_timestamp(stored, &previous)) {
        backup_error(err, err_size, "automatic backup timestamp is invalid");
        return false;
    }
    int64_t interval = (int64_t)interval_hours * 3600;
    if (has_previous && (timestamp <= previous || timestamp - previous < interval)) return true;

    char local_path[CMNY_BACKUP_PATH_CAP];
    if (!cmny_backup_create_rotating(db, database_path, timestamp, retention,
                                     local_path, sizeof(local_path), err, err_size)) {
        return false;
    }
    char value[32];
    (void)snprintf(value, sizeof(value), "%lld", (long long)timestamp);
    if (!cmny_db_setting_set(db, "last_auto_backup", value, err, err_size)) return false;
    *created = true;
    (void)snprintf(created_path, created_path_size, "%s", local_path);
    return true;
}
