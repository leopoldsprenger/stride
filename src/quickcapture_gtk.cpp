// GTK4 front end for `stride --quick-capture`.
//
// The same card the main window opens for a new to-do, on its own: a rounded square to tick, a title, and two quiet
// fields (List, When). It takes its colours from the same place the main window does -- the active GTK theme -- and
// shares its stylesheet, so the two always look like one app.
//
// Keys are the ones the ncurses dialog has always had:
//
//   * Enter on Title or List moves to the next field (like Tab); Enter on When (the last field) saves.
//   * Shift+Enter or Ctrl+Enter saves immediately from any field.
//   * Escape cancels from any field.
//   * Losing window focus (click elsewhere, Alt-Tab away) cancels too -- this is what lets a compositor's window rule
//     replace a whole dedicated terminal with a small popup that gets out of the way the instant you look away.
//
// What you type is understood as you type: `@list`, `#tag` and `!when` work inside the title exactly as they do in the
// main window, dates take `today`, `fri`, `+3d`, `oct 12`..., and a line under the fields says where the to-do will
// land. A date it can't read stops the save instead of being stored as garbage.
//
// The window is undecorated and left unsized so it shrinks to fit the card; the compositor floats and centres it via a
// window rule matched on the Wayland app_id set below (see the "Quick capture" section of README.md). The card sits in
// a margin of transparent pixels that carries its shadow, and a click in that margin cancels.
#include "quickcapture.h"

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <string>
#include <utility>

#include "gui_dates.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "util.h"

namespace {

// Wayland compositors (and X11 WMs, via WM_CLASS) match window rules against this -- the mangowm rule and bind in
// README.md's "Quick capture" section use exactly this string. Keep the two in sync if you ever rename it.
constexpr const char* kAppId = "stride-quick-capture";

struct Target {
  bool found = false, inbox = false;
  int areaId = 0, projectId = 0;
  std::string label;
};

// Exact name first, then a unique substring ("reno" -> "Home Renovation"); blank means the Inbox.
Target resolveList(Store& s, const std::string& raw) {
  Target t;
  std::string q = lower(trimmed(raw));
  if (q.empty() || q == "inbox" || q == "none") {
    t.found = t.inbox = true;
    t.label = "Inbox";
    return t;
  }
  auto projects = s.projects();
  auto areas = s.areas();
  for (auto& p : projects)
    if (lower(p.name) == q) return Target{true, false, s.projectAreaId(p.id), p.id, p.name};
  for (auto& a : areas)
    if (lower(a.name) == q) return Target{true, false, a.id, 0, a.name};
  std::vector<Target> hits;
  for (auto& p : projects)
    if (lower(p.name).find(q) != std::string::npos) hits.push_back({true, false, s.projectAreaId(p.id), p.id, p.name});
  for (auto& a : areas)
    if (lower(a.name).find(q) != std::string::npos) hits.push_back({true, false, a.id, 0, a.name});
  if (hits.size() == 1) return hits[0];
  return t;
}

struct QCState {
  Store* store = nullptr;
  GtkWidget *window = nullptr, *card = nullptr;
  GtkWidget *entryTitle = nullptr, *entryList = nullptr, *entryDate = nullptr;
  GtkWidget *listField = nullptr, *dateField = nullptr, *hint = nullptr;
  GMainLoop* loop = nullptr;
  bool decided = false;  // guards against finishing twice (Escape, then the destroy signal it triggers)
  bool saved = false;
  bool activeOnce = false;
  // what the live preview last understood; save() uses it
  Target target;
  guidates::Parsed when;
  guidates::Sigils sigils;
  bool valid = false;
};

// Ends the dialog exactly once: records the outcome and stops the loop runQuickCaptureGtk() is blocked in.
void finish(QCState* st, bool saved) {
  if (st->decided) return;
  st->decided = true;
  st->saved = saved;
  g_main_loop_quit(st->loop);
}

std::string entryText(GtkWidget* e) { return gtk_editable_get_text(GTK_EDITABLE(e)); }

void setClass(GtkWidget* w, const char* c, bool on) {
  if (on) gtk_widget_add_css_class(w, c);
  else gtk_widget_remove_css_class(w, c);
}

// Re-reads the three fields and says, under them, what will happen.
void refresh(QCState* st) {
  st->sigils = guidates::parseSigils(entryText(st->entryTitle));
  std::string whenText = st->sigils.hasWhen ? st->sigils.when : entryText(st->entryDate);
  std::string listText = !st->sigils.list.empty() ? st->sigils.list : entryText(st->entryList);
  st->when = guidates::parse(whenText, true);
  st->target = resolveList(*st->store, listText);

  bool listMissing = !st->target.found;
  setClass(st->dateField, "invalid", !st->when.ok);
  setClass(st->listField, "invalid", listMissing);

  std::string msg;
  bool err = false;
  if (!st->when.ok) {
    msg = "Couldn't read “" + trimmed(whenText) + "” as a date";
    err = true;
  } else if (listMissing) {
    msg = "No list called “" + trimmed(listText) + "” — it will go to the Inbox";
    err = true;
  } else {
    msg = "→ " + st->target.label;
    if (!st->when.value.empty()) msg += "  ·  " + st->when.label;
    for (auto& t : splitComma(st->sigils.tags)) msg += "  ·  #" + t;
  }
  gtk_label_set_text(GTK_LABEL(st->hint), msg.c_str());
  setClass(st->hint, "error", err);
  st->valid = st->when.ok;
}

void save(QCState* st) {
  refresh(st);
  if (!st->valid) return;  // an unreadable date stays on screen, with the reason, instead of being saved wrong
  finish(st, true);
}

gboolean onEscape(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
  auto* st = static_cast<QCState*>(data);
  if (keyval == GDK_KEY_Escape) {
    finish(st, false);
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

// Attached per entry (capture phase, so it runs before GtkText's own Enter handling), since "is this the last field"
// and "what's next" differ per entry.
gboolean onEntryKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer data) {
  auto* pair = static_cast<std::pair<QCState*, GtkWidget*>*>(data);
  QCState* st = pair->first;
  GtkWidget* nextFocus = pair->second;  // nullptr for the last field
  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter) return GDK_EVENT_PROPAGATE;
  bool quick = (state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) != 0;
  if (quick || !nextFocus) save(st);
  else gtk_widget_grab_focus(nextFocus);
  return GDK_EVENT_STOP;
}

void onActiveChanged(GObject* window, GParamSpec*, gpointer data) {
  auto* st = static_cast<QCState*>(data);
  if (gtk_window_is_active(GTK_WINDOW(window))) st->activeOnce = true;
  else if (st->activeOnce) finish(st, false);
}

void onDestroy(GtkWidget*, gpointer data) {
  // Fallback for any close path that isn't Escape/save/focus-loss (e.g. a compositor keybind that kills the window
  // outright); finish() is a no-op if one of those already ran.
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

// A captioned field: the same little box the main editor uses.
GtkWidget* field(const char* cap, const char* placeholder, GtkWidget** entryOut, GtkWidget** boxOut) {
  GtkWidget* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_hexpand(col, TRUE);
  GtkWidget* c = gtk_label_new(cap);
  gtk_widget_add_css_class(c, "field-cap");
  gtk_label_set_xalign(GTK_LABEL(c), 0);
  gtk_box_append(GTK_BOX(col), c);
  GtkWidget* f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(f, "field");
  GtkWidget* e = gtk_entry_new();
  gtk_widget_add_css_class(e, "bare");
  gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
  gtk_widget_set_hexpand(e, TRUE);
  gtk_box_append(GTK_BOX(f), e);
  gtk_box_append(GTK_BOX(col), f);
  *entryOut = e;
  *boxOut = f;
  return col;
}

GtkWidget* keyHint(const char* key, const char* what) {
  GtkWidget* b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget* k = gtk_label_new(key);
  gtk_widget_add_css_class(k, "keycap");
  GtkWidget* l = gtk_label_new(what);
  gtk_widget_add_css_class(l, "btn-note");
  gtk_box_append(GTK_BOX(b), k);
  gtk_box_append(GTK_BOX(b), l);
  return b;
}

// What is specific to this window; everything else comes from the shared stylesheet (buildCss).
const char* kQcCss = R"CSS(
window.quickcapture { background: transparent; }
.qc-outer { padding: 30px 34px 40px 34px; }
.qc-card { background-color: {{card}}; border-radius: 16px; padding: 16px 20px 14px 18px;
           box-shadow: 0 18px 50px rgba(0,0,0,0.38), 0 0 0 1px {{line}}; animation: qc-in 180ms cubic-bezier(.2,.9,.25,1); }
@keyframes qc-in { from { opacity: 0; transform: translateY(-8px) scale(0.98); } to { opacity: 1; transform: none; } }
.qc-title { font-size: 18px; font-weight: 500; }
.qc-hint { font-size: 12.5px; color: {{fg2}}; }
.qc-hint.error { color: {{red}}; }
)CSS";

}  // namespace

bool runQuickCaptureGtk(Store& s, int& exitCode) {
  // Sets WM_CLASS on X11 (and is gdk_wayland_toplevel_set_application_id()'s fallback below, if that ever can't run)
  // to the same string a compositor window rule matches on -- see kAppId's comment.
  g_set_prgname(kAppId);
  if (!gtk_init_check()) return false;  // no display reachable

  gPal = resolvePalette();
  std::string css = buildCss(gPal) + replaceAll(replaceAll(replaceAll(kQcCss, "{{card}}", cssColor(gPal.card)), "{{line}}", cssColor(gPal.line)),
                                                  "{{fg2}}", cssColor(gPal.fg2));
  css = replaceAll(css, "{{red}}", cssColor(gPal.red));
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, css.c_str());
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
  g_object_unref(provider);

  QCState st;
  st.store = &s;
  st.loop = g_main_loop_new(nullptr, FALSE);

  GtkWidget* window = gtk_window_new();
  st.window = window;
  gtk_widget_add_css_class(window, "quickcapture");
  gtk_window_set_title(GTK_WINDOW(window), "Quick Capture");
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
  // No default size: the window sizes itself to the card, which is the whole point -- no full terminal-sized floats.

  GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(outer, "qc-outer");
  gtk_window_set_child(GTK_WINDOW(window), outer);

  GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(card, "qc-card");
  gtk_widget_set_size_request(card, 540, -1);
  st.card = card;
  gtk_box_append(GTK_BOX(outer), card);

  // title row: the to-do's own box, then the title
  GtkWidget* titleRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget* box = gtk_drawing_area_new();
  gtk_widget_set_size_request(box, 26, 26);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(box), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    draw::checkbox(cr, w, h, draw::checkPose(false, 0, false, true), false, draw::Shape::Square);
  }, nullptr, nullptr);
  gtk_box_append(GTK_BOX(titleRow), box);
  st.entryTitle = gtk_entry_new();
  gtk_widget_add_css_class(st.entryTitle, "bare");
  gtk_widget_add_css_class(st.entryTitle, "qc-title");
  gtk_entry_set_placeholder_text(GTK_ENTRY(st.entryTitle), "New to-do   (add @list #tag !when inline)");
  gtk_widget_set_hexpand(st.entryTitle, TRUE);
  gtk_box_append(GTK_BOX(titleRow), st.entryTitle);
  gtk_box_append(GTK_BOX(card), titleRow);

  GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(body, 38);
  GtkWidget* grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(grid), field("LIST", "Inbox", &st.entryList, &st.listField));
  gtk_box_append(GTK_BOX(grid), field("WHEN", "today, fri, +3d, someday", &st.entryDate, &st.dateField));
  gtk_box_append(GTK_BOX(body), grid);

  GtkWidget* foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  st.hint = gtk_label_new("");
  gtk_widget_add_css_class(st.hint, "qc-hint");
  gtk_label_set_xalign(GTK_LABEL(st.hint), 0);
  gtk_label_set_ellipsize(GTK_LABEL(st.hint), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(st.hint, TRUE);
  gtk_box_append(GTK_BOX(foot), st.hint);
  gtk_box_append(GTK_BOX(foot), keyHint("↵", "next"));
  gtk_box_append(GTK_BOX(foot), keyHint("⇧↵", "save"));
  gtk_box_append(GTK_BOX(foot), keyHint("Esc", "cancel"));
  gtk_box_append(GTK_BOX(body), foot);
  gtk_box_append(GTK_BOX(card), body);

  for (GtkWidget* e : {st.entryTitle, st.entryList, st.entryDate})
    g_signal_connect_swapped(e, "changed", G_CALLBACK(+[](gpointer p) { refresh(static_cast<QCState*>(p)); }), &st);

  // Escape cancels from anywhere: one controller on the window, capture phase so it runs ahead of the focused entry.
  GtkEventController* escCtl = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(escCtl, GTK_PHASE_CAPTURE);
  g_signal_connect(escCtl, "key-pressed", G_CALLBACK(onEscape), &st);
  gtk_widget_add_controller(window, escCtl);

  // Enter / Shift+Enter per field. Leaked intentionally: they outlive the widgets they're attached to and this whole
  // function exits once.
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

  // A click on the card is the card's; a click in the transparent margin around it cancels.
  GtkGesture* inside = gtk_gesture_click_new();
  g_signal_connect(inside, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double, double, gpointer) {
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
  }), nullptr);
  gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(inside));
  GtkGesture* outside = gtk_gesture_click_new();
  g_signal_connect(outside, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer p) {
    finish(static_cast<QCState*>(p), false);
  }), &st);
  gtk_widget_add_controller(outer, GTK_EVENT_CONTROLLER(outside));

  g_signal_connect(window, "notify::is-active", G_CALLBACK(onActiveChanged), &st);
  g_signal_connect(window, "destroy", G_CALLBACK(onDestroy), &st);
  g_signal_connect(window, "realize", G_CALLBACK(onRealize), nullptr);

  refresh(&st);
  gtk_window_present(GTK_WINDOW(window));
  gtk_widget_grab_focus(st.entryTitle);

  g_main_loop_run(st.loop);
  g_main_loop_unref(st.loop);
  delete pairTitle;
  delete pairList;
  delete pairDate;

  std::string title = trimmed(st.sigils.title);
  std::string tags = st.sigils.tags;

  if (GTK_IS_WINDOW(window)) gtk_window_destroy(GTK_WINDOW(window));
  while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);

  if (!st.saved || title.empty()) {
    exitCode = 1;
    return true;
  }
  Item t;
  t.title = title;
  t.tags = tags;
  t.doDate = st.when.value;  // "" | "someday" | ISO -- Store::saveTask turns the literal "someday" into the flag
  s.saveTask(t, st.target.areaId, st.target.projectId);
  exitCode = 0;
  return true;
}
