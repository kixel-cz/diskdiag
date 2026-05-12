# Makefile for diskdiag
# Supports Linux and macOS (Darwin)

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lm

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
TARGET  = diskdiag
SRC     = diskdiag.c
MAN     = diskdiag.8

UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
    MANDIR = $(PREFIX)/share/man/man8
else
    MANDIR = $(PREFIX)/share/man/man8
endif

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

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
