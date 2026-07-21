#!/usr/bin/env python3
"""Black-box PTY tests for CMNY navigation, settings, persistence, and CRUD."""

from __future__ import annotations

import fcntl
import os
import pty
import select
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path


def drain(master: int, seconds: float = 0.08) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.02)
        if ready:
            try:
                if not os.read(master, 65536):
                    return
            except OSError:
                return


def start(
    binary: Path,
    database: Path,
    rows: int = 24,
    columns: int = 90,
    *,
    theme: str | None = "ocean",
    no_color: bool = True,
) -> tuple[subprocess.Popen[bytes], int]:
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    environment = os.environ.copy()
    environment.pop("NO_COLOR", None)
    environment.update({"TERM": "xterm-256color", "LC_ALL": "C.UTF-8", "ESCDELAY": "25"})
    command = [str(binary), "--db", str(database), "--ascii"]
    if theme is not None:
        command.extend(["--theme", theme])
    if no_color:
        command.append("--no-color")
    process = subprocess.Popen(
        command,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=environment,
        close_fds=True,
    )
    os.close(slave)
    drain(master, 0.25)
    return process, master


def send(master: int, data: bytes, delay: float = 0.08) -> None:
    os.write(master, data)
    drain(master, delay)


def resize(process: subprocess.Popen[bytes], master: int, rows: int, columns: int) -> None:
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    process.send_signal(signal.SIGWINCH)
    drain(master)


def finish(process: subprocess.Popen[bytes], master: int) -> None:
    drain(master, 0.15)
    process.wait(timeout=3)
    os.close(master)
    if process.returncode != 0:
        raise AssertionError(f"CMNY exited with {process.returncode}")


def main() -> None:
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/cmny").resolve()
    with tempfile.TemporaryDirectory(prefix="cmny-tui-", dir="build") as directory:
        database = Path(directory) / "cmny.db"

        # First run: exercise every main screen, CRUD, recurring entries, and Settings.
        process, master = start(binary, database)
        for keys in (
            b"a", b"\r", b"1.234\r", b"\x1542.75\r", b"\r",
            b"\x15   \r", b"\x15Testing\r", b"PTY integration entry\r",
            b"\t", b"\x1b[Z", b"3",
        ):
            send(master, keys)
        send(master, b"\x1b", 0.15)  # Reports -> Overview.
        send(master, b"2")
        send(master, b"\x7f")       # Activity -> Overview.
        for keys in (
            b"2", b"b", b"\r", b"100.00\r",
            b"/", b"Testing\r", b"c", b"f", b"c",
            b"r", b"a", b"\r", b"d", b"y",
        ):
            send(master, keys)
        send(master, b"\x1b", 0.15)  # Leave recurring manager.
        for keys in (
            b"d", b"y", b"u",       # Delete and restore the selected entry.
            b"\x1bOD", b"\x1bOC",   # Previous and next month.
            b"?",
        ):
            send(master, keys)
        send(master, b"\x1b", 0.15)  # Leave Help.

        # Settings: Ocean -> Violet, Overview -> Activity, Add key a -> x.
        for keys in (b"4", b"\r", b"\x1bOB", b"\r", b"\x1bOB", b"\r", b"e", b"x"):
            send(master, keys)
        send(master, b"\x1bOF")      # Last Settings row.
        send(master, b"\x1bOA")      # Create backup now.
        send(master, b"\r")
        send(master, b"\x1b", 0.15)  # Settings -> Overview.
        send(master, b"q")
        finish(process, master)

        with sqlite3.connect(database) as connection:
            rows = connection.execute(
                "SELECT kind, amount_cents, category, note FROM transactions ORDER BY id"
            ).fetchall()
            budget = connection.execute(
                "SELECT category, limit_cents FROM budgets"
            ).fetchone()
            recurring_count = connection.execute("SELECT count(*) FROM recurring").fetchone()[0]
            preferences = dict(connection.execute(
                "SELECT key, value FROM settings WHERE key IN "
                "('last_expense_category', 'theme', 'start_screen', 'key_add')"
            ))
        expected = (1, 4275, "Testing", "PTY integration entry")
        assert rows == [expected, expected], rows
        assert budget == ("Testing", 10000), budget
        assert recurring_count == 0
        assert preferences == {
            "key_add": "x",
            "last_expense_category": "Testing",
            "start_screen": "activity",
            "theme": "violet",
        }, preferences
        backups = list(Path(directory).glob("cmny.db.backup-*"))
        assert len(backups) == 1, backups
        with sqlite3.connect(backups[0]) as connection:
            assert connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"

        # Restart without explicit preferences: Activity, Violet, and x must be restored.
        process, master = start(binary, database, theme=None, no_color=False)
        for keys in (
            b"X", b"\r", b"5.00\r", b"\r", b"\r", b"Persisted custom key\r",
            b"3",
        ):
            send(master, keys)
        send(master, b"\x1b", 0.15)
        for keys in (b"4", b"\x1b"):
            send(master, keys, 0.15)
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM transactions").fetchone()[0] == 3
            assert connection.execute(
                "SELECT amount_cents, category, note FROM transactions ORDER BY id DESC LIMIT 1"
            ).fetchone() == (500, "Testing", "Persisted custom key")

        # Deletion remains recoverable, and all entries can still be removed deliberately.
        process, master = start(binary, database, theme=None)
        for keys in (b"d", b"y", b"u", b"d", b"y", b"d", b"y", b"d", b"y", b"q"):
            send(master, keys)
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM transactions").fetchone()[0] == 0
            assert connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"

        # Reset keybindings from Settings and verify the reset is persistent.
        process, master = start(binary, database, theme=None)
        for keys in (b"4", b"\x1bOF", b"\r", b"y", b"q"):
            send(master, keys)
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute(
                "SELECT value FROM settings WHERE key='key_add'"
            ).fetchone()[0] == "a"

        # Small terminals, interrupted forms, and live resizes must exit cleanly.
        process, master = start(binary, database, rows=10, columns=30)
        send(master, b"q")
        finish(process, master)

        process, master = start(binary, database)
        send(master, b"a")
        process.send_signal(signal.SIGINT)
        drain(master)
        finish(process, master)

        process, master = start(binary, database)
        send(master, b"a")
        resize(process, master, 10, 30)
        resize(process, master, 24, 90)
        send(master, b"\x1b", 0.15)
        send(master, b"q")
        finish(process, master)

        # Theme changes apply in color mode and survive the next launch.
        process, master = start(binary, database, theme="violet", no_color=False)
        for keys in (b"4", b"\r", b"q"):
            send(master, keys)
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute(
                "SELECT value FROM settings WHERE key='theme'"
            ).fetchone()[0] == "amber"

        process, master = start(binary, database, theme=None, no_color=False)
        send(master, b"q")
        finish(process, master)

    print("ok - TUI PTY tests")


if __name__ == "__main__":
    main()
