#pragma once
// Standalone, minimal capture dialog for `stride --quick-capture`.
//
// Meant to be bound to a global hotkey (or a bar widget) that opens a small
// *floating terminal* running just this -- so it works even when the main
// Stride window isn't open anywhere. It never touches App: it initializes
// just enough ncurses for one three-field dialog, saves (or cancels) a
// single task, and returns immediately so the floating terminal closes
// behind it. See the "Quick capture" section of README.md for how to wire
// a compositor keybinding + window rule up to it.
#include "store.h"

// Runs the dialog against `s` and returns a process exit code: 0 if a task
// was saved, 1 if the dialog was cancelled or left titleless.
int runQuickCapture(Store& s);
