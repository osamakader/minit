CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
LDFLAGS ?=

SRC     = src/main.c src/config.c src/deps.c src/log.c src/supervisor.c
OBJ     = $(SRC:.c=.o)
TARGET  = minit

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c include/minit.h
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/etc/minit
	install -m 644 etc/minit.conf.example $(DESTDIR)/etc/minit/
