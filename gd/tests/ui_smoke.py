"""Real ncurses regression tests. Run after make check; requires tmux and Python 3."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent


class Terminal:
    def __init__(self, fixture, language="en", locale="C.UTF-8"):
        self.session = f"gd-ui-smoke-{os.getpid()}-{time.time_ns()}"
        command = (
            f"exec env LC_ALL={shlex.quote(locale)} {shlex.quote(str(ROOT / 'gd'))} "
            f"{('--lang ' + language + ' ') if language else ''}{shlex.quote(str(ROOT / 'tests' / fixture))}"
        )
        self.tmux("new-session", "-d", "-s", self.session, "-x", "140", "-y", "46", command)

    def tmux(self, *args):
        return subprocess.check_output(["tmux", "-L", self.session, *args], text=True)

    def screen(self):
        return self.tmux("capture-pane", "-t", self.session, "-p")

    def keys(self, *keys):
        self.tmux("send-keys", "-t", self.session, *keys)

    def text(self, text):
        self.tmux("send-keys", "-t", self.session, "-l", text)

    def wait(self, text, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            screen = self.screen()
            if text in screen:
                return screen
            time.sleep(0.02)
        raise AssertionError(f"Timed out waiting for {text!r}\n{self.screen()}")

    def close(self):
        subprocess.run(["tmux", "-L", self.session, "kill-session", "-t", self.session], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_absent(self, text, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            screen = self.screen()
            if text not in screen:
                return screen
            time.sleep(0.02)
        raise AssertionError(f"Still displaying {text!r}\n{self.screen()}")


def variable_pane(screen):
    return screen.split("Variables", 1)[1].split("Stack", 1)[0]


def function_break(t, function):
    t.keys("F")
    t.wait("Function Breakpoint")
    t.text(function)
    t.wait("Function: " + function)
    t.keys("Enter")
    t.wait("Breakpoint set: " + function)


def source_and_variables():
    t = Terminal("frame_vars_fixture")
    try:
        t.wait("NOT STARTED")
        t.keys("?")
        t.wait("Execution")
        help_screen = t.screen()
        assert "Ctrl-g" in help_screen and "Ctrl-G" not in help_screen, help_screen
        started = time.monotonic()
        t.keys("Escape")
        t.wait_absent("Execution")
        # Allow slow CI hosts; Escape must not wait for ncurses' 1-second default.
        assert time.monotonic() - started < 0.8
        t.keys("B")
        t.wait("Conditional Breakpoint")
        t.wait("dev")
        t.keys("Escape")
        t.wait("cancelled")
        t.keys("/")
        t.wait("Search:")
        t.text("partial")
        t.keys("Escape")
        t.wait_absent("Search:")
        assert "^[" not in t.screen()
        t.keys("F2")
        t.wait("VIM navigation mode")
        t.keys("/")
        t.wait("Search forward:")
        t.text("return")
        t.keys("Enter")
        t.wait("Search 'return':")
        t.keys("Escape")
        t.wait("Search and highlights cleared")
        t.keys("n")
        t.wait("No previous search")
        t.keys("F2")
        t.wait("GDB control mode")
        function_break(t, "leaf")
        t.keys("r")
        t.wait("STOPPED")
        t.keys("d")
        screen = t.wait("Assembly view")
        assert "[ASM]" in screen.splitlines()[0] and "0x" in screen, screen
        t.keys("d")
        t.wait("Source view")
        t.keys("Tab", "Tab", "Down", "Enter")
        screen = t.wait("Frame #1:")
        pane = variable_pane(screen)
        assert "Args" in pane and "Locals" in pane, pane
        for name in ("dev", "value", "cursor"):
            assert len(re.findall(r"\b" + name + r"\s*=", pane)) == 1, pane
        t.keys("Tab", "Enter")
        screen = t.wait("status =")
        assert "status" in variable_pane(screen)
        t.keys("Down", "a")
        t.wait("Expression :")
        t.keys("Escape")
        t.wait("Variables")
        t.keys("Down", "Down", "Down", "Enter")
        t.wait("*cursor =")
        t.keys("a")
        t.wait("Matched variable")
        t.wait("(dev)->status")
        t.keys("Escape")
        # Stack selection must refresh the model for a different caller.
        t.keys("Tab", "Down", "Enter")
        screen = t.wait("Frame #2:")
        pane = variable_pane(screen)
        assert "adjusted" in pane and "cursor =" not in pane, pane
        print("TUI source/search/Escape/modes/frame variables/expansion/address: ok")
    finally:
        t.close()


def mixed_mode():
    t = Terminal("mixed_debug_fixture")
    try:
        t.wait("NOT STARTED")
        function_break(t, "mixed_nodebug_worker")
        t.keys("r")
        screen = t.wait("STOPPED")
        assert "[ASM]" in screen.splitlines()[0], screen
        t.keys("f")
        screen = t.wait("Source [EXEC]")
        assert "[ASM]" not in screen.splitlines()[0], screen
        print("TUI function breakpoint/Assembly/finish to Source: ok")
    finally:
        t.close()


def function_candidates():
    t = Terminal("functions_fixture")
    try:
        t.wait("NOT STARTED")
        t.keys("F")
        t.wait("Function Breakpoint")
        t.text("cache_fn_269")
        t.wait("Function: cache_fn_269")
        t.wait("cache_fn_269")
        t.keys("BSpace")
        t.wait("Function: cache_fn_26_")
        t.text("9")
        t.wait("Function: cache_fn_269")
        t.keys("Enter")
        t.wait("Breakpoint set: cache_fn_269")
        t.keys("F", "Escape")
        t.wait_absent("Function Breakpoint")
        print("TUI function candidate typing/backspace/set/Escape: ok")
    finally:
        t.close()


def missing_source():
    with tempfile.TemporaryDirectory(prefix="gd-ui-source-") as directory:
        source = Path(directory) / "main.c"
        program = Path(directory) / "fixture"
        source.write_text("int main(void) { return 0; }\n")
        subprocess.run(["gcc", "-g", "-O0", "-o", str(program), str(source)], check=True)
        source.unlink()
        t = Terminal(str(program))
        try:
            t.wait("NOT STARTED")
            t.keys("F")
            t.wait("Function Breakpoint")
            t.text("main")
            t.wait("Function: main")
            t.keys("Enter")
            t.wait("STOPS [B] 1")
            t.keys("r")
            screen = t.wait("STOPPED")
            assert "[ASM]" in screen.splitlines()[0] and "0x" in screen, screen
            print("TUI debug metadata with missing source: Assembly fallback ok")
        finally:
            t.close()


def project_search():
    t = Terminal("multi_fixture")
    try:
        t.wait("NOT STARTED")
        t.keys("C-g")
        t.wait("Project search (literal):")
        t.text("int helper(int value)")
        t.keys("Enter")
        screen = t.wait("multi_main.c:1")
        assert "multi_helper.c:1" in screen, screen
        t.keys("b")
        t.wait("Breakpoint set at")
        t.keys("Enter")
        t.wait("Search result: multi_helper.c:1")
        t.keys("[")
        t.wait("View history")
        assert "multi_main.c:" in t.screen()
        t.keys("r")
        screen = t.wait("STOPPED")
        assert "helper:" in screen.splitlines()[0], screen
        assert "multi_helper.c:" in screen, screen
        print("TUI cross-file search/result breakpoint/source history: ok")
    finally:
        t.close()


def language_defaults():
    for language, locale, expected in (
        (None, "C.UTF-8", "未実行"),
        ("auto", "C.UTF-8", "未実行"),
        ("en", "C.UTF-8", "NOT STARTED"),
        (None, "C", "NOT STARTED"),
    ):
        t = Terminal("fixture", language, locale)
        try:
            t.wait(expected)
            if language is None and locale == "C.UTF-8":
                t.keys("L")
                t.wait("NOT STARTED")
                t.keys("L")
                t.wait("未実行")
        finally:
            t.close()
    print("TUI UTF-8 Japanese default / English override / C fallback / L toggle: ok")


if __name__ == "__main__":
    language_defaults()
    source_and_variables()
    mixed_mode()
    function_candidates()
    missing_source()
    project_search()
