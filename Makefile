CC      ?= gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
LDFLAGS = -lpcap -lgpiod -lmosquitto -pthread
TARGET  = ic905-relay
PREFIX  ?= /usr/local

SRCS = ic905_relay.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/bin/
	install -m 644 ic905-relay.service /etc/systemd/system/
	[ -f /etc/ic905-relay.conf ] || install -m 644 ic905-relay.conf /etc/
	systemctl daemon-reload

uninstall:
	systemctl stop ic905-relay 2>/dev/null || true
	systemctl disable ic905-relay 2>/dev/null || true
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f /etc/systemd/system/ic905-relay.service
	rm -f /etc/ic905-relay.conf
	systemctl daemon-reload

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install uninstall clean
