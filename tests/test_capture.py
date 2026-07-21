#!/usr/bin/env python3
"""Regression checks for streamed terminal screenshot capture."""

from __future__ import annotations

import tempfile
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from capture_demo import Terminal, render_svg  # noqa: E402


def main() -> None:
    terminal = Terminal(4, 12)
    terminal.feed(b"\x1b[2;")
    terminal.feed(b"3HAB")
    terminal.feed(b"\x1b")
    terminal.feed(b"[3;4HC")
    assert terminal.cells[1][2].char == "A"
    assert terminal.cells[1][3].char == "B"
    assert terminal.cells[2][3].char == "C"
    assert terminal.pending == ""

    with tempfile.TemporaryDirectory(prefix="cmny-capture-") as temporary:
        output = Path(temporary) / "capture.svg"
        render_svg(terminal, output, "stream test")
        document = output.read_text(encoding="utf-8")
    assert "2;3H" not in document and "3;4H" not in document
    assert document.count(">A</text>") == 1
    print("ok - screenshot capture")


if __name__ == "__main__":
    main()
