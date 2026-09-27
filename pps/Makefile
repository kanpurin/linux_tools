CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=

CORE := bin/pps-core
SOURCE := src/pps.c
PREFIX ?= $(HOME)/.local
INSTALL_DIR ?= $(PREFIX)/lib/pps
USER_BIN_DIR ?= $(PREFIX)/bin
BASHRC ?= $(HOME)/.bashrc

.DEFAULT_GOAL := setup
.PHONY: all setup build prepare clean test install

all: setup

setup: install

build: prepare $(CORE)

prepare:
	chmod +x bin/pps

$(CORE): $(SOURCE) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

bin:
	mkdir -p $@

test: build
	bash tests/run.sh

install: build
	install -d "$(INSTALL_DIR)/bin" "$(INSTALL_DIR)/shell" "$(USER_BIN_DIR)"
	install -m 755 "$(CORE)" "$(INSTALL_DIR)/bin/pps-core"
	install -m 755 bin/pps "$(INSTALL_DIR)/bin/pps"
	install -m 644 shell/pps.bash "$(INSTALL_DIR)/shell/pps.bash"
	install -m 644 README.md "$(INSTALL_DIR)/README.md"
	install -m 755 bin/pps "$(USER_BIN_DIR)/pps"
	install -d "$(dir $(BASHRC))"
	@touch "$(BASHRC)"
	@line='source "$(INSTALL_DIR)/shell/pps.bash"'; \
	if ! grep -Fqx "$$line" "$(BASHRC)"; then \
		printf '\n# pps Bash integration\n%s\n' "$$line" >> "$(BASHRC)"; \
		printf '%s\n' 'Added pps Bash integration to $(BASHRC).'; \
	else \
		printf '%s\n' 'pps Bash integration is already present in $(BASHRC).'; \
	fi
	@printf '%s\n' \
	  'pps installed in $(INSTALL_DIR).' \
	  'It will be available in newly opened Bash sessions.' \
	  'For this already-running shell, run: source "$(BASHRC)"'

clean:
	rm -f $(CORE)
