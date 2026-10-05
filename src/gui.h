#pragma once
// Full GTK4 front end for Stride: `stride --gui` (or a bare `stride` when STRIDE_INTERFACE=gui, which is what the
// home-manager module's `programs.stride.interface = "gui"` arranges). The TUI is untouched and stays the default.
//
// Built only when GTK4 is available (STRIDE_HAVE_GTK4); otherwise runGui() explains that and returns nonzero.
#include <cstdio>

#include "store.h"

// Returned when the GUI can't start (built without GTK4, or no display); the caller falls back to the TUI.
constexpr int kGuiUnavailable = 125;


#ifndef STRIDE_HAVE_GTK4
inline int runGui(Store&) {
  std::fputs("stride: this build has no GTK4 support, so --gui is unavailable\n", stderr);
  return kGuiUnavailable;
}
#else
int runGui(Store& store);
#endif
