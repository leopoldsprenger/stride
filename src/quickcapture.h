#pragma once
// Standalone, minimal capture dialog for `stride --quick-capture`.
//
// Meant to be bound to a global hotkey (or a bar widget) so it works even
// when the main Stride window isn't open anywhere. It never touches App.
//
// Two front ends share the same save path:
//
//   * GTK4 window (default when Stride was built with GTK4 and a Wayland/X11
//     display is reachable). It's a small, borderless, non-resizable window
//     sized to its own content, so a compositor can float it with a single
//     window rule -- no dedicated terminal needed. It closes on save, on
//     Escape, and when it loses focus. See quickcapture_gtk.cpp.
//   * ncurses dialog (fallback: no GTK4 at build time, no display, or
//     `--tui`). Meant to run inside a small floating terminal.
//
// See the "Quick capture" section of README.md for compositor wiring.
#include <string>

#include "store.h"

// Runs the dialog against `s` and returns a process exit code: 0 if a task
// was saved, 1 if the dialog was cancelled or left titleless. Uses the GTK4
// window when available unless `forceTui` is set, else the ncurses dialog.
int runQuickCapture(Store& s, bool forceTui = false);

// The ncurses front end on its own (what runQuickCapture falls back to).
int runQuickCaptureTui(Store& s);

// Shared by both front ends: resolves `list` to an existing area/project
// name (blank or unknown = Inbox) and saves the task. Returns 0 if saved,
// 1 if `title` is blank (nothing saved).
int saveQuickCapture(Store& s, const std::string& title, const std::string& list, const std::string& doDate);

#ifdef STRIDE_HAVE_GTK4
// GTK4 front end. Returns false if GTK could not be initialised (no display),
// in which case nothing was shown and `exitCode` is untouched; otherwise runs
// the window to completion, stores the exit code (as for runQuickCapture) and
// returns true.
bool runQuickCaptureGtk(Store& s, int& exitCode);
#endif
