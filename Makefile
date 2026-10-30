CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
LDFLAGS ?=

SRC     = src/main.c src/config.c src/deps.c src/log.c src/supervisor.c src/system.c src/output.c src/watchdog.c src/control.c
OBJ     = $(SRC:.c=.o)
TARGET  = minit
CTL     = minitctl
CTL_OBJ = src/minitctl.o
UNIT    = tests/unit

.PHONY: all clean install static size test

all: $(TARGET) $(CTL)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(CTL): $(CTL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# stripped static binaries for initramfs/rootfs, e.g. make static CC=musl-gcc
static: $(TARGET)-static $(CTL)-static

$(TARGET)-static: $(OBJ)
	$(CC) $(LDFLAGS) -static -s -o $@ $^

$(CTL)-static: $(CTL_OBJ)
	$(CC) $(LDFLAGS) -static -s -o $@ $^

size: static
	@ls -l $(TARGET)-static $(CTL)-static | awk '{ printf "%s: %d bytes\n", $$NF, $$5; t += $$5 } END { printf "total: %d bytes (budget 512000)\n", t }'
	@size $(TARGET)-static $(CTL)-static

$(UNIT): tests/unit.o $(filter-out src/main.o,$(OBJ))
	$(CC) $(LDFLAGS) -o $@ $^

test: all $(UNIT)
	$(UNIT)
	tests/regress.sh

%.o: %.c include/minit.h
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

clean:
	rm -f $(OBJ) $(CTL_OBJ) $(TARGET) $(CTL) $(TARGET)-static $(CTL)-static tests/unit.o $(UNIT)

install: $(TARGET) $(CTL)
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(CTL) $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/etc/minit
	install -m 644 examples/minit.conf.example $(DESTDIR)/etc/minit/
