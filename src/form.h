#pragma once
// A small dialog toolkit built on input.h: a centered bordered window, and a
// Form that lays out several fields (single- or multi-line) and lets the
// user move between them with Tab/Shift+Tab/Up/Down without losing what's
// already typed, matching how every "New X" / "Edit X" dialog in the app
// behaves.

#include <ncurses.h>

#include <string>
#include <vector>

struct FormField {
  std::string label;
  std::string value;
  bool multiline = false;
  int height = 1;  // rows reserved for a multiline field
};

enum class FormResult { Saved, Cancelled };

// Opens a centered bordered popup, clamped to fit the terminal. Returns the
// window; the caller should getmaxyx() it afterwards since the requested
// size may have been shrunk.
WINDOW* openDialog(int h, int w, const std::string& title);

// Runs the field list interactively inside `win`, starting each field
// prepopulated with its current value and the cursor at the end (ready to
// append), and returns Saved (Enter through the last field, or Shift+Enter
// from anywhere) or Cancelled (Escape from anywhere).
FormResult runForm(WINDOW* win, std::vector<FormField>& fields, int startY, int labelX, int valueX, int valueW);

// A single yes/no prompt; Escape or 'n' cancel, 'y' confirms.
bool confirmDialog(const std::string& message);
