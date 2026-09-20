.PHONY: all procview testforge autotest-assist-tui gd clean

all: procview testforge autotest-assist-tui gd

procview:
	$(MAKE) -C procview

testforge:
	$(MAKE) -C testforge

autotest-assist-tui:
	$(MAKE) -C autotest-assist-tui

gd:
	$(MAKE) -C gd

clean:
	$(MAKE) -C procview clean
	$(MAKE) -C testforge clean
	$(MAKE) -C autotest-assist-tui clean
	$(MAKE) -C gd clean
