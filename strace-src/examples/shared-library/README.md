# Shared-library source-resolution demo

This example verifies that `strace-src` can resolve and select source frames
across shared-object boundaries:

```text
shared-demo (main.c)
  -> libmessage.so (message.c)
       -> liboutput.so (output.c)
            -> openat / write / close
```

Build and run it from this directory:

```sh
make
make run
make trace
```

In `strace-src`, search for `/strace-src-shared-library-demo`, select the
`openat` or `write` trace, and press `s`. The Stack popup should offer source
frames from `output.c`, `message.c`, and `main.c`; select one with `Up`/`Down`
and press Enter to show that file in the Source pane. These local files appear
as highlighted `[source]` frames, while libc locations such as `open64.c` are
shown dimmed as `[file unavailable]` when the corresponding source tree is not
installed.

The executable and both shared libraries embed debug information and use an
`$ORIGIN` runtime search path, so the example runs directly from its build
directory without installing the libraries system-wide.
