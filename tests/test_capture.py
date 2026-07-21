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
    with tempfile.TemporaryDirectory(prefix="cmny-manage-capture-") as temporary:
        manage = Path(temporary) / "manage.svg"
        activity = Path(temporary) / "activity.svg"
        narrow = Path(temporary) / "narrow.svg"
        medium = Path(temporary) / "medium.svg"
        wide = Path(temporary) / "wide.svg"
        capture(binary, "manage", "amber", 24, 90, manage)
        document = manage.read_text(encoding="utf-8")
        capture(binary, "activity", "ocean", 24, 90, activity)
        activity_document = activity.read_text(encoding="utf-8")
        capture(binary, "overview", "ocean", 14, 40, narrow)
        narrow_document = narrow.read_text(encoding="utf-8")
        capture(binary, "overview", "high-contrast", 24, 80, medium)
        medium_document = medium.read_text(encoding="utf-8")
        capture(binary, "overview", "monochrome", 35, 120, wide)
        wide_document = wide.read_text(encoding="utf-8")
    screen = "\n".join(rendered_rows(document))
    assert "5Manage" in screen
    assert "Demo settings and entries disappear when you quit.".replace(" ", "") in screen
    assert "Newtransfer[v]" in screen
    assert "Filterbyaccount[g]" in screen
    assert "Manageaccounts[m]" in screen
    assert "Escback" in screen
    assert "GETTINGSTARTED" not in screen
    activity_screen = "\n".join(rendered_rows(activity_document))
    assert "type:allaccount:all" in activity_screen
    assert "DATEACCOUNTCATEGORYPAYEE/DETAILSAMOUNT" in activity_screen
    assert "[]2026-07-20CashFoodCoffeeandlunch-23.80" in activity_screen
    narrow_screen = "\n".join(rendered_rows(narrow_document))
    assert "[1/5]Overview" in narrow_screen and "Tabscreens" in narrow_screen
    assert "Income" in narrow_screen and "Saved" in narrow_screen
    medium_screen = "\n".join(rendered_rows(medium_document))
    wide_screen = "\n".join(rendered_rows(wide_document))
    assert "1Home" in medium_screen and "5Manage" in medium_screen
    assert "[1]Overview" in wide_screen and "[5]Manage" in wide_screen
    assert "GETTINGSTARTED" not in medium_screen + wide_screen
    assert "#22c55e" in medium_document
    assert "#22c55e" not in wide_document
    print("ok - screenshot capture")


if __name__ == "__main__":
    main()
