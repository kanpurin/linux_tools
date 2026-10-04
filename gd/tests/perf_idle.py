"""Measure idle gd CPU time in a real terminal (Linux, tmux)."""

import os
from pathlib import Path
import time

from ui_smoke import Terminal, function_break


def cpu_seconds(pid):
    # Field 2 (comm) may contain spaces; fields after its closing ')' start at 3.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


t = Terminal("frame_vars_fixture")
try:
    t.wait("NOT STARTED")
    function_break(t, "main")
    t.keys("r")
    t.wait("STOPPED")
    pid = int(t.tmux("display-message", "-p", "-t", t.session, "#{pane_pid}"))
    time.sleep(0.3)
    start = cpu_seconds(pid)
    duration = 4
    time.sleep(duration)
    elapsed = cpu_seconds(pid) - start
    print(f"idle stopped: CPU={elapsed:.3f}s/{duration}s ({elapsed / duration * 100:.2f}%)")
finally:
    t.close()
