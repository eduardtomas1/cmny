#!/usr/bin/env python3
"""Black-box PTY tests for CMNY navigation, settings, persistence, and CRUD."""

from __future__ import annotations

import fcntl
import os
import pty
import re
import select
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import time
from datetime import date, timedelta
from pathlib import Path


def drain(master: int, seconds: float = 0.08) -> bytes:
    deadline = time.monotonic() + seconds
    output = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.02)
        if ready:
            try:
                chunk = os.read(master, 65536)
                if not chunk:
                    return bytes(output)
                output.extend(chunk)
            except OSError:
                return bytes(output)
    return bytes(output)


def start(
    binary: Path,
    database: Path,
    rows: int = 24,
    columns: int = 90,
    *,
    theme: str | None = "ocean",
    no_color: bool = True,
    dismiss_tutorial: bool = True,
    initial_output: list[bytes] | None = None,
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
    initial = drain(master, 0.35)
    if initial_output is not None:
        initial_output.append(initial)
    if dismiss_tutorial and b"GETTING STARTED" in initial:
        os.write(master, b"s")
        drain(master, 0.2)
    return process, master


def send(master: int, data: bytes, delay: float = 0.08) -> bytes:
    os.write(master, data)
    return drain(master, delay)


def mouse_click(master: int, x: int, y: int, *, double: bool = False) -> bytes:
    """Send an xterm X10 click using zero-based screen coordinates."""
    press = b"\x1b[M" + bytes((32, x + 33, y + 33))
    release = b"\x1b[M" + bytes((35, x + 33, y + 33))
    return send(master, (press + release) * (2 if double else 1), 0.45)


def mouse_wheel_up(master: int, x: int, y: int) -> bytes:
    """Send the portable xterm button-4 wheel event exposed by ncurses v1."""
    return send(master, b"\x1b[M" + bytes((96, x + 33, y + 33)), 0.3)


def resize(process: subprocess.Popen[bytes], master: int, rows: int, columns: int) -> bytes:
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    process.send_signal(signal.SIGWINCH)
    return drain(master)


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

        # First-run onboarding is explicit, skippable, and remembered.
        skipped_database = Path(directory) / "tutorial-skipped.db"
        initial: list[bytes] = []
        process, master = start(
            binary, skipped_database, dismiss_tutorial=False, initial_output=initial
        )
        assert b"GETTING STARTED" in initial[0] and b"FIVE SPACES" in initial[0], initial[0][-700:]
        skipped = send(master, b"s", 0.2)
        assert b"Tutorial skipped" in skipped, skipped[-500:]
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(skipped_database) as connection:
            assert connection.execute(
                "SELECT value FROM settings WHERE key='tutorial_seen'"
            ).fetchone()[0] == "1"

        initial = []
        process, master = start(
            binary, skipped_database, dismiss_tutorial=False, initial_output=initial
        )
        assert b"GETTING STARTED" not in initial[0], initial[0][-500:]
        send(master, b"q")
        finish(process, master)

        # A real ledger gets an exact, valid automatic backup on its next launch.
        auto_directory = Path(directory) / "auto"
        auto_directory.mkdir()
        auto_database = auto_directory / "cmny.db"
        process, master = start(binary, auto_database)
        send(master, b"q")
        finish(process, master)
        process, master = start(binary, auto_database)
        send(master, b"q")
        finish(process, master)
        automatic_backups = list(auto_directory.glob("cmny.db.auto-backup-*"))
        assert len(automatic_backups) == 1, automatic_backups
        with sqlite3.connect(automatic_backups[0]) as connection:
            assert connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"

        # Completing all five pages survives a resize and can be replayed from Manage.
        tutorial_database = Path(directory) / "tutorial-complete.db"
        initial = []
        process, master = start(
            binary, tutorial_database, dismiss_tutorial=False, initial_output=initial
        )
        expected_pages = (
            b"adds an expense or income",
            b"searchable command palet",
            b"Changes save automatically",
            b"disposable sandbox",
        )
        for marker in expected_pages:
            page = send(master, b"\r", 0.15)
            assert marker in page, (marker, page[-600:])
        small = resize(process, master, 10, 30)
        assert b"CMNY needs a little more room" in small, small[-500:]
        restored = resize(process, master, 14, 40)
        assert b" /  DEMO" in restored and b"disposable sandbox" in restored, restored[-700:]
        completed = send(master, b"\r", 0.2)
        assert b"Tutorial complete" in completed or b"Income" in completed, completed[-700:]
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(tutorial_database) as connection:
            assert connection.execute(
                "SELECT value FROM settings WHERE key='tutorial_seen'"
            ).fetchone()[0] == "1"

        empty_initial: list[bytes] = []
        process, master = start(
            binary, tutorial_database, rows=24, columns=80, initial_output=empty_initial
        )
        overview = empty_initial[0]
        assert b"This month is ready" in overview, overview[-700:]
        empty_spaces = (
            (b"2", b"No activity"),
            (b"3", b"No budgets"),
            (b"4", b"No spending to analy"),
            (b"5", b"No entries this month"),
        )
        for key, marker in empty_spaces:
            output = send(master, key, 0.15)
            assert marker in output, (key, marker, output[-700:])

        send(master, b"\x1bOF")
        send(master, b"\x1bOA")
        send(master, b"\x1bOA")
        replay = send(master, b"\r", 0.2)
        assert b"GETTING STARTED" in replay and b"FIVE SPACES" in replay, replay[-700:]
        send(master, b"s", 0.15)
        send(master, b"q")
        finish(process, master)

        # First run: exercise every main space, CRUD, recurring entries, and Manage.
        process, master = start(binary, database)
        for keys in (
            b"a", b"\r", b"1.234\r", b"\x1540 + 2.75\r", b"\r",
            b"\x15   \r", b"\x15Testing\r", b"\r", b"PTY integration entry\r",
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

        # Manage: Ocean -> Violet, Overview -> Activity, Add key a -> x.
        for keys in (
            b"5", b"\r", b"\x1bOB", b"\r", b"\x1bOB", b"\x1bOB", b"\r", b"e", b"x",
        ):
            send(master, keys)
        send(master, b"\x1bOF")      # Last Manage row.
        send(master, b"\x1bOA")      # Create backup now (penultimate row).
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
        guide = send(master, b"?", 0.2)
        assert b"KEYBINDINGS" in guide and b"NAVIGATION" in guide and b"MONEY" in guide, guide[-900:]
        assert b"x / e / d / u" in guide and b"MOUSE" in guide, guide[-900:]
        send(master, b"\x1b")
        relative_date = (date.today() - timedelta(days=1)).isoformat().encode()
        for keys in (
            b"X", b"\r", b"2 * 3 - 1\r", b"\x15yesterday\r", b"\r",
            b"\r", b"Persisted custom key\r",
            b"3",
        ):
            send(master, keys)
        send(master, b"\x1b", 0.15)
        for keys in (b"5", b"\x1b"):
            send(master, keys, 0.15)
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM transactions").fetchone()[0] == 3
            assert connection.execute(
                "SELECT amount_cents, category, note, occurred_on "
                "FROM transactions ORDER BY id DESC LIMIT 1"
            ).fetchone() == (500, "Testing", "Persisted custom key", relative_date.decode())

        # Deletion remains recoverable, and all entries can still be removed deliberately.
        process, master = start(binary, database, theme=None)
        for keys in (b"d", b"y", b"u", b"d", b"y", b"d", b"y", b"d", b"y", b"q"):
            send(master, keys)
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM transactions").fetchone()[0] == 0
            assert connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"

        # Ledger-native terminal workflow: accounts, payees, transfers, durable
        # undo, reconciliation, restart safety, conflicts, and responsive modals.
        ledger_database = Path(directory) / "ledger-ui.db"
        process, master = start(binary, ledger_database)
        send(master, b"m")
        for keys in (
            b"a", b"Checking\r", b"\r", b"CMNY Bank\r", b"100.00\r", b"\r",
            b"a", b"Savings\r", b"\x1bOB", b"\r", b"CMNY Bank\r", b"\r",
        ):
            send(master, keys)
        send(master, b"\x1bOF")
        for keys in (
            b"e", b"\x15Savings Vault\r", b"\r", b"\x15CMNY Bank\r",
            b"x", b"y", b"x", b"y",
        ):
            send(master, keys)
        send(master, b"\x1bOH", 0.2)
        send(master, b"\x1b", 0.2)

        # The account chooser appears only now that multiple active accounts exist.
        for keys in (
            b"a", b"\x1bOB", b"\r", b"\r", b"10.00\r", b"\r", b"\r",
            b"Corner Store\r", b"Account-aware expense\r",
        ):
            send(master, keys)
        for keys in (
            b"v", b"\x1bOB", b"\r", b"\x1bOB", b"\r", b"25.00\r", b"\r",
            b"Internal\r", b"Rainy day transfer\r",
        ):
            send(master, keys)
        activity = send(master, b"2", 0.2)
        assert b"Transfer" in activity and b"Internal" in activity, activity[-1000:]
        send(master, b"g")
        send(master, b"\x1bOB")
        send(master, b"\x1bOB")
        filtered = send(master, b"\r", 0.2)
        assert b"account:Checking" in filtered, filtered[-900:]
        cleared_filter = send(master, b"c", 0.2)
        assert b"all" in cleared_filter, cleared_filter[-900:]
        send(master, b"q")
        finish(process, master)

        with sqlite3.connect(ledger_database) as connection:
            accounts = connection.execute(
                "SELECT a.name,a.institution,COALESCE(SUM(CASE WHEN e.voided_at IS NULL "
                "THEN p.amount_minor ELSE 0 END),0),a.archived FROM accounts a "
                "LEFT JOIN postings p ON p.account_id=a.id LEFT JOIN entries e ON e.id=p.entry_id "
                "GROUP BY a.id ORDER BY a.sort_order"
            ).fetchall()
            normal = connection.execute(
                "SELECT e.payee,e.note,p.amount_minor FROM entries e "
                "JOIN postings p ON p.entry_id=e.id WHERE e.entry_type=1 AND e.voided_at IS NULL"
            ).fetchone()
            transfer = connection.execute(
                "SELECT e.id,SUM(p.amount_minor) FROM entries e JOIN postings p ON p.entry_id=e.id "
                "WHERE e.entry_type=2 AND e.voided_at IS NULL GROUP BY e.id"
            ).fetchone()
        assert accounts[1] == ("Checking", "CMNY Bank", 6500, 0), accounts
        assert accounts[2] == ("Savings Vault", "CMNY Bank", 2500, 0), accounts
        assert normal == ("Corner Store", "Account-aware expense", -1000), normal
        assert transfer is not None and transfer[1] == 0, transfer

        # Undo is stored in SQLite: restarting and pressing u reverses the transfer.
        process, master = start(binary, ledger_database)
        undone = send(master, b"u", 0.25)
        assert b"Undid latest ledger change" in undone, undone[-800:]
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(ledger_database) as connection:
            assert connection.execute(
                "SELECT count(*) FROM entries WHERE entry_type=2 AND voided_at IS NULL"
            ).fetchone()[0] == 0
            assert connection.execute(
                "SELECT expense_cents FROM (SELECT 1) LEFT JOIN "
                "(SELECT SUM(amount_cents) expense_cents FROM transactions WHERE kind=1) ON 1=1"
            ).fetchone()[0] == 1000

        # Reconcile Checking: opening +100.00 and expense -10.00 must both be
        # cleared before the 90.00 statement can finalize.
        process, master = start(binary, ledger_database)
        send(master, b"l")
        send(master, b"a")
        send(master, b"\x1bOB")
        send(master, b"\r")
        send(master, b"\r")
        session = send(master, b"90.00\r", 0.25)
        assert b"Discrepancy" in session, session[-900:]
        send(master, b" ")
        send(master, b"\x1bOB")
        zero = send(master, b" ", 0.25)
        assert b"Discrepancy" in zero and b"0.00" in zero, zero[-900:]
        send(master, b"f")
        finalized = send(master, b"y", 0.25)
        assert b"RECONCILIATIONS" in finalized, finalized[-900:]
        send(master, b"\x1b")
        reconciled_activity = send(master, b"2", 0.2)
        assert b"[R]" in reconciled_activity, reconciled_activity[-1000:]
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(ledger_database) as connection:
            assert connection.execute(
                "SELECT status FROM reconciliation_sessions ORDER BY id DESC LIMIT 1"
            ).fetchone()[0] == 2
            assert connection.execute(
                "SELECT count(*) FROM postings p JOIN entries e ON e.id=p.entry_id "
                "WHERE p.account_id=(SELECT id FROM accounts WHERE name='Checking') "
                "AND e.voided_at IS NULL AND p.clear_state=2"
            ).fetchone()[0] == 2

        # An open session survives terminal resize and process restart; explicit
        # cancellation restores prior states without silently discarding work.
        process, master = start(binary, ledger_database)
        send(master, b"l")
        send(master, b"a")
        send(master, b"\x1bOB")
        send(master, b"\x1bOB")
        send(master, b"\r")
        send(master, b"\r")
        send(master, b"\r")
        small_reconcile = resize(process, master, 10, 30)
        assert b"CMNY needs a little more room" in small_reconcile, small_reconcile[-600:]
        restored_reconcile = resize(process, master, 24, 90)
        assert b"RECONCILE" in restored_reconcile, restored_reconcile[-800:]
        send(master, b"\x1b")
        send(master, b"\x1b")
        send(master, b"q")
        finish(process, master)
        process, master = start(binary, ledger_database)
        resumed = send(master, b"l", 0.25)
        assert b"Savings Vault" in resumed and b"OPEN SESSION" in resumed, resumed[-900:]
        send(master, b"\r")
        send(master, b"x")
        send(master, b"y")
        send(master, b"\x1b")
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(ledger_database) as connection:
            assert connection.execute(
                "SELECT status FROM reconciliation_sessions ORDER BY id DESC LIMIT 1"
            ).fetchone()[0] == 3

        # A changed revision is reported as a conflict, not overwritten by undo.
        process, master = start(binary, ledger_database)
        for keys in (
            b"a", b"\r", b"\r", b"1.00\r", b"\r", b"\x15Misc\r",
            b"\r", b"Conflict probe\r",
        ):
            send(master, keys)
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(ledger_database) as connection:
            entry_id = connection.execute(
                "SELECT id FROM entries WHERE entry_type=1 AND voided_at IS NULL ORDER BY id DESC LIMIT 1"
            ).fetchone()[0]
            connection.execute("UPDATE entries SET revision=revision+1 WHERE id=?", (entry_id,))
        process, master = start(binary, ledger_database)
        conflict = send(master, b"u", 0.25)
        assert b"entry changed after this history action" in conflict, conflict[-900:]
        send(master, b"q")
        finish(process, master)

        # Reset keybindings from Manage and verify the reset is persistent.
        process, master = start(binary, database, theme=None)
        for keys in (b"5", b"\x1bOF", b"\r", b"y", b"q"):
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

        # The five-space shell and command palette remain complete at 40x14.
        process, master = start(binary, database, rows=14, columns=40)
        palette = send(master, b":", 0.15)
        assert b"COMMAND PALETTE" in palette, palette[-500:]

        # Arrow navigation: Overview, Activity, then Plan is the third command.
        send(master, b"\x1bOB")
        send(master, b"\x1bOB")
        plan = send(master, b"\r", 0.15)
        assert b"BUDGETS" in plan or b"RECURRING" in plan, plan[-500:]

        # Cancel keeps the current space, and Esc then performs universal back.
        send(master, b":")
        send(master, b"backup")
        cancelled = send(master, b"\x1b", 0.15)
        assert b"BUDGETS" in cancelled or b"RECURRING" in cancelled, cancelled[-500:]
        previous = send(master, b"\x1b", 0.15)
        assert b"ACTIVITY" in previous, previous[-500:]

        # Search text survives a resize and executes the single matching command.
        send(master, b":")
        send(master, b"manage")
        resize(process, master, 10, 30)
        restored_palette = resize(process, master, 14, 40)
        assert b"manage" in restored_palette.lower(), restored_palette[-500:]
        manage = send(master, b"\r", 0.15)
        assert b"MANAGE" in manage, manage[-500:]
        back = send(master, b"\x1b", 0.15)
        assert b"ACTIVITY" in back, back[-500:]

        # Numeric navigation covers all five spaces; brackets retain period control.
        expected_spaces = (
            (b"1", (b"Income", b"RECENT ACTIVITY")),
            (b"2", (b"ACTIVITY",)),
            (b"3", (b"BUDGETS", b"RECURRING")),
            (b"4", (b"SPENDING BY CATEGORY", b"SIX-MONTH SPEND")),
            (b"5", (b"MANAGE",)),
        )
        for key, markers in expected_spaces:
            output = send(master, key, 0.15)
            assert any(marker in output for marker in markers), (key, output[-500:])
        send(master, b"\x1b")       # Manage -> Insights.
        send(master, b"[")          # Previous period.
        send(master, b"]")          # Next period.
        guide = send(master, b"?", 0.15)
        assert b"KEYBINDINGS" in guide and b"NAVIGATION" in guide and b"MONEY" in guide, guide[-700:]
        mouse_guide = send(master, b"\x1bOF", 0.15)
        assert b"Mouse auto" in mouse_guide and b"Shift-drag" in mouse_guide, mouse_guide[-700:]
        send(master, b"\x1b")
        send(master, b"q")
        finish(process, master)

        # Theme changes apply in color mode and survive the next launch.
        process, master = start(binary, database, theme="violet", no_color=False)
        for keys in (b"5", b"\r", b"q"):
            send(master, keys)
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute(
                "SELECT value FROM settings WHERE key='theme'"
            ).fetchone()[0] == "amber"

        process, master = start(binary, database, theme=None, no_color=False)
        send(master, b"5")
        send(master, b"\r")       # Amber -> High Contrast.
        send(master, b"\r")       # High Contrast -> Monochrome.
        send(master, b"q")
        finish(process, master)
        with sqlite3.connect(database) as connection:
            assert connection.execute(
                "SELECT value FROM settings WHERE key='theme'"
            ).fetchone()[0] == "monochrome"

        # An explicit theme is accepted while --no-color remains authoritative.
        initial = []
        process, master = start(
            binary, database, theme="high-contrast", no_color=True, initial_output=initial
        )
        plain = initial[0] + send(master, b"5", 0.15)
        color_sgr = re.compile(rb"\x1b\[(?:[0-9]+;)*3[0-7](?:;[0-9]+)*m")
        assert color_sgr.search(plain) is None, plain[-700:]
        send(master, b"q")
        finish(process, master)

        # Optional mouse input is additive: clicks and wheel-up work when the
        # linked curses/terminfo stack negotiates X10 mouse reporting.
        mouse_database = Path(directory) / "mouse.db"
        process, master = start(binary, mouse_database)
        for amount, category, note in (
            (b"1.00\r", b"First\r", b"First mouse entry\r"),
            (b"2.00\r", b"Second\r", b"Second mouse entry\r"),
        ):
            for keys in (b"a", b"\r", amount, b"\r", b"\x15" + category, b"\r", note):
                send(master, keys)
        send(master, b"1")

        # At 90 columns Activity begins at x=10; a click activates that tab.
        activity = mouse_click(master, 12, 1)
        mouse_available = b"ACTIVITY  1-2/2" in activity
        if mouse_available:
            # Compact navigation recomputes its hit target after a resize.
            resize(process, master, 14, 40)
            compact_plan = mouse_click(master, 19, 1)
            assert b"BUDGETS" in compact_plan or b"RECURRING" in compact_plan, compact_plan[-500:]
            resize(process, master, 24, 90)
            activity = mouse_click(master, 12, 1)
            assert b"ACTIVITY  1-2/2" in activity, activity[-500:]

            # Single-click selects the older second row; Enter activates it.
            mouse_click(master, 5, 6)
            detail = send(master, b"\r", 0.2)
            assert b"TRANSACTION DETAIL" in detail and b"First mouse entry" in detail, detail[-700:]
            send(master, b"\x1b")

            # Wheel-up selects the newer first row without changing key behavior.
            mouse_wheel_up(master, 5, 6)
            detail = send(master, b"\r", 0.2)
            assert b"Second mouse entry" in detail, detail[-700:]
            send(master, b"\x1b")

            # Double-click activates Activity and command-palette rows.
            detail = mouse_click(master, 5, 6, double=True)
            assert b"TRANSACTION DETAIL" in detail and b"First mouse entry" in detail, detail[-700:]
            send(master, b"\x1b")
            send(master, b":")
            plan = mouse_click(master, 10, 7, double=True)
            assert b"BUDGETS" in plan or b"RECURRING" in plan, plan[-500:]

            # Palette wheel-up moves from Activity back to Overview.
            send(master, b":")
            send(master, b"\x1bOB")
            mouse_wheel_up(master, 10, 5)
            overview = send(master, b"\r", 0.2)
            assert b"RECENT ACTIVITY" in overview, overview[-500:]

            # Manage wheel-up moves Reset -> Backup, and double-clicking Mouse
            # changes Auto -> On. A keyboard activation then persists Off.
            manage = mouse_click(master, 44, 1)
            assert b"MANAGE" in manage, manage[-500:]
            send(master, b"\x1bOF")
            mouse_wheel_up(master, 8, 12)
            backup = send(master, b"\r", 0.2)
            assert b"Backup created:" in backup, backup[-500:]
            send(master, b"\x1bOH")
            mouse_click(master, 8, 7, double=True)
            send(master, b"\r")
        else:
            # Raw mouse packets are intentionally non-fatal when curses does
            # not expose mouse support; all required interactions stay keyed.
            send(master, b"5")
            send(master, b"\x1bOB")
            send(master, b"\x1bOB")
            send(master, b"\r")
            send(master, b"\r")
        send(master, b"q")
        finish(process, master)

        with sqlite3.connect(mouse_database) as connection:
            assert connection.execute(
                "SELECT value FROM settings WHERE key='mouse'"
            ).fetchone()[0] == "off"

        # With mouse explicitly off, the complete keyboard path remains live.
        process, master = start(binary, mouse_database)
        activity = send(master, b"2", 0.15)
        assert b"ACTIVITY  1-2/2" in activity, activity[-500:]
        send(master, b":")
        send(master, b"plan")
        plan = send(master, b"\r", 0.15)
        assert b"BUDGETS" in plan or b"RECURRING" in plan, plan[-500:]
        back = send(master, b"\x1b", 0.15)
        assert b"ACTIVITY  1-2/2" in back, back[-500:]
        send(master, b"q")
        finish(process, master)

    print("ok - TUI PTY tests")


if __name__ == "__main__":
    main()
