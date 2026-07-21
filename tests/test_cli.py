#!/usr/bin/env python3
"""Black-box checks for CMNY's non-interactive data commands."""

from __future__ import annotations

import sqlite3
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run(binary: Path, *arguments: str, expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([binary, *arguments], text=True, capture_output=True)
    if result.returncode != expected:
        raise AssertionError(
            f"expected exit {expected}, got {result.returncode}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def main() -> None:
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/cmny").resolve()
    with tempfile.TemporaryDirectory(prefix="cmny-cli-", dir="build") as temporary:
        directory = Path(temporary)
        database = directory / "ledger.db"
        incoming = directory / "incoming.csv"
        second = directory / "second.csv"
        exported = directory / "export.csv"
        backup = directory / "ledger.backup"
        incoming.write_text(
            "kind,amount,category,note,date\n"
            "income,3000.00,Salary,Monthly salary,2026-07-01\n"
            "expense,42.75,Food,Groceries,2026-07-02\n",
            encoding="ascii",
        )
        second.write_text(
            "kind,amount,category,note,date\n"
            "expense,10.00,Fun,Cinema,2026-07-03\n",
            encoding="ascii",
        )

        assert run(binary, "--version").stdout == "cmny 0.3.0\n"
        assert "Ledger check: OK" in run(binary, "--db", str(database), "--check").stdout
        refused = run(binary, "--db", str(database), "--import", str(incoming), expected=1)
        assert "confirmation needs a terminal" in refused.stderr
        imported = run(binary, "--db", str(database), "--import", str(incoming), "--yes")
        assert "Import preview: 2 transactions" in imported.stdout
        assert "Imported 2 transactions" in imported.stdout

        run(binary, "--db", str(database), "--export", str(exported))
        assert exported.read_text(encoding="ascii").startswith("kind,amount,category,note,date\n")
        run(binary, "--db", str(database), "--backup", str(backup))
        assert backup.is_file()
        run(binary, "--db", str(database), "--import", str(second), "--yes")
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT COUNT(*) FROM transactions").fetchone()[0] == 3

        same_file = directory / "same-ledger.db"
        os.link(database, same_file)
        rejected = run(binary, "--db", str(database), "--restore", str(same_file),
                       "--yes", expected=1)
        assert "source and destination must be different" in rejected.stderr
        same_file.unlink()

        restored = run(binary, "--db", str(database), "--restore", str(backup), "--yes")
        assert "Ledger restored" in restored.stdout
        assert list(directory.glob("ledger.db.before-restore-*"))
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT COUNT(*) FROM transactions").fetchone()[0] == 2
            assert connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"
        assert "Ledger check: OK" in run(binary, "--db", str(database), "--check").stdout

    print("ok - CLI data commands")


if __name__ == "__main__":
    main()
