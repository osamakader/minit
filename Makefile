CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
LDFLAGS ?=

SRC     = src/main.c src/config.c src/deps.c src/log.c src/supervisor.c src/system.c src/output.c src/watchdog.c
OBJ     = $(SRC:.c=.o)
TARGET  = minit

.PHONY: all clean install static size

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# stripped static binary for initramfs/rootfs, e.g. make static CC=musl-gcc
static: $(TARGET)-static

$(TARGET)-static: $(OBJ)
	$(CC) $(LDFLAGS) -static -s -o $@ $^

size: $(TARGET)-static
	@ls -l $(TARGET)-static | awk '{ printf "%s: %d bytes (budget 512000)\n", $$NF, $$5 }'
	@size $(TARGET)-static

%.o: %.c include/minit.h
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET) $(TARGET)-static

install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/etc/minit
	install -m 644 examples/minit.conf.example $(DESTDIR)/etc/minit/
