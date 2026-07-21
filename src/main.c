#include "cmny.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
        "  --backup PATH       Create a safe SQLite backup and exit\n"
        "  --restore PATH      Restore a CMNY backup and exit\n"
        "  --check             Check ledger health and exit\n"
        "  --export PATH       Export every transaction to CSV and exit\n"
        "  --import PATH       Preview and import a CMNY CSV file\n"
        "  --yes               Confirm import or restore without prompting\n"
        "  --no-color          Disable terminal colors\n"
        "  --ascii             Force portable ASCII visuals\n"
        "  -h, --help          Show this help\n"
        "  -v, --version       Show the version\n\n"
        "Environment: CMNY_DB, CMNY_CURRENCY, CMNY_THEME, XDG_DATA_HOME, NO_COLOR\n",
        CMNY_VERSION);
}

static bool confirm_cli(const char *question) {
    char answer[16];
    (void)printf("%s [y/N] ", question);
    (void)fflush(stdout);
    return fgets(answer, sizeof(answer), stdin) != NULL &&
           (answer[0] == 'y' || answer[0] == 'Y');
}

static bool safety_backup_path(const char *database, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s.before-restore-%lld", database,
                           (long long)time(NULL));
    return written >= 0 && (size_t)written < out_size;
}

static bool same_existing_file(const char *left, const char *right) {
    struct stat left_info;
    struct stat right_info;
    return left != NULL && right != NULL && stat(left, &left_info) == 0 &&
           stat(right, &right_info) == 0 && left_info.st_dev == right_info.st_dev &&
           left_info.st_ino == right_info.st_ino;
}

int main(int argc, char **argv) {
    CmnyOptions options = {0};
    bool print_path = false;
    bool check_database = false;
    bool assume_yes = false;
    const char *backup_path = NULL;
    const char *restore_path = NULL;
    const char *export_path = NULL;
    const char *import_path = NULL;
    const char *currency_input = getenv("CMNY_CURRENCY");
    const char *theme_input = getenv("CMNY_THEME");
    if (currency_input != NULL && *currency_input == '\0') currency_input = NULL;
    if (theme_input != NULL && *theme_input == '\0') theme_input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) options.demo = true;
        else if (strcmp(argv[i], "--no-color") == 0) options.no_color = true;
        else if (strcmp(argv[i], "--ascii") == 0) options.ascii = true;
        else if (strcmp(argv[i], "--db-path") == 0) print_path = true;
        else if (strcmp(argv[i], "--check") == 0) check_database = true;
        else if (strcmp(argv[i], "--yes") == 0) assume_yes = true;
        else if ((strcmp(argv[i], "--db") == 0 || strcmp(argv[i], "--database") == 0) && i + 1 < argc) {
            options.db_override = argv[++i];
        } else if (strcmp(argv[i], "--currency") == 0 && i + 1 < argc) {
            currency_input = argv[++i];
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme_input = argv[++i];
        } else if (strcmp(argv[i], "--backup") == 0 && i + 1 < argc) {
            backup_path = argv[++i];
        } else if (strcmp(argv[i], "--restore") == 0 && i + 1 < argc) {
            restore_path = argv[++i];
        } else if (strcmp(argv[i], "--export") == 0 && i + 1 < argc) {
            export_path = argv[++i];
        } else if (strcmp(argv[i], "--import") == 0 && i + 1 < argc) {
            import_path = argv[++i];
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

    int command_count = (backup_path != NULL ? 1 : 0) + (restore_path != NULL ? 1 : 0) +
                        (export_path != NULL ? 1 : 0) + (import_path != NULL ? 1 : 0) +
                        (check_database ? 1 : 0);
    if (command_count > 1) {
        (void)fprintf(stderr, "cmny: choose only one backup, restore, check, export, or import command\n");
        return 2;
    }
    bool command_mode = command_count == 1;

    if (options.demo && (options.db_override != NULL || command_mode)) {
        (void)fprintf(stderr, "cmny: --demo cannot be combined with --db or data commands\n");
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
    options.theme_explicit = theme_input != NULL;
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
    if (!command_mode && (!cmny_isatty(CMNY_STDIN_FD) || !cmny_isatty(CMNY_STDOUT_FD))) {
        (void)fprintf(stderr, "cmny: the interactive app needs a terminal (try cmny --help)\n");
        return 1;
    }
    if ((restore_path != NULL || import_path != NULL) && !assume_yes &&
        (!cmny_isatty(CMNY_STDIN_FD) || !cmny_isatty(CMNY_STDOUT_FD))) {
        (void)fprintf(stderr, "cmny: confirmation needs a terminal; inspect first, then use --yes\n");
        return 1;
    }
    if (restore_path != NULL &&
        (strcmp(path, restore_path) == 0 || same_existing_file(path, restore_path))) {
        (void)fprintf(stderr, "cmny: restore source and destination must be different\n");
        return 1;
    }
    if (restore_path != NULL && !assume_yes &&
        !confirm_cli("Replace the current ledger with this backup?")) {
        (void)printf("Restore cancelled.\n");
        return 0;
    }
    struct stat database_before;
    bool database_existed = !options.demo && stat(path, &database_before) == 0;
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

    if (command_mode) {
        int result = 0;
        if (check_database) {
            if (cmny_db_check(&db, err, sizeof(err))) (void)printf("Ledger check: OK\n");
            else result = 1;
        } else if (backup_path != NULL) {
            if (strcmp(path, backup_path) == 0 ||
                !cmny_prepare_db_parent(backup_path, err, sizeof(err)) ||
                !cmny_db_backup(&db, backup_path, err, sizeof(err))) result = 1;
            else (void)printf("Backup created: %s\n", backup_path);
        } else if (export_path != NULL) {
            size_t count = 0;
            if (!cmny_prepare_db_parent(export_path, err, sizeof(err)) ||
                !cmny_csv_export(&db, export_path, &count, err, sizeof(err))) result = 1;
            else (void)printf("Exported %zu transactions: %s\n", count, export_path);
        } else if (import_path != NULL) {
            CmnyImportPreview preview = {0};
            if (!cmny_csv_import(&db, import_path, false, &preview, err, sizeof(err))) {
                result = 1;
            } else {
                char income[64], expense[64], question[160];
                cmny_money_format_plain(preview.income_cents, income, sizeof(income));
                cmny_money_format_plain(preview.expense_cents, expense, sizeof(expense));
                (void)printf("Import preview: %zu transactions, %s income, %s expenses\n",
                             preview.transaction_count, income, expense);
                (void)snprintf(question, sizeof(question), "Import these %zu transactions?",
                               preview.transaction_count);
                if (!assume_yes && !confirm_cli(question)) {
                    (void)printf("Import cancelled.\n");
                } else if (!cmny_csv_import(&db, import_path, true, &preview, err, sizeof(err))) {
                    result = 1;
                } else {
                    (void)printf("Imported %zu transactions.\n", preview.transaction_count);
                }
            }
        } else if (restore_path != NULL) {
            char safety[4200] = {0};
            if (database_existed) {
                if (!safety_backup_path(path, safety, sizeof(safety))) {
                    (void)snprintf(err, sizeof(err), "safety backup path is too long");
                    result = 1;
                } else if (!cmny_db_backup(&db, safety, err, sizeof(err))) {
                    result = 1;
                }
            }
            if (result == 0 && !cmny_db_restore(&db, restore_path, currency, err, sizeof(err))) {
                result = 1;
            } else if (result == 0) {
                (void)printf("Ledger restored from: %s\n", restore_path);
                if (database_existed) (void)printf("Previous ledger backed up to: %s\n", safety);
            }
        }
        if (result != 0) (void)fprintf(stderr, "cmny: %s\n", err[0] != '\0' ? err : "command failed");
        cmny_db_close(&db);
        return result;
    }

    (void)setlocale(LC_ALL, "");
    int result = cmny_ui_run(&db, &options, options.demo ? ":memory:" : path);
    cmny_db_close(&db);
    return result;
}
