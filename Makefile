CC ?= cc
PREFIX ?= /usr/local
BUILD_DIR := build
VERSION := $(strip $(shell cat VERSION))

UNAME_S := $(shell uname -s)
PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)

ifeq ($(UNAME_S),Darwin)
  CURSES_CFLAGS ?=
  CURSES_LIBS ?= -lncurses
else ifneq ($(PKG_CONFIG),)
  CURSES_CFLAGS ?= $(shell pkg-config --cflags ncursesw 2>/dev/null)
  CURSES_LIBS ?= $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)
else
  CURSES_CFLAGS ?=
  CURSES_LIBS ?= -lncursesw
endif

ifneq ($(PKG_CONFIG),)
  SQLITE_CFLAGS ?= $(shell pkg-config --cflags sqlite3 2>/dev/null)
  SQLITE_LIBS ?= $(shell pkg-config --libs sqlite3 2>/dev/null || echo -lsqlite3)
else
  SQLITE_CFLAGS ?=
  SQLITE_LIBS ?= -lsqlite3
endif

CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -DCMNY_VERSION=\"$(VERSION)\" \
            $(CURSES_CFLAGS) $(SQLITE_CFLAGS)
CFLAGS ?= -O2
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
          -Wstrict-prototypes -Wmissing-prototypes -Wformat=2

SOURCES := $(sort $(wildcard src/*.c src/*/*.c src/*/*/*.c))
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
DEPENDENCIES := $(OBJECTS:.o=.d)
BINARY := $(BUILD_DIR)/cmny
DEBUG_CFLAGS := -O0 -g3 -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
                -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
SANITIZE_CFLAGS := -O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined \
                   -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
                   -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
FUZZ_CC ?= clang
ANALYZE_CC ?= clang
FUZZ_CFLAGS := -O1 -g3 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
               -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
               -Wstrict-prototypes -Wformat=2
DB_TEST_SOURCES := src/core.c src/db.c src/catalog.c src/ledger.c src/history.c \
                   src/reconcile.c src/import.c src/rules.c src/bank_csv.c \
                   src/csv_parser.c src/migrate_v3.c src/migrate_v4.c src/migrate_v5.c

.PHONY: all run demo screenshots test check policy-check debug sanitize analyze fuzz-build fuzz-smoke package verify-packages clean install uninstall

all: $(BINARY)

$(BINARY): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(CURSES_LIBS) $(SQLITE_LIBS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c include/cmny.h | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

run: $(BINARY)
	$(BINARY)

demo: $(BINARY)
	$(BINARY) --demo

screenshots: $(BINARY)
	python3 tools/capture_demo.py --screen overview --output assets/screenshots/overview.svg
	python3 tools/capture_demo.py --screen activity --theme violet --output assets/screenshots/activity.svg
	python3 tools/capture_demo.py --screen plan --theme violet --output assets/screenshots/plan.svg
	python3 tools/capture_demo.py --screen insights --theme amber --output assets/screenshots/insights.svg
	python3 tools/capture_demo.py --screen manage --theme ocean --output assets/screenshots/manage.svg

$(BUILD_DIR)/test_core: tests/test_core.c src/core.c include/cmny.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_core.c src/core.c $(LDFLAGS) -o $@

$(BUILD_DIR)/test_db: tests/test_db.c $(DB_TEST_SOURCES) include/cmny.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_db.c $(DB_TEST_SOURCES) \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_ledger: tests/test_ledger.c $(DB_TEST_SOURCES) include/cmny.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_ledger.c $(DB_TEST_SOURCES) \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_history: tests/test_history.c $(DB_TEST_SOURCES) include/cmny.h include/cmny_history.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_history.c $(DB_TEST_SOURCES) \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_reconcile: tests/test_reconcile.c $(DB_TEST_SOURCES) include/cmny.h include/cmny_reconcile.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_reconcile.c $(DB_TEST_SOURCES) \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_rules: tests/test_rules.c $(DB_TEST_SOURCES) include/cmny_rules.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_rules.c $(DB_TEST_SOURCES) \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_import: tests/test_import.c $(DB_TEST_SOURCES) include/cmny_import.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_import.c $(DB_TEST_SOURCES) \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_csv: tests/test_csv.c $(DB_TEST_SOURCES) src/csv.c include/cmny.h include/cmny_csv_parser.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_csv.c $(DB_TEST_SOURCES) \
		src/csv.c \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_paths: tests/test_paths.c src/paths.c include/cmny_paths.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_paths.c src/paths.c $(LDFLAGS) -o $@

$(BUILD_DIR)/test_csv_parser: tests/test_csv_parser.c src/csv_parser.c include/cmny_csv_parser.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_csv_parser.c src/csv_parser.c $(LDFLAGS) -o $@

$(BUILD_DIR)/test_bank_csv: tests/test_bank_csv.c src/bank_csv.c src/csv_parser.c include/cmny_bank_csv.h include/cmny_csv_parser.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_bank_csv.c src/bank_csv.c src/csv_parser.c \
		$(LDFLAGS) -o $@

$(BUILD_DIR)/test_expressions: tests/test_expressions.c src/expressions.c include/cmny_expr.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_expressions.c src/expressions.c $(LDFLAGS) -o $@

$(BUILD_DIR)/test_plan: tests/test_plan.c src/plan.c src/expressions.c include/cmny_plan.h include/cmny_expr.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_plan.c src/plan.c src/expressions.c \
		$(LDFLAGS) -o $@

$(BUILD_DIR)/test_backup: tests/test_backup.c $(DB_TEST_SOURCES) src/backup.c include/cmny.h include/cmny_backup.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_backup.c $(DB_TEST_SOURCES) src/backup.c \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

test: $(BINARY) $(BUILD_DIR)/test_core $(BUILD_DIR)/test_db $(BUILD_DIR)/test_ledger \
	$(BUILD_DIR)/test_history $(BUILD_DIR)/test_reconcile \
	$(BUILD_DIR)/test_rules $(BUILD_DIR)/test_import \
	$(BUILD_DIR)/test_csv \
	$(BUILD_DIR)/test_paths $(BUILD_DIR)/test_csv_parser $(BUILD_DIR)/test_bank_csv \
	$(BUILD_DIR)/test_expressions $(BUILD_DIR)/test_plan \
	$(BUILD_DIR)/test_backup
	$(BUILD_DIR)/test_core
	$(BUILD_DIR)/test_db
	$(BUILD_DIR)/test_ledger
	$(BUILD_DIR)/test_history
	$(BUILD_DIR)/test_reconcile
	$(BUILD_DIR)/test_rules
	$(BUILD_DIR)/test_import
	$(BUILD_DIR)/test_csv
	$(BUILD_DIR)/test_paths
	$(BUILD_DIR)/test_csv_parser
	$(BUILD_DIR)/test_bank_csv
	$(BUILD_DIR)/test_expressions
	$(BUILD_DIR)/test_plan
	$(BUILD_DIR)/test_backup
	python3 tests/test_cli.py $(BINARY)
	python3 tests/test_tui.py $(BINARY)
	python3 tests/test_capture.py $(BINARY)

policy-check:
	@printf '%s\n' "$(VERSION)" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$$'
	@test -z "$$(find . -path './.git' -prune -o -path './build' -prune -o \
		-path './dist' -prune -o -type f -name '*.md' ! -path './README.md' -print)"
	@test ! -d docs

check: all test policy-check

debug:
	+$(MAKE) BUILD_DIR=build/debug CFLAGS="$(DEBUG_CFLAGS)" all

sanitize:
	+$(MAKE) BUILD_DIR=build/sanitize CFLAGS="$(SANITIZE_CFLAGS)" \
		LDFLAGS="-fsanitize=address,undefined" test

analyze:
	@for source in $(SOURCES); do \
		$(ANALYZE_CC) $(CPPFLAGS) $(DEBUG_CFLAGS) --analyze \
			-Xanalyzer -analyzer-output=text "$$source" || exit 1; \
	done

build/fuzz/fuzz_csv: tests/fuzz/fuzz_csv.c src/csv_parser.c include/cmny_csv_parser.h
	mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) tests/fuzz/fuzz_csv.c src/csv_parser.c -o $@

build/fuzz/fuzz_amount: tests/fuzz/fuzz_amount.c src/expressions.c include/cmny_expr.h
	mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) tests/fuzz/fuzz_amount.c src/expressions.c -o $@

build/fuzz/fuzz_date: tests/fuzz/fuzz_date.c src/expressions.c include/cmny_expr.h
	mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) tests/fuzz/fuzz_date.c src/expressions.c -o $@

build/fuzz/fuzz_bank_csv: tests/fuzz/fuzz_bank_csv.c src/bank_csv.c src/csv_parser.c include/cmny_bank_csv.h include/cmny_csv_parser.h
	mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) tests/fuzz/fuzz_bank_csv.c \
		src/bank_csv.c src/csv_parser.c -o $@

build/fuzz/fuzz_plan: tests/fuzz/fuzz_plan.c src/plan.c src/expressions.c include/cmny_plan.h include/cmny_expr.h
	mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) tests/fuzz/fuzz_plan.c \
		src/plan.c src/expressions.c -o $@

fuzz-build: build/fuzz/fuzz_csv build/fuzz/fuzz_amount build/fuzz/fuzz_date \
	build/fuzz/fuzz_bank_csv build/fuzz/fuzz_plan

fuzz-smoke: fuzz-build
	build/fuzz/fuzz_csv tests/fuzz/corpus/csv -max_total_time=10 -max_len=4096
	build/fuzz/fuzz_amount tests/fuzz/corpus/amount -max_total_time=10 -max_len=256
	build/fuzz/fuzz_date tests/fuzz/corpus/date -max_total_time=10 -max_len=128
	build/fuzz/fuzz_bank_csv tests/fuzz/corpus/bank_csv -max_total_time=10 -max_len=65536
	build/fuzz/fuzz_plan tests/fuzz/corpus/plan -max_total_time=10 -max_len=512

package: check
	python3 tools/package_release.py

verify-packages:
	python3 tools/verify_packages.py

install: $(BINARY)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(BINARY) "$(DESTDIR)$(PREFIX)/bin/cmny"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/cmny"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
