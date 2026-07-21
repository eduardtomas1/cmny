#include "cmny.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define cmny_mkdir(path, mode) _mkdir(path)
#else
#define cmny_mkdir(path, mode) mkdir(path, mode)
#endif

static bool path_separator(char ch) {
#ifdef _WIN32
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

bool cmny_resolve_db_path(const char *override_path, char *out, size_t out_size,
                          char *err, size_t err_size) {
    const char *path = override_path;
    if (path == NULL || *path == '\0') {
        path = getenv("CMNY_DB");
    }
    if (path != NULL && *path != '\0') {
        if (snprintf(out, out_size, "%s", path) >= (int)out_size) {
            (void)snprintf(err, err_size, "database path is too long");
            return false;
        }
        return true;
    }

#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (base != NULL && *base != '\0') {
        if (snprintf(out, out_size, "%s\\CMNY\\cmny.db", base) >= (int)out_size) {
            (void)snprintf(err, err_size, "local application data path is too long");
            return false;
        }
        return true;
    }
    const char *user_home = getenv("USERPROFILE");
    if (user_home == NULL || *user_home == '\0') user_home = getenv("HOME");
    if (user_home == NULL || *user_home == '\0') {
        (void)snprintf(err, err_size, "LOCALAPPDATA is not set; use --db PATH or CMNY_DB");
        return false;
    }
    if (snprintf(out, out_size, "%s\\AppData\\Local\\CMNY\\cmny.db", user_home) >=
        (int)out_size) {
        (void)snprintf(err, err_size, "user data path is too long");
        return false;
    }
    return true;
#else
    const char *base = getenv("XDG_DATA_HOME");
    if (base != NULL && base[0] == '/') {
        if (snprintf(out, out_size, "%s/cmny/cmny.db", base) >= (int)out_size) {
            (void)snprintf(err, err_size, "XDG data path is too long");
            return false;
        }
        return true;
    }

    const char *user_home = getenv("HOME");
    if (user_home == NULL || *user_home == '\0') {
        (void)snprintf(err, err_size, "HOME is not set; use --db PATH or CMNY_DB");
        return false;
    }
    if (snprintf(out, out_size, "%s/.local/share/cmny/cmny.db", user_home) >= (int)out_size) {
        (void)snprintf(err, err_size, "home data path is too long");
        return false;
    }
    return true;
#endif
}

static bool ensure_directory(const char *path, char *err, size_t err_size) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return true;
        }
        (void)snprintf(err, err_size, "%s exists but is not a directory", path);
        return false;
    }
    if (errno != ENOENT) {
        (void)snprintf(err, err_size, "cannot inspect %s: %s", path, strerror(errno));
        return false;
    }
    if (cmny_mkdir(path, 0700) != 0 && errno != EEXIST) {
        (void)snprintf(err, err_size, "cannot create %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

bool cmny_prepare_db_parent(const char *path, char *err, size_t err_size) {
    if (path == NULL || *path == '\0' || strlen(path) >= 4096) {
        (void)snprintf(err, err_size, "invalid database path");
        return false;
    }

    char parent[4096];
    (void)snprintf(parent, sizeof(parent), "%s", path);
    char *slash = NULL;
    for (char *cursor = parent; *cursor != '\0'; cursor++) {
        if (path_separator(*cursor)) slash = cursor;
    }
    if (slash == NULL) {
        return true;
    }
    if (slash == parent) {
        return true;
    }
    *slash = '\0';

    for (char *cursor = parent + 1; *cursor != '\0'; cursor++) {
        if (!path_separator(*cursor)) {
            continue;
        }
        char separator = *cursor;
        *cursor = '\0';
        if (!ensure_directory(parent, err, err_size)) {
            *cursor = separator;
            return false;
        }
        *cursor = separator;
    }
    return ensure_directory(parent, err, err_size);
}
