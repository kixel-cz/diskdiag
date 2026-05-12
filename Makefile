# Makefile for diskdiag
# Supports Linux and macOS (Darwin)

CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lm

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
MANDIR  = $(PREFIX)/share/man/man8
TARGET  = diskdiag
SRC     = diskdiag.c
MAN     = diskdiag.8

# On macOS, 'gcc' is typically an alias for clang.
# Use clang explicitly if available, otherwise fall back to gcc.
UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
    CC := $(shell command -v clang 2>/dev/null || echo gcc)
else
    CC := $(shell command -v gcc 2>/dev/null || echo cc)
endif

.PHONY: all clean install uninstall test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: $(TARGET)
	sudo bash tests/run_tests.sh

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 $(MAN) $(DESTDIR)$(MANDIR)/$(MAN)
	gzip -f $(DESTDIR)$(MANDIR)/$(MAN)
	@echo "Installed $(TARGET) to $(DESTDIR)$(BINDIR)/$(TARGET)"
	@echo "Installed man page to $(DESTDIR)$(MANDIR)/$(MAN).gz"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/$(MAN).gz
	@echo "Uninstalled $(TARGET)"

clean:
	rm -f $(TARGET)
