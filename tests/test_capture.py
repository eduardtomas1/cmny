#!/usr/bin/env python3
"""Regression checks for streamed terminal screenshot capture."""

from __future__ import annotations

import html
import re
import tempfile
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from capture_demo import Terminal, capture, render_svg  # noqa: E402


def rendered_rows(document: str) -> list[str]:
    rows: dict[float, list[tuple[float, str]]] = {}
    pattern = r'<text x="([0-9.]+)" y="([0-9.]+)"[^>]*>(.*?)</text>'
    for x, y, character in re.findall(pattern, document):
        if "cmny --demo" in character:
            continue
        rows.setdefault(float(y), []).append((float(x), html.unescape(character)))
    return ["".join(character for _, character in sorted(row))
            for _, row in sorted(rows.items())]


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

    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build" / "cmny").resolve()
    with tempfile.TemporaryDirectory(prefix="cmny-settings-capture-") as temporary:
        settings = Path(temporary) / "settings.svg"
        narrow = Path(temporary) / "narrow.svg"
        capture(binary, "settings", "amber", 24, 90, settings)
        document = settings.read_text(encoding="utf-8")
        capture(binary, "overview", "ocean", 14, 40, narrow)
        narrow_document = narrow.read_text(encoding="utf-8")
    screen = "\n".join(rendered_rows(document))
    assert "[4]Settings" in screen
    assert "Demo settings and entries disappear when you quit.".replace(" ", "") in screen
    assert "ResetkeybindingsEnter" in screen
    assert "Escback" in screen
    narrow_screen = "\n".join(rendered_rows(narrow_document))
    assert "1Home2List3Stats4Setup" in narrow_screen
    assert "Income" in narrow_screen and "Saved" in narrow_screen
    print("ok - screenshot capture")


if __name__ == "__main__":
    main()
