#pragma once
// Low-level ncurses input: a key reader that never lets a raw escape
// sequence leak into a text buffer, plus the single-line and multi-line
// field editors built on top of it.

#include <ncurses.h>
#include <string>

enum class Key { Char, Left, Right, Up, Down, Home, End, Backspace, Delete, Enter, ShiftEnter, Tab, ShiftTab, Escape, None };

struct KeyEvent {
  Key type = Key::None;
  char ch = 0;
};

// Reads one logical keypress from `win` (which must have keypad(win, TRUE)
// set). Absorbs multi-byte escape sequences itself -- arrow/Home/End/Delete,
// plain Escape, and CSI-u encoded Shift/Ctrl+Enter on terminals that support
// the Kitty keyboard protocol or xterm's modifyOtherKeys -- so unrecognized
// bytes are discarded instead of leaking into whatever the caller is doing.
KeyEvent readKey(WINDOW* win);

enum class FieldOutcome { Next, Prev, Saved, Cancelled };

void clearRect(WINDOW* win, int y, int x, int width, int height = 1);

// Single-line field editor. Starts with the cursor after the existing text
// (ready to append) and supports Left/Right/Home/End/Backspace/Delete,
// horizontally scrolling if the text runs past `width`.
FieldOutcome editLine(WINDOW* win, int y, int x, int width, std::string& text);

// Multi-line field editor (task/project descriptions, checklist items).
// Enter inserts a newline; Tab/Shift+Tab or Up/Down at the first/last line
// move focus to the next/previous field. Renders word-wrapped, scrolling
// vertically to keep the cursor's line in view.
FieldOutcome editArea(WINDOW* win, int y, int x, int width, int height, std::string& text);
