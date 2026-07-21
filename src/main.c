#include "cmny.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef _WIN32
#define cmny_isatty(fd) _isatty(fd)
#define CMNY_STDIN_FD _fileno(stdin)
#define CMNY_STDOUT_FD _fileno(stdout)
#else
#define cmny_isatty(fd) isatty(fd)
#define CMNY_STDIN_FD STDIN_FILENO
#define CMNY_STDOUT_FD STDOUT_FILENO
#endif

static void print_help(FILE *stream) {
    (void)fprintf(stream,
        "CMNY %s - your money, clearly\n\n"
        "Usage: cmny [options]\n\n"
        "Options:\n"
        "  --demo              Open a deterministic in-memory demo\n"
        "  --db PATH           Use a specific SQLite database\n"
        "  --db-path           Print the resolved database path and exit\n"
        "  --currency CODE     Set a new ledger's two-decimal currency (default: EUR)\n"
        "  --theme NAME        Use ocean, violet, or amber (default: ocean)\n"
        "  --no-color          Disable terminal colors\n"
        "  --ascii             Force portable ASCII visuals\n"
        "  -h, --help          Show this help\n"
        "  -v, --version       Show the version\n\n"
        "Environment: CMNY_DB, CMNY_CURRENCY, CMNY_THEME, XDG_DATA_HOME, NO_COLOR\n",
        CMNY_VERSION);
}

int main(int argc, char **argv) {
    CmnyOptions options = {0};
    bool print_path = false;
    const char *currency_input = getenv("CMNY_CURRENCY");
    const char *theme_input = getenv("CMNY_THEME");
    if (currency_input != NULL && *currency_input == '\0') currency_input = NULL;
    if (theme_input != NULL && *theme_input == '\0') theme_input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) options.demo = true;
        else if (strcmp(argv[i], "--no-color") == 0) options.no_color = true;
        else if (strcmp(argv[i], "--ascii") == 0) options.ascii = true;
        else if (strcmp(argv[i], "--db-path") == 0) print_path = true;
        else if ((strcmp(argv[i], "--db") == 0 || strcmp(argv[i], "--database") == 0) && i + 1 < argc) {
            options.db_override = argv[++i];
        } else if (strcmp(argv[i], "--currency") == 0 && i + 1 < argc) {
            currency_input = argv[++i];
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme_input = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(stdout);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            (void)printf("cmny %s\n", CMNY_VERSION);
            return 0;
        } else {
            (void)fprintf(stderr, "cmny: unknown or incomplete option: %s\n\n", argv[i]);
            print_help(stderr);
            return 2;
        }
    }

    if (options.demo && options.db_override != NULL) {
        (void)fprintf(stderr, "cmny: --demo and --db cannot be used together\n");
        return 2;
    }
    char currency[4] = {0};
    if (currency_input != NULL && !cmny_currency_supported(currency_input, currency)) {
        (void)fprintf(stderr, "cmny: use a supported two-decimal currency such as EUR or USD\n");
        return 2;
    }
    if (theme_input != NULL && !cmny_theme_parse(theme_input, &options.theme)) {
        (void)fprintf(stderr, "cmny: theme must be ocean, violet, or amber\n");
        return 2;
    }
    options.currency = currency;
    if (getenv("NO_COLOR") != NULL) options.no_color = true;

    char path[4096] = {0};
    char err[256] = {0};
    if (!options.demo && !cmny_resolve_db_path(options.db_override, path, sizeof(path), err, sizeof(err))) {
        (void)fprintf(stderr, "cmny: %s\n", err);
        return 1;
    }
    if (print_path) {
        (void)printf("%s\n", options.demo ? ":memory:" : path);
        return 0;
    }
    if (!cmny_isatty(CMNY_STDIN_FD) || !cmny_isatty(CMNY_STDOUT_FD)) {
        (void)fprintf(stderr, "cmny: the interactive app needs a terminal (try cmny --help)\n");
        return 1;
    }
    if (!options.demo && !cmny_prepare_db_parent(path, err, sizeof(err))) {
        (void)fprintf(stderr, "cmny: %s\n", err);
        return 1;
    }

    CmnyDb db = {0};
    if (!cmny_db_open(&db, path, options.demo, currency_input != NULL ? currency : NULL,
                      currency, err, sizeof(err))) {
        (void)fprintf(stderr, "cmny: %s\n", err);
        return 1;
    }
    if (options.demo && !cmny_db_seed_demo(&db, err, sizeof(err))) {
        (void)fprintf(stderr, "cmny: cannot prepare demo: %s\n", err);
        cmny_db_close(&db);
        return 1;
    }

    (void)setlocale(LC_ALL, "");
    int result = cmny_ui_run(&db, &options, options.demo ? ":memory:" : path);
    cmny_db_close(&db);
    return result;
}
