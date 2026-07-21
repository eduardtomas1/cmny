#!/usr/bin/env python3
"""Capture the real curses demo into a deterministic, dependency-free SVG."""

from __future__ import annotations

import argparse
import fcntl
import html
import os
import pty
import select
import struct
import subprocess
import termios
import time
from dataclasses import dataclass
from pathlib import Path


PALETTE = {
    None: "#cbd5e1",
    0: "#111827",
    1: "#ef4444",
    2: "#22c55e",
    3: "#f59e0b",
    4: "#3b82f6",
    5: "#a855f7",
    6: "#22d3ee",
    7: "#e5e7eb",
}
DEFAULT_BG = "#0b1020"


@dataclass
class Style:
    fg: int | None = None
    bg: int | None = None
    bold: bool = False
    dim: bool = False
    reverse: bool = False

    def copy(self) -> "Style":
        return Style(self.fg, self.bg, self.bold, self.dim, self.reverse)


@dataclass
class Cell:
    char: str = " "
    style: Style | None = None


class Terminal:
    def __init__(self, rows: int, columns: int) -> None:
        self.rows = rows
        self.columns = columns
        self.cells = [[Cell(style=Style()) for _ in range(columns)] for _ in range(rows)]
        self.row = 0
        self.column = 0
        self.saved = (0, 0)
        self.style = Style()
        self.last_char = " "
        self.pending = ""

    def clear(self) -> None:
        self.cells = [[Cell(style=Style()) for _ in range(self.columns)] for _ in range(self.rows)]
        self.row = 0
        self.column = 0

    def put(self, char: str) -> None:
        if 0 <= self.row < self.rows and 0 <= self.column < self.columns:
            self.cells[self.row][self.column] = Cell(char, self.style.copy())
        self.last_char = char
        self.column += 1
        if self.column >= self.columns:
            self.column = self.columns - 1

    @staticmethod
    def params(raw: str) -> list[int]:
        raw = raw.lstrip("?")
        if raw == "":
            return [0]
        result = []
        for part in raw.split(";"):
            try:
                result.append(int(part) if part else 0)
            except ValueError:
                result.append(0)
        return result

    def csi(self, raw: str, final: str) -> None:
        values = self.params(raw)
        first = values[0] if values else 0
        count = first or 1
        if final in ("H", "f"):
            self.row = max(0, min(self.rows - 1, (values[0] if values else 1 or 1) - 1))
            col = values[1] if len(values) > 1 else 1
            self.column = max(0, min(self.columns - 1, (col or 1) - 1))
        elif final == "A":
            self.row = max(0, self.row - count)
        elif final == "B":
            self.row = min(self.rows - 1, self.row + count)
        elif final == "C":
            self.column = min(self.columns - 1, self.column + count)
        elif final == "D":
            self.column = max(0, self.column - count)
        elif final == "G":
            self.column = max(0, min(self.columns - 1, count - 1))
        elif final == "d":
            self.row = max(0, min(self.rows - 1, count - 1))
        elif final == "J":
            if first in (0, 2, 3):
                self.clear()
        elif final == "K":
            if first == 2:
                start, end = 0, self.columns
            elif first == 1:
                start, end = 0, self.column + 1
            else:
                start, end = self.column, self.columns
            for column in range(start, end):
                self.cells[self.row][column] = Cell(" ", self.style.copy())
        elif final == "X":
            for column in range(self.column, min(self.columns, self.column + count)):
                self.cells[self.row][column] = Cell(" ", self.style.copy())
        elif final == "b":
            for _ in range(count):
                self.put(self.last_char)
        elif final == "s":
            self.saved = (self.row, self.column)
        elif final == "u":
            self.row, self.column = self.saved
        elif final == "m":
            self.sgr(values)

    def sgr(self, values: list[int]) -> None:
        if not values:
            values = [0]
        for value in values:
            if value == 0:
                self.style = Style()
            elif value == 1:
                self.style.bold = True
            elif value == 2:
                self.style.dim = True
            elif value == 7:
                self.style.reverse = True
            elif value == 22:
                self.style.bold = self.style.dim = False
            elif value == 27:
                self.style.reverse = False
            elif 30 <= value <= 37:
                self.style.fg = value - 30
            elif value == 39:
                self.style.fg = None
            elif 40 <= value <= 47:
                self.style.bg = value - 40
            elif value == 49:
                self.style.bg = None

    def feed(self, data: bytes) -> None:
        text = self.pending + data.decode("utf-8", errors="ignore")
        self.pending = ""
        index = 0
        while index < len(text):
            char = text[index]
            if char == "\x1b":
                if index + 1 >= len(text):
                    self.pending = text[index:]
                    break
                if index + 1 < len(text) and text[index + 1] == "[":
                    end = index + 2
                    while end < len(text) and not ("@" <= text[end] <= "~"):
                        end += 1
                    if end < len(text):
                        self.csi(text[index + 2 : end], text[end])
                        index = end + 1
                        continue
                    self.pending = text[index:]
                    break
                if index + 1 < len(text) and text[index + 1] in "78":
                    if text[index + 1] == "7":
                        self.saved = (self.row, self.column)
                    else:
                        self.row, self.column = self.saved
                    index += 2
                    continue
                if index + 2 < len(text) and text[index + 1] in "()" :
                    index += 3
                    continue
                if text[index + 1] in "()":
                    self.pending = text[index:]
                    break
                index += 2
                continue
            if char == "\r":
                self.column = 0
            elif char == "\n":
                self.row = min(self.rows - 1, self.row + 1)
            elif char == "\b":
                self.column = max(0, self.column - 1)
            elif char == "\t":
                self.column = min(self.columns - 1, (self.column // 8 + 1) * 8)
            elif char >= " ":
                self.put(char)
            index += 1


def read_quiet(master: int, terminal: Terminal, quiet: float = 0.18, limit: float = 2.0) -> None:
    deadline = time.monotonic() + limit
    last_data = time.monotonic()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.04)
        if ready:
            try:
                data = os.read(master, 65536)
            except OSError:
                return
            if not data:
                return
            terminal.feed(data)
            last_data = time.monotonic()
        elif time.monotonic() - last_data >= quiet:
            return


def colors(style: Style) -> tuple[str, str]:
    foreground = PALETTE.get(style.fg, PALETTE[None])
    background = PALETTE.get(style.bg, DEFAULT_BG) if style.bg is not None else DEFAULT_BG
    if style.reverse:
        foreground, background = background, foreground
    return foreground, background


def render_svg(terminal: Terminal, output: Path, label: str) -> None:
    cell_width = 8.45
    line_height = 18.4
    padding = 20
    width = terminal.columns * cell_width + padding * 2
    height = terminal.rows * line_height + padding * 2 + 28
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}" viewBox="0 0 {width:.1f} {height:.1f}">',
        f'<rect width="100%" height="100%" rx="12" fill="{DEFAULT_BG}"/>',
        '<circle cx="20" cy="17" r="4" fill="#ff5f57"/><circle cx="34" cy="17" r="4" fill="#febc2e"/><circle cx="48" cy="17" r="4" fill="#28c840"/>',
        f'<text x="{width / 2:.1f}" y="20" text-anchor="middle" font-family="ui-monospace, SFMono-Regular, Menlo, monospace" font-size="11" fill="#64748b">{html.escape(label)}</text>',
    ]
    origin_y = padding + 28
    for row_index, row in enumerate(terminal.cells):
        for col, cell in enumerate(row):
            style = cell.style or Style()
            _, background = colors(style)
            if background != DEFAULT_BG:
                parts.append(
                    f'<rect x="{padding + col * cell_width:.2f}" y="{origin_y + row_index * line_height - 13.8:.2f}" '
                    f'width="{cell_width + 0.2:.2f}" height="{line_height:.2f}" fill="{background}"/>'
                )
        for col, cell in enumerate(row):
            if cell.char == " ":
                continue
            style = cell.style or Style()
            foreground, _ = colors(style)
            weight = "700" if style.bold else "400"
            opacity = "0.55" if style.dim else "1"
            parts.append(
                f'<text x="{padding + col * cell_width:.2f}" y="{origin_y + row_index * line_height:.2f}" '
                f'font-family="Menlo, monospace" font-size="14" '
                f'font-variant-ligatures="none" '
                f'font-weight="{weight}" opacity="{opacity}" '
                f'fill="{foreground}">{html.escape(cell.char)}</text>'
            )
    parts.append("</svg>\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(parts), encoding="utf-8")


def capture(binary: Path, screen: str, theme: str, rows: int, columns: int, output: Path) -> None:
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    environment = os.environ.copy()
    environment.pop("NO_COLOR", None)
    environment.update({"TERM": "xterm-256color", "LC_ALL": "C.UTF-8"})
    process = subprocess.Popen(
        [str(binary), "--demo", "--ascii", "--theme", theme],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=environment,
        close_fds=True,
    )
    os.close(slave)
    terminal = Terminal(rows, columns)
    read_quiet(master, terminal)
    key = {"overview": b"1", "activity": b"2", "reports": b"3"}[screen]
    os.write(master, key)
    read_quiet(master, terminal)
    render_svg(terminal, output, f"cmny --demo  |  {columns}x{rows}  |  {screen}  |  {theme}")
    os.write(master, b"q")
    try:
        read_quiet(master, Terminal(rows, columns), quiet=0.08, limit=1.0)
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=2)
    finally:
        os.close(master)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/cmny"))
    parser.add_argument("--screen", choices=("overview", "activity", "reports"), required=True)
    parser.add_argument("--theme", choices=("ocean", "violet", "amber"), default="ocean")
    parser.add_argument("--rows", type=int, default=34)
    parser.add_argument("--columns", type=int, default=110)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    capture(args.binary.resolve(), args.screen, args.theme, args.rows, args.columns, args.output)


if __name__ == "__main__":
    main()
