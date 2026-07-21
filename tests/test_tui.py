#!/usr/bin/env python3
"""Black-box PTY smoke test for CMNY's terminal lifecycle and CRUD flow."""

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
    environment.update({"TERM": "xterm-256color", "LC_ALL": "C.UTF-8"})
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


def send(master: int, data: bytes) -> None:
    os.write(master, data)
    drain(master)


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
        process, master = start(binary, database)
        for keys in (
            b"n", b"1", b"1.234\r", b"\x1542.75\r", b"\r",
            b"\x15   \r", b"\x15Testing\r",
            b"PTY integration entry\r",
            b"2", b"B", b"\r", b"100.00\r",
            b"R", b"d", b"y", b"u", b"r", b"1", b"D", b"1", b"y", b"b", b"q",
        ):
            send(master, keys)
        finish(process, master)

        with sqlite3.connect(database) as connection:
            rows = connection.execute(
                "SELECT kind, amount_cents, category, note FROM transactions"
                " ORDER BY id"
            ).fetchall()
            budget = connection.execute(
                "SELECT category, limit_cents FROM budgets"
            ).fetchone()
            recurring_count = connection.execute("SELECT count(*) FROM recurring").fetchone()[0]
            preferences = dict(connection.execute(
                "SELECT key, value FROM settings WHERE key IN "
                "('last_expense_category', 'screen')"
            ))
        expected = (1, 4275, "Testing", "PTY integration entry")
        assert rows == [expected, expected], rows
        assert budget == ("Testing", 10000), budget
        assert recurring_count == 0
        assert preferences == {"last_expense_category": "Testing", "screen": "activity"}
        backups = list(Path(directory).glob("cmny.db.backup-*"))
        assert len(backups) == 1, backups
        with sqlite3.connect(backups[0]) as connection:
            assert connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"

        process, master = start(binary, database)
        for keys in (b"d", b"y", b"u", b"d", b"y", b"d", b"y", b"q"):
            send(master, keys)
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM transactions").fetchone()[0] == 0
            assert connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"

        process, master = start(binary, database, rows=10, columns=30)
        send(master, b"q")
        finish(process, master)

        process, master = start(binary, database)
        send(master, b"n")
        process.send_signal(signal.SIGINT)
        drain(master)
        finish(process, master)

        process, master = start(binary, database)
        send(master, b"n")
        resize(process, master, 10, 30)
        resize(process, master, 24, 90)
        send(master, b"\x1b")
        send(master, b"q")
        finish(process, master)

        process, master = start(binary, database, theme="violet", no_color=False)
        for keys in (b"p", b"q"):
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
