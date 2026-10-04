#!/usr/bin/env python3
"""Compare idle CPU and terminal writes using a saved trace and open popup."""
import argparse
import os
import pty
import select
import subprocess
import tempfile
import time


def collect(master, duration):
    result = bytearray()
    end = time.monotonic() + duration
    while time.monotonic() < end:
        ready, _, _ = select.select([master], [], [], max(0, end - time.monotonic()))
        if ready:
            try:
                result.extend(os.read(master, 65536))
            except OSError:
                break
    return bytes(result)


def ticks(pid):
    with open(f"/proc/{pid}/stat") as stream:
        fields = stream.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def measure(binary, trace):
    master, slave = pty.openpty()
    process = subprocess.Popen([binary, "--lang", "ja", trace], stdin=slave,
                               stdout=slave, stderr=slave,
                               env={**os.environ, "TERM": "xterm", "LC_ALL": "C.UTF-8"})
    os.close(slave)
    try:
        initial = collect(master, 1)
        assert "トレースログ".encode() in initial
        os.write(master, b"?")
        opened = collect(master, 0.5)
        assert "ヘルプ".encode() in opened
        start = ticks(process.pid)
        idle = collect(master, 3)
        elapsed = (ticks(process.pid) - start) / os.sysconf("SC_CLK_TCK")
        os.write(master, b"qL")
        changed = collect(master, 0.5)
        assert b"Trace Log" in changed
        os.write(master, b"q")
        collect(master, 0.5)
        assert process.wait(timeout=3) == 0
        print(f"{binary}: idle CPU={elapsed:.3f}s/3s, terminal bytes={len(idle)}")
        return len(idle)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="strace-src-perf-") as directory:
        trace = os.path.join(directory, "idle.trace")
        master, slave = pty.openpty()
        producer = subprocess.Popen(["./strace-src", "-o", trace, "./tests/fd_fixture"],
                                    stdin=slave, stdout=slave, stderr=slave,
                                    env={**os.environ, "TERM": "xterm", "LC_ALL": "C.UTF-8"})
        os.close(slave)
        try:
            collect(master, 1)
            os.write(master, b"q")
            collect(master, 0.5)
            assert producer.wait(timeout=3) == 0
        finally:
            if producer.poll() is None:
                producer.kill()
                producer.wait()
            os.close(master)
        if args.baseline:
            measure(args.baseline, trace)
        assert measure("./strace-src", trace) == 0, "idle popup must not redraw"


if __name__ == "__main__":
    main()
