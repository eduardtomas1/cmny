CC ?= cc
PREFIX ?= /usr/local
BUILD_DIR := build

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

CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L $(CURSES_CFLAGS) $(SQLITE_CFLAGS)
CFLAGS ?= -O2
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
          -Wstrict-prototypes -Wmissing-prototypes -Wformat=2

SOURCES := src/main.c src/core.c src/paths.c src/db.c src/csv.c src/ui.c
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
DEPENDENCIES := $(OBJECTS:.o=.d)
BINARY := $(BUILD_DIR)/cmny
DEBUG_CFLAGS := -O0 -g3 -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
                -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
SANITIZE_CFLAGS := -O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined \
                   -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
                   -Wstrict-prototypes -Wmissing-prototypes -Wformat=2

.PHONY: all run demo screenshots test check debug sanitize package verify-packages clean install uninstall

all: $(BINARY)

$(BINARY): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(CURSES_LIBS) $(SQLITE_LIBS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c include/cmny.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

run: $(BINARY)
	$(BINARY)

demo: $(BINARY)
	$(BINARY) --demo

screenshots: $(BINARY)
	python3 tools/capture_demo.py --screen overview --output assets/screenshots/overview.svg
	python3 tools/capture_demo.py --screen reports --theme violet --output assets/screenshots/reports.svg
	python3 tools/capture_demo.py --screen settings --theme amber --output assets/screenshots/settings.svg

$(BUILD_DIR)/test_core: tests/test_core.c src/core.c include/cmny.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_core.c src/core.c $(LDFLAGS) -o $@

$(BUILD_DIR)/test_db: tests/test_db.c src/core.c src/db.c include/cmny.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_db.c src/core.c src/db.c \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

$(BUILD_DIR)/test_csv: tests/test_csv.c src/core.c src/db.c src/csv.c include/cmny.h tests/test.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) tests/test_csv.c src/core.c src/db.c src/csv.c \
		$(LDFLAGS) $(SQLITE_LIBS) -o $@

test: $(BINARY) $(BUILD_DIR)/test_core $(BUILD_DIR)/test_db $(BUILD_DIR)/test_csv
	$(BUILD_DIR)/test_core
	$(BUILD_DIR)/test_db
	$(BUILD_DIR)/test_csv
	python3 tests/test_cli.py $(BINARY)
	python3 tests/test_tui.py $(BINARY)
	python3 tests/test_capture.py $(BINARY)

check: all test

debug:
	+$(MAKE) BUILD_DIR=build/debug CFLAGS="$(DEBUG_CFLAGS)" all

sanitize:
	+$(MAKE) BUILD_DIR=build/sanitize CFLAGS="$(SANITIZE_CFLAGS)" \
		LDFLAGS="-fsanitize=address,undefined" test

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
