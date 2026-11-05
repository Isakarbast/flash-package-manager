VERSION := $(shell sed -n 's/^\#define FPM_VERSION "\(.*\)"/\1/p' src/fpm.h)
CC      ?= cc
CFLAGS  ?= -O2 -pipe
PREFIX  ?= /usr
# Always applied, even when CFLAGS is overridden on the command line.
FPM_CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

fpm: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c src/fpm.h
	$(CC) $(FPM_CFLAGS) $(CFLAGS) -c -o $@ $<

# Fully static binary: make clean && make LDFLAGS=-static
install: fpm
	install -Dm755 fpm $(DESTDIR)$(PREFIX)/bin/fpm
	install -Dm644 fpm.conf.example $(DESTDIR)/etc/fpm.conf

check: fpm
	sh tests/run.sh

# Release tarball of the source files (tracked or not, minus .gitignore), byte-for-byte reproducible (sorted,
# fixed owner and time, no gzip timestamp) so recipes can pin its sha256.
dist:
	git ls-files -z --cached --others --exclude-standard | LC_ALL=C sort -z | tar --null -T - --mtime='2026-01-01 00:00Z' \
		--owner=0 --group=0 --numeric-owner --format=gnu \
		--transform='s,^,fpm-$(VERSION)/,' -cf - | gzip -n9 > fpm-$(VERSION).tar.gz

clean:
	rm -f fpm $(OBJ)

.PHONY: install check clean dist
