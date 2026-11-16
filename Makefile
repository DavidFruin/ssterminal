CC = gcc
PREFIX ?= /usr/local

# -MMD -MP emits .d files listing each object's header dependencies, so
# editing a header rebuilds everything that includes it. Without this,
# changing a struct in a header left stale objects linking against the old
# layout - which builds cleanly and then misbehaves at runtime.
DEPFLAGS = -MMD -MP
CFLAGS = -Wall -Wextra -O2 $(DEPFLAGS)

# The libcurl headers are vendored (vendor/include), so only the runtime
# library is needed to link. With libcurl's dev package installed,
# pkg-config supplies the flags. Without it there is no unversioned
# libcurl.so for plain -lcurl to find, so link the SONAME directly:
# libcurl.so.4 is what every distro's runtime package ships, whatever the
# exact version behind it.
CURL_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null || echo -l:libcurl.so.4)

# ncursesw (wide char) so UTF-8 glyphs in the feed render correctly.
NCURSES_CFLAGS = $(shell pkg-config --cflags ncursesw 2>/dev/null || echo -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600)
NCURSES_LIBS   = $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw -ltinfo)

# Shared library. Static, not shared: each built binary ends up a single
# self-contained file that works wherever it's copied or symlinked (e.g.
# onto PATH via `make install`), with no runtime library search path to get
# wrong. libcurl itself stays dynamic - a normal system library.
LIB_SRCS = lib/ss_api.c lib/ss_json.c lib/ss_config.c lib/ss_state.c lib/ss_utils.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB = lib/libss.a

CLI_SRCS = cli/main.c cli/output.c
CLI_OBJS = $(CLI_SRCS:.c=.o)
CLI_BIN = simple-social-cli

WIZ_SRCS = wizard/src/main.c wizard/src/repl.c wizard/src/wizard.c wizard/src/commands.c \
           wizard/src/output.c wizard/src/input.c
WIZ_OBJS = $(WIZ_SRCS:.c=.o)
WIZ_BIN = simple-social-cli-interactive

TUI_SRCS = tui/src/main.c tui/src/app.c tui/src/ui.c tui/src/net.c tui/src/store.c \
           tui/src/timefmt.c tui/src/cfg.c tui/src/detail.c tui/src/shell.c \
           tui/src/editor.c tui/src/filepick.c tui/src/settings.c tui/src/auth.c
TUI_OBJS = $(TUI_SRCS:.c=.o)
TUI_BIN = simple-social-tui

ALL_OBJS = $(LIB_OBJS) $(CLI_OBJS) $(WIZ_OBJS) $(TUI_OBJS)

all: $(CLI_BIN) $(WIZ_BIN) $(TUI_BIN)

cli: $(CLI_BIN)
wizard: $(WIZ_BIN)
tui: $(TUI_BIN)

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -Ivendor/include -Ilib -c -o $@ $<

$(CLI_BIN): $(CLI_OBJS) $(LIB)
	$(CC) -o $@ $(CLI_OBJS) $(LIB) $(CURL_LIBS)

cli/%.o: cli/%.c
	$(CC) $(CFLAGS) -Ilib -c -o $@ $<

$(WIZ_BIN): $(WIZ_OBJS) $(LIB)
	$(CC) -o $@ $(WIZ_OBJS) $(LIB) $(CURL_LIBS)

wizard/src/%.o: wizard/src/%.c
	$(CC) $(CFLAGS) -Ilib -c -o $@ $<

$(TUI_BIN): $(TUI_OBJS) $(LIB)
	$(CC) -o $@ $(TUI_OBJS) $(LIB) $(CURL_LIBS) $(NCURSES_LIBS)

tui/src/%.o: tui/src/%.c
	$(CC) $(CFLAGS) -Ilib $(NCURSES_CFLAGS) -c -o $@ $<

clean:
	rm -f $(ALL_OBJS) $(ALL_OBJS:.o=.d) $(LIB) $(CLI_BIN) $(WIZ_BIN) $(TUI_BIN) tests/test_json tests/test_json.d

# Short PATH names are unchanged from the three old repos.
install-cli: $(CLI_BIN)
	install -D -m 755 $(CLI_BIN) $(DESTDIR)$(PREFIX)/bin/sscli
install-wizard: $(WIZ_BIN)
	install -D -m 755 $(WIZ_BIN) $(DESTDIR)$(PREFIX)/bin/sswiz
install-tui: $(TUI_BIN)
	install -D -m 755 $(TUI_BIN) $(DESTDIR)$(PREFIX)/bin/sstui
install: install-cli install-wizard install-tui

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/sscli $(DESTDIR)$(PREFIX)/bin/sswiz $(DESTDIR)$(PREFIX)/bin/sstui

# Unit tests that need no server: the JSON reader.
tests/test_json: tests/test_json.c $(LIB)
	$(CC) $(CFLAGS) -Ilib -o $@ tests/test_json.c $(LIB) $(CURL_LIBS)

unit: tests/test_json
	./tests/test_json

# Needs TEST_EMAIL / TEST_PASSWORD (and TEST_BASE_URL; the default is dev).
test: $(CLI_BIN) $(WIZ_BIN)
	./tests/test_cli.sh
	python3 wizard/tests/test_wizard.py

-include $(ALL_OBJS:.o=.d)

.PHONY: all unit cli wizard tui clean install install-cli install-wizard install-tui uninstall test
