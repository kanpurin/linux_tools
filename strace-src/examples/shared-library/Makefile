CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g -O0 -fno-omit-frame-pointer
SOFLAGS := -fPIC
RPATH := -Wl,-rpath,'$$ORIGIN'

.PHONY: all clean run trace

all: shared-demo libmessage.so liboutput.so

liboutput.so: output.c output.h
	$(CC) $(CFLAGS) $(SOFLAGS) -shared -Wl,-soname,liboutput.so -o $@ output.c

libmessage.so: message.c message.h output.h liboutput.so
	$(CC) $(CFLAGS) $(SOFLAGS) -shared -Wl,-soname,libmessage.so $(RPATH) \
		-o $@ message.c -L. -loutput

shared-demo: main.c message.h libmessage.so
	$(CC) $(CFLAGS) $(RPATH) -o $@ main.c -L. -lmessage

run: all
	./shared-demo demo

trace: all
	../../strace-src ./shared-demo demo

clean:
	rm -f shared-demo libmessage.so liboutput.so
