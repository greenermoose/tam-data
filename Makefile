# tamdata - terminal spreadsheet multitool
#
# Phase 0: strict C11 build linking libtam statically, plus an ASan/UBSan
# unit-test harness. libtam 0.4.0 or later is discovered with
# `pkg-config libtam`; set LIBTAM_PREFIX (or LIBTAM_CFLAGS/LIBTAM_LIBS) for a
# prefix outside pkg-config's default search path.

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -Werror -std=c11
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Isrc

# When Nix is detected (e.g. Tamlinux with Home Manager, where ~/.local/bin is
# store-linked and /usr/local is retired), default development installs to the
# user development prefix $(HOME)/.local/dev rather than root-owned /usr/local.
ifndef PREFIX
  ifneq ($(or $(NIX_PROFILES),$(wildcard $(HOME)/.nix-profile)),)
    PREFIX := $(HOME)/.local/dev
  else
    PREFIX := /usr/local
  endif
endif
BINDIR   ?= $(PREFIX)/bin

PKG_CONFIG ?= pkg-config
ifdef LIBTAM_PREFIX
LIBTAM_CFLAGS ?= -I$(LIBTAM_PREFIX)/include
LIBTAM_LIBS   ?= -L$(LIBTAM_PREFIX)/lib -ltam
LIBTAM_ERROR  := $(if $(shell grep -s -l tam_exec_opts $(LIBTAM_PREFIX)/include/tam.h),,LIBTAM_PREFIX=$(LIBTAM_PREFIX) has no libtam 0.4.0 or later include/tam.h)
else ifeq ($(origin LIBTAM_LIBS),undefined)
LIBTAM_FOUND  := $(shell $(PKG_CONFIG) --atleast-version=0.4.0 libtam 2>/dev/null && echo yes)
LIBTAM_CFLAGS := $(if $(LIBTAM_FOUND),$(shell $(PKG_CONFIG) --cflags libtam))
LIBTAM_LIBS   := $(if $(LIBTAM_FOUND),$(shell $(PKG_CONFIG) --libs libtam))
LIBTAM_ERROR  := $(if $(LIBTAM_FOUND),,$(PKG_CONFIG) cannot find libtam 0.4.0 or later)
endif

LDFLAGS  ?=
LDLIBS   ?=

TD_BUILD_ID := $(shell git -C $(CURDIR) describe --always --dirty 2>/dev/null)
CPPFLAGS += -DTD_BUILD_ID=\"$(TD_BUILD_ID)\"

BUILD    := build
BIN      := $(BUILD)/tamdata
TEST_BIN := $(BUILD)/test_tamdata

SRC := src/arena.c src/val.c src/sheet.c src/term.c src/input.c src/tamdata.c
DEPS := src/version.h src/utf8.h src/arena.h src/val.h src/sheet.h src/term.h \
        src/input.h src/tamdata.h

OBJS      := $(SRC:src/%.c=$(BUILD)/obj/%.o)
TEST_SRCS := tests/test_main.c tests/test_arena.c tests/test_val.c \
             tests/test_sheet.c tests/test_term.c tests/test_input.c
TEST_OBJS := $(TEST_SRCS:tests/%.c=$(BUILD)/test/%.o) \
             $(SRC:src/%.c=$(BUILD)/test/src/%.o)

TEST_CFLAGS  ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
TEST_LDFLAGS ?= -fsanitize=address,undefined

CONFIG := $(CC)|$(CPPFLAGS)|$(CFLAGS)|$(TEST_CFLAGS)|$(TEST_LDFLAGS)|$(LDFLAGS)|$(LDLIBS)|$(LIBTAM_CFLAGS)|$(LIBTAM_LIBS)|$(PREFIX)

.PHONY: all command test install uninstall clean libtam-check FORCE

all: command
command: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)/obj $(BUILD)/test/src

# Rebuild everything when the compiler, flags or install prefix change, but
# leave timestamps alone when the configuration is identical.
$(BUILD)/.flags: FORCE | $(BUILD)
	@printf '%s\n' '$(CONFIG)' | cmp -s - $@ || printf '%s\n' '$(CONFIG)' > $@

libtam-check:
ifneq ($(LIBTAM_ERROR),)
	@printf '%s\n' 'tamdata: cannot build: $(LIBTAM_ERROR).' \
		'  Install libtam 0.4.0 or later first:   make -C <libtam checkout> install PREFIX=<prefix>' \
		'  then let pkg-config see it:   export PKG_CONFIG_PATH=<prefix>/lib/pkgconfig' \
		'  or name the prefix:   make LIBTAM_PREFIX=<prefix>' >&2
	@exit 1
endif

$(BUILD)/obj/%.o: src/%.c $(DEPS) $(BUILD)/.flags | $(BUILD) libtam-check
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIBTAM_CFLAGS) -c $< -o $@

$(BIN): $(OBJS) | $(BUILD)
	$(CC) $(OBJS) $(LDFLAGS) $(LIBTAM_LIBS) $(LDLIBS) -o $@

$(BUILD)/test/src/%.o: src/%.c $(DEPS) $(BUILD)/.flags | $(BUILD) libtam-check
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(LIBTAM_CFLAGS) \
		-DTD_TAMDATA_NO_MAIN -c $< -o $@

$(BUILD)/test/%.o: tests/%.c tests/test.h $(DEPS) $(BUILD)/.flags | $(BUILD) libtam-check
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(LIBTAM_CFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_OBJS) | $(BUILD)
	$(CC) $(TEST_CFLAGS) $(TEST_LDFLAGS) $(TEST_OBJS) $(LDFLAGS) \
		$(LIBTAM_LIBS) $(LDLIBS) -o $@

test: $(TEST_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	./$(TEST_BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/tamdata

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tamdata

clean:
	rm -rf $(BUILD)

FORCE:
