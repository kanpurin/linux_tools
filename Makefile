.PHONY: all procview testforge autotest-assist-tui gd ltree strace-src pps clean

all: procview testforge autotest-assist-tui gd ltree strace-src pps

procview:
	$(MAKE) -C procview

testforge:
	$(MAKE) -C testforge

autotest-assist-tui:
	$(MAKE) -C autotest-assist-tui

gd:
	$(MAKE) -C gd

ltree:
	$(MAKE) -C ltree

strace-src:
	$(MAKE) -C strace-src

pps:
	$(MAKE) -C pps build

clean:
	$(MAKE) -C procview clean
	$(MAKE) -C testforge clean
	$(MAKE) -C autotest-assist-tui clean
	$(MAKE) -C gd clean
	$(MAKE) -C ltree clean
	$(MAKE) -C strace-src clean
	$(MAKE) -C pps clean
