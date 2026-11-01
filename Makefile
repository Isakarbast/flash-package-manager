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

clean:
	rm -f fpm $(OBJ)

.PHONY: install check clean
