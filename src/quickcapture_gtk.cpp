// GTK4 front end for `stride --quick-capture`.
//
// Same three fields, same keys, same save/cancel semantics as the ncurses
// dialog in quickcapture.cpp (see runQuickCaptureTui) -- this is a drop-in
// visual replacement, not a different tool:
//
//   * Enter on Title or List moves focus to the next field (like Tab).
//   * Enter on Do date (the last field) saves, same as running off the end
//     of the ncurses form.
//   * Shift+Enter saves immediately from any field.
//   * Escape cancels from any field.
//   * Tab / Shift+Tab move focus forward/back (GTK's default focus chain
//     does this for free -- no code needed).
//   * Losing window focus entirely (click elsewhere, Alt-Tab away) cancels
//     too, same as Escape -- this is what lets a compositor's window rule
//     replace a whole dedicated terminal with a small popup that gets out
//     of the way the instant you look away from it.
//
// The window is undecorated and left unsized (no gtk_window_set_default_size)
// so it always shrinks to fit exactly its three fields -- the compositor
// floats and centers it via a window rule matched on the Wayland app_id set
// below, sized however the compositor's rule wants (or auto, if the rule
// leaves size alone); see the "Quick capture" section of README.md.
#include "quickcapture.h"

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <string>
#include <utility>

#include "util.h"

namespace {

// Wayland compositors (and X11 WMs, via WM_CLASS) match window rules against
// this -- the mangowm rule and bind in README.md's "Quick capture" section
// use exactly this string. Keep the two in sync if you ever rename it.
constexpr const char* kAppId = "stride-quick-capture";

struct QCState {
  GtkWidget* entryTitle = nullptr;
  GtkWidget* entryList = nullptr;
  GtkWidget* entryDate = nullptr;
  GMainLoop* loop = nullptr;
  bool decided = false;  // guards against finishing twice (e.g. Escape then
                          // the destroy signal it triggers)
  bool saved = false;
};

// Ends the dialog exactly once: records the outcome and stops the loop that
// runQuickCaptureGtk() is blocked in. Safe to call from multiple signal
// handlers (Escape, Enter-on-last-field, focus-out, window-manager close) --
// only the first call has any effect.
void finish(QCState* st, bool saved) {
  if (st->decided) return;
  st->decided = true;
  st->saved = saved;
  g_main_loop_quit(st->loop);
}

gboolean onEscape(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
  auto* st = static_cast<QCState*>(data);
  if (keyval == GDK_KEY_Escape) {
    finish(st, false);
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

// Attached per-entry (capture phase, so it runs before GtkText's own Enter
// handling) rather than once on the window, since "is this the last field"
// and "what's the next field" differ per entry.
gboolean onEntryKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer data) {
  auto* pair = static_cast<std::pair<QCState*, GtkWidget*>*>(data);
  QCState* st = pair->first;
  GtkWidget* nextFocus = pair->second;  // nullptr for the last field
  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter) return GDK_EVENT_PROPAGATE;
  bool shift = (state & GDK_SHIFT_MASK) != 0;
  if (shift || !nextFocus) {
    finish(st, true);
  } else {
    gtk_widget_grab_focus(nextFocus);
  }
  return GDK_EVENT_STOP;
}

void onActiveChanged(GObject* window, GParamSpec*, gpointer data) {
  auto* st = static_cast<QCState*>(data);
  if (!gtk_window_is_active(GTK_WINDOW(window))) finish(st, false);
}

void onDestroy(GtkWidget*, gpointer data) {
  // Fallback for any close path that isn't Escape/save/focus-loss (e.g. a
  // compositor keybind that kills the window outright); finish() is a
  // no-op if one of those already ran.
  finish(static_cast<QCState*>(data), false);
}

void onRealize(GtkWidget* window, gpointer) {
#ifdef GDK_WINDOWING_WAYLAND
  GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
  GdkDisplay* display = gtk_widget_get_display(window);
  if (surface && GDK_IS_WAYLAND_DISPLAY(display) && GDK_IS_WAYLAND_TOPLEVEL(surface)) {
    gdk_wayland_toplevel_set_application_id(GDK_TOPLEVEL(surface), kAppId);
  }
#else
  (void)window;
#endif
}

GtkWidget* makeRow(GtkWidget* grid, int row, const char* label, const std::string& initial) {
  GtkWidget* l = gtk_label_new(label);
  gtk_widget_add_css_class(l, "qc-label");
  gtk_widget_set_halign(l, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);

  GtkWidget* entry = gtk_entry_new();
  gtk_widget_add_css_class(entry, "qc-entry");
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(entry)), initial.c_str(), -1);
  // Cursor at the end, ready to append -- matches editLine()'s starting
  // position in the ncurses dialog.
  gtk_editable_set_position(GTK_EDITABLE(entry), -1);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
  return entry;
}

const char* kCss = R"CSS(
window.quickcapture {
  background-color: #101418;
}
.qc-frame {
  border: 1px solid #3a8fa3;
  border-radius: 0px;
  padding: 10px 14px;
  background-color: #101418;
}
.qc-title {
  color: #56c2d6;
  font-weight: bold;
  font-family: monospace;
  margin-bottom: 6px;
}
.qc-label {
  color: #d8d8d8;
  font-family: monospace;
  margin-right: 10px;
  min-width: 72px;
}
.qc-entry, .qc-entry text {
  font-family: monospace;
  color: #e6e6e6;
  background-color: #1a2027;
  border: none;
  box-shadow: none;
  caret-color: #56c2d6;
}
.qc-entry {
  padding: 2px 4px;
}
.qc-entry:focus, .qc-entry:focus-within {
  outline: none;
  background-color: #1f2732;
}
.qc-hint {
  color: #6f7a85;
  font-family: monospace;
  font-size: 90%;
  margin-top: 8px;
}
)CSS";

}  // namespace

bool runQuickCaptureGtk(Store& s, int& exitCode) {
  // Sets WM_CLASS on X11 (and is gdk_wayland_toplevel_set_application_id()'s
  // fallback below, if that ever can't run) to the same string a compositor
  // window rule matches on -- see kAppId's comment.
  g_set_prgname(kAppId);
  if (!gtk_init_check()) return false;  // no display reachable

  GtkCssProvider* css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css, kCss);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  QCState st;
  st.loop = g_main_loop_new(nullptr, FALSE);

  GtkWidget* window = gtk_window_new();
  gtk_widget_add_css_class(window, "quickcapture");
  gtk_window_set_title(GTK_WINDOW(window), "Quick Capture");
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
  // No default size: the window sizes itself to its content below, which is
  // the whole point -- no more full terminal-sized floats for three fields.

  GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(outer, "qc-frame");
  gtk_window_set_child(GTK_WINDOW(window), outer);

  GtkWidget* title = gtk_label_new("QUICK CAPTURE");
  gtk_widget_add_css_class(title, "qc-title");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_box_append(GTK_BOX(outer), grid);

  st.entryTitle = makeRow(grid, 0, "Title", "");
  st.entryList = makeRow(grid, 1, "List", "");
  st.entryDate = makeRow(grid, 2, "Do date", "");
  gtk_widget_set_size_request(st.entryTitle, 320, -1);

  GtkWidget* hint = gtk_label_new(
      "blank List = Inbox \xc2\xb7 Tab next \xc2\xb7 Enter/\xe2\x87\xa7"
      "Enter save \xc2\xb7 Esc cancel");
  gtk_widget_add_css_class(hint, "qc-hint");
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(outer), hint);

  // Escape cancels from anywhere: one controller on the window, capture
  // phase so it runs ahead of whatever the focused entry would do with it.
  GtkEventController* escCtl = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(escCtl, GTK_PHASE_CAPTURE);
  g_signal_connect(escCtl, "key-pressed", G_CALLBACK(onEscape), &st);
  gtk_widget_add_controller(window, escCtl);

  // Enter/Shift+Enter per field. Leaked intentionally: these outlive the
  // widgets they're attached to and this whole function exits once, so
  // there's nothing meaningful to free them into.
  auto* pairTitle = new std::pair<QCState*, GtkWidget*>(&st, st.entryList);
  auto* pairList = new std::pair<QCState*, GtkWidget*>(&st, st.entryDate);
  auto* pairDate = new std::pair<QCState*, GtkWidget*>(&st, nullptr);
  for (auto p : {std::make_pair(st.entryTitle, pairTitle), std::make_pair(st.entryList, pairList),
                 std::make_pair(st.entryDate, pairDate)}) {
    GtkEventController* ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(ctl, "key-pressed", G_CALLBACK(onEntryKey), p.second);
    gtk_widget_add_controller(p.first, ctl);
  }

  g_signal_connect(window, "notify::is-active", G_CALLBACK(onActiveChanged), &st);
  g_signal_connect(window, "destroy", G_CALLBACK(onDestroy), &st);
  g_signal_connect(window, "realize", G_CALLBACK(onRealize), nullptr);

  gtk_window_present(GTK_WINDOW(window));
  gtk_widget_grab_focus(st.entryTitle);

  g_main_loop_run(st.loop);
  g_main_loop_unref(st.loop);
  delete pairTitle;
  delete pairList;
  delete pairDate;

  std::string title_v = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(st.entryTitle)));
  std::string list_v = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(st.entryList)));
  std::string date_v = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(st.entryDate)));

  if (GTK_IS_WINDOW(window)) gtk_window_destroy(GTK_WINDOW(window));
  while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);

  exitCode = st.saved ? saveQuickCapture(s, title_v, list_v, date_v) : 1;
  return true;
}
