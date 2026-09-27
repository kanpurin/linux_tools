CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?= -lncursesw

TARGET := lifewatch
SRC := lifewatch.c

PREFIX ?= /usr/local
SYSCONFDIR ?= /etc

.PHONY: all clean install uninstall test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: $(TARGET)
	sh ./tests/run.sh ./$(TARGET)

install: $(TARGET)
	install -Dm0755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/$(TARGET)
	install -Dm0644 packaging/lifewatch.service $(DESTDIR)$(SYSCONFDIR)/systemd/system/lifewatch.service
	install -Dm0644 packaging/lifewatch.logrotate $(DESTDIR)$(SYSCONFDIR)/logrotate.d/lifewatch
	install -d -m0750 $(DESTDIR)/var/log/lifewatch
	@if [ ! -e "$(DESTDIR)$(SYSCONFDIR)/lifewatch.conf" ]; then \
		install -Dm0640 packaging/lifewatch.conf $(DESTDIR)$(SYSCONFDIR)/lifewatch.conf; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/$(TARGET)
	rm -f $(DESTDIR)$(SYSCONFDIR)/systemd/system/lifewatch.service
	rm -f $(DESTDIR)$(SYSCONFDIR)/logrotate.d/lifewatch

clean:
	rm -f $(TARGET)
