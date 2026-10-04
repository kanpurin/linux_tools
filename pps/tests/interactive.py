#!/usr/bin/env python3
"""Exercise Process Select through a pseudo-terminal."""

import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import sys
import termios
import time

CORE = sys.argv[1] if len(sys.argv) > 1 else "./bin/pps-core"
STATUS = re.compile(rb":\s+(\d+)/(\d+)")
RETURN_PID = re.compile(rb"PPS_(?:SELECT|SETVAR)\s+(\d+)")
MISSING = re.compile(rb"Pattern not found:")


def make_controlling_tty():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


class Session:
    def __init__(self, *args):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        self.proc = subprocess.Popen([CORE, *args], stdin=slave, stdout=slave,
                                     stderr=slave, preexec_fn=make_controlling_tty)
        os.close(slave)
        self.fd = master
        self.initial = self.read_until(STATUS)

    def read_until(self, pattern):
        output = bytearray()
        deadline = time.monotonic() + 5
        while not pattern.search(output):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise AssertionError(f"no {pattern.pattern!r}: {output[-300:]!r}")
            output.extend(os.read(self.fd, 65536))
        return bytes(output)

    def send(self, keys, pattern=STATUS):
        os.write(self.fd, keys)
        return self.read_until(pattern)

    def return_pid(self, key=b"P"):
        output = self.send(key, RETURN_PID)
        self.proc.wait(timeout=2)
        assert b"\x1b[?1049l" in output, "alternate screen not restored"
        return int(RETURN_PID.search(output).group(1))

    def close(self):
        if self.proc.poll() is None:
            try:
                os.write(self.fd, b"q")
                self.proc.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.kill()
                self.proc.wait()
        os.close(self.fd)


def position(output):
    matches = STATUS.findall(output)
    assert matches, f"missing status: {output[-300:]!r}"
    return tuple(map(int, matches[-1]))


def victim(label):
    name = f"pps-ui-test-{label}-{os.getpid()}-{time.monotonic_ns()}"
    proc = subprocess.Popen(["bash", "-c", f"exec -a {name} sleep 60"],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    return name, proc


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def test_navigation():
    s = Session()
    try:
        total = position(s.initial)[1]
        assert total > 30
        assert position(s.send(b"j"))[0] == 2
        assert position(s.send(b"k"))[0] == 1
        assert position(s.send(b"f"))[0] == 23
        assert position(s.send(b"b"))[0] == 1
        assert position(s.send(b"d"))[0] == 12
        assert position(s.send(b"u"))[0] == 1
        assert position(s.send(b"G"))[0] == total
        assert position(s.send(b"g"))[0] == 1
        assert position(s.send(b"\x1b[C"))[0] == 1
        assert position(s.send(b"\x1b[D"))[0] == 1
        s.send(b"h", re.compile(rb"HELP -- q to return"))
        assert position(s.send(b"q"))[0] == 1
        assert s.return_pid() > 0
    finally:
        s.close()

    s = Session()
    try:
        assert s.return_pid(b"\r") > 0
    finally:
        s.close()


def test_search_sort_refresh():
    name, child = victim("search")
    try:
        s = Session()
        try:
            s.send(b"/" + name.encode() + b"\r")
            for key in b"cmstp":
                s.send(b"o" + bytes([key]))
                s.send(b"r")
            assert s.return_pid() == child.pid
        finally:
            s.close()
    finally:
        stop(child)

    name, child = victim("sort-first")
    try:
        s = Session()
        try:
            for key in b"cmstp":
                s.send(b"o" + bytes([key]))
            s.send(b"/" + name.encode() + b"\r")
            assert s.return_pid() == child.pid
        finally:
            s.close()
    finally:
        stop(child)

    s = Session()
    child = None
    try:
        name, child = victim("new")
        time.sleep(0.02)
        s.send(b"r")
        s.send(b"/" + name.encode() + b"\r")
        assert s.return_pid() == child.pid
    finally:
        s.close()
        if child:
            stop(child)


def test_filter():
    name, first = victim("filter")
    try:
        one = subprocess.run([CORE, name], capture_output=True, timeout=3)
        assert one.returncode == 0 and str(first.pid).encode() in one.stdout
        second = subprocess.Popen(["bash", "-c", f"exec -a {name} sleep 60"],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        try:
            s = Session(name)
            try:
                assert position(s.initial)[1] == 2
                assert s.return_pid() in (first.pid, second.pid)
            finally:
                s.close()
        finally:
            stop(second)
    finally:
        stop(first)


def test_exited_process():
    name, child = victim("exited")
    try:
        s = Session()
        try:
            initial_count = position(s.initial)[1]
            stop(child)
            output = s.send(b"G")
            assert b"[process exited]" not in output
            assert position(output)[1] <= initial_count - 1
            assert MISSING.search(s.send(b"/" + name.encode() + b"\r", MISSING))
        finally:
            s.close()
    finally:
        stop(child)


def test_stale_selection():
    name, child = victim("stale")
    try:
        s = Session()
        try:
            s.send(b"/" + name.encode() + b"\r")
            stop(child)
            warning = re.compile(rb"no longer exists or changed")
            assert warning.search(s.send(b"P", warning))
            assert s.proc.poll() is None, "stale PID was returned"
        finally:
            s.close()
    finally:
        stop(child)


def test_many_exited():
    children = [victim("batch")[1] for _ in range(48)]
    try:
        s = Session()
        try:
            before = position(s.initial)[1]
            for child in children:
                stop(child)
            after = position(s.send(b"oc"))[1]
            assert after <= before - len(children), (before, after)
            assert position(s.send(b"G"))[1] == after
        finally:
            s.close()
    finally:
        for child in children:
            stop(child)


def test_signal():
    name, child = victim("signal")
    try:
        s = Session()
        try:
            s.send(b"/" + name.encode() + b"\r")
            s.send(b"ss")
            state = subprocess.check_output(["ps", "-o", "stat=", "-p", str(child.pid)])
            assert state[:1].upper() == b"T", state
            s.send(b"sc")
            state = subprocess.check_output(["ps", "-o", "stat=", "-p", str(child.pid)])
            assert state[:1].upper() != b"T", state
            s.send(b"st")
            child.wait(timeout=2)
            assert child.returncode == -15
        finally:
            s.close()
    finally:
        stop(child)

    name, child = victim("signal-race")
    try:
        s = Session()
        try:
            s.send(b"/" + name.encode() + b"\r")
            s.send(b"s", re.compile(rb"Signal: \[t\]TERM"))
            stop(child)
            warning = re.compile(rb"no longer exists or changed")
            assert warning.search(s.send(b"t", warning))
            assert s.proc.poll() is None
        finally:
            s.close()
    finally:
        stop(child)


if __name__ == "__main__":
    for case in (test_navigation, test_search_sort_refresh, test_filter,
                 test_exited_process, test_stale_selection, test_many_exited,
                 test_signal):
        case()
        print(f"{case.__name__}: pass")
