// GTK4 front end for Stride -- `stride --gui`.
//
// Design goals, in priority order:
//   1. Keyboard-first. Every action has a key; the mouse is a convenience, never a requirement.
//   2. Quiet. Few colours, generous space, motion only where it carries meaning (completing, adding, moving).
//   3. Fast. Local SQLite only; sync runs on a worker thread and never blocks a keystroke.
//
// It is a thin view over the same Store the TUI uses -- no data model of its own -- so the TUI, quick capture and
// this window can all be open against one database at once. A cheap PRAGMA data_version poll notices their writes
// and refreshes.
#include "gui.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <climits>
#include <optional>
#include <set>
#include <thread>

#include "config.h"
#include "finder.h"
#include "gui_dates.h"
#include "gui_draw.h"
#include "gui_md.h"
#include "gui_theme.h"
#include "sync.h"
#include "util.h"

namespace {

constexpr const char* kAppId = "io.github.leopoldsprenger.stride";
constexpr size_t kRowLimit = 250;  // widgets per view before "Show more"

// ---------------------------------------------------------------------------------------------------------------
// small widget helpers
// ---------------------------------------------------------------------------------------------------------------

GtkWidget* hbox(int spacing = 0) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing); }
GtkWidget* vbox(int spacing = 0) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing); }
void addClass(GtkWidget* w, const char* c) { gtk_widget_add_css_class(w, c); }
void rmClass(GtkWidget* w, const char* c) { gtk_widget_remove_css_class(w, c); }
void setClass(GtkWidget* w, const char* c, bool on) { on ? addClass(w, c) : rmClass(w, c); }

GtkWidget* label(const std::string& text, const char* css = nullptr, float xalign = 0.f) {
  GtkWidget* l = gtk_label_new(text.c_str());
  gtk_label_set_xalign(GTK_LABEL(l), xalign);
  if (css) {  // "meta faint" means two classes
    std::string all = css;
    for (size_t p = 0; p < all.size();) {
      size_t sp = all.find(' ', p);
      std::string one = all.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
      if (!one.empty()) addClass(l, one.c_str());
      if (sp == std::string::npos) break;
      p = sp + 1;
    }
  }
  return l;
}

void clearChildren(GtkWidget* box) {
  while (GtkWidget* c = gtk_widget_get_first_child(box)) gtk_box_remove(GTK_BOX(box), c);
}

// Icons never hold a colour of their own: they hold a role and read the live palette every time they draw, so a theme
// change recolours them without rebuilding anything.
enum class Role { Identity, Fg2, Fg3, Fg3Faint, Accent, Red };

struct IconData {
  Icon icon;
  double param = 0;
  Role role = Role::Identity;
};

GtkWidget* iconWidget(Icon ic, int size, double param = 0, bool tinted = true) {
  GtkWidget* da = gtk_drawing_area_new();
  gtk_widget_set_size_request(da, size, size);
  gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
  auto* d = new IconData{ic, param, tinted ? Role::Identity : Role::Fg2};
  g_object_set_data_full(G_OBJECT(da), "icon", d, [](gpointer p) { delete static_cast<IconData*>(p); });
  gtk_drawing_area_set_draw_func(
      GTK_DRAWING_AREA(da),
      [](GtkDrawingArea* area, cairo_t* cr, int w, int, gpointer) {
        auto* id = static_cast<IconData*>(g_object_get_data(G_OBJECT(area), "icon"));
        Rgba c;
        switch (id->role) {
          case Role::Identity: c = draw::colorFor(id->icon); break;
          case Role::Fg2: c = gPal.fg2; break;
          case Role::Fg3: c = gPal.fg3; break;
          case Role::Fg3Faint: c = withAlpha(gPal.fg3, 0.55); break;
          case Role::Accent: c = gPal.accent; break;
          case Role::Red: c = gPal.red; break;
        }
        draw::icon(cr, id->icon, w, c, id->param);
      },
      nullptr, nullptr);
  return da;
}
void iconSet(GtkWidget* da, Icon ic, double param) {
  auto* d = static_cast<IconData*>(g_object_get_data(G_OBJECT(da), "icon"));
  d->icon = ic;
  d->param = param;
  gtk_widget_queue_draw(da);
}
void iconRole(GtkWidget* da, Role r) {
  auto* d = static_cast<IconData*>(g_object_get_data(G_OBJECT(da), "icon"));
  d->role = r;
  gtk_widget_queue_draw(da);
}

void later(std::function<void()> fn) {
  auto* f = new std::function<void()>(std::move(fn));
  g_idle_add(+[](gpointer p) -> gboolean {
    auto* fn = static_cast<std::function<void()>*>(p);
    (*fn)();
    delete fn;
    return G_SOURCE_REMOVE;
  }, f);
}

// A keycap pill ("↵", "Esc", "P").
GtkWidget* keycap(const std::string& k) { return label(k, "keycap", 0.5f); }

// Any widget as a mouse target. Never focusable -- the window-level key handler owns the keyboard.
void onClick(GtkWidget* w, std::function<void()> fn) {
  auto* f = new std::function<void()>(std::move(fn));
  GtkGesture* g = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
  g_signal_connect(g, "pressed", G_CALLBACK(+[](GtkGestureClick* gc, int, double, double, gpointer) {
                     gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
                   }),
                   nullptr);
  g_signal_connect(g, "released", G_CALLBACK(+[](GtkGestureClick* gc, int, double, double, gpointer d) {
                     (*static_cast<std::function<void()>*>(d))();
                     gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
                   }),
                   f);
  gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(g));
  g_object_set_data_full(G_OBJECT(w), "click-fn", f, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
  gtk_widget_set_cursor_from_name(w, "pointer");
}

GtkWidget* button(const std::string& text, const std::string& key, const char* cssClass, std::function<void()> fn) {
  GtkWidget* b = hbox(10);
  addClass(b, "btn");
  if (cssClass && *cssClass) addClass(b, cssClass);
  gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(b), label(text));
  if (!key.empty()) gtk_box_append(GTK_BOX(b), keycap(key));
  onClick(b, std::move(fn));
  return b;
}

std::string joinTags(const std::string& raw) {
  std::string out;
  for (auto& t : splitComma(raw)) {
    std::string v = trimmed(t);
    if (!v.empty() && v[0] == '#') v = trimmed(v.substr(1));
    if (v.empty()) continue;
    if (!out.empty()) out += ",";
    out += v;
  }
  return out;
}

std::string entryText(GtkWidget* entry) { return gtk_editable_get_text(GTK_EDITABLE(entry)); }

// A markdown notes area with a ghost placeholder (GtkTextView has none of its own).
GtkWidget* textArea(const std::string& initial, const std::string& placeholder, GtkWidget** viewOut, int minHeight = 38) {
  GtkWidget* ov = gtk_overlay_new();
  GtkWidget* tv = md::create(initial, false);
  gtk_widget_set_size_request(tv, -1, minHeight);
  gtk_overlay_set_child(GTK_OVERLAY(ov), tv);
  GtkWidget* ph = label(placeholder, "hint");
  gtk_widget_set_can_target(ph, FALSE);
  gtk_widget_set_halign(ph, GTK_ALIGN_START);
  gtk_widget_set_valign(ph, GTK_ALIGN_START);
  gtk_widget_set_margin_top(ph, 2);
  gtk_widget_set_visible(ph, initial.empty());
  gtk_overlay_add_overlay(GTK_OVERLAY(ov), ph);
  g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), "changed", G_CALLBACK(+[](GtkTextBuffer* b, gpointer p) {
                     gtk_widget_set_visible(GTK_WIDGET(p), gtk_text_buffer_get_char_count(b) == 0);
                   }),
                   ph);
  *viewOut = tv;
  return ov;
}

// Smoothly scrolls a GtkScrolledWindow's vertical adjustment.
struct Scroller {
  GtkWidget* sw = nullptr;
  double from = 0, to = 0;
  gint64 t0 = 0;
  guint tick = 0;
  void go(GtkWidget* scrolled, double target) {
    sw = scrolled;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    double maxv = std::max(0.0, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    target = std::clamp(target, 0.0, maxv);
    from = gtk_adjustment_get_value(adj);
    to = target;
    if (std::fabs(to - from) < 0.5) return;
    t0 = 0;
    if (tick) return;  // already animating; the running tick picks up the new target
    tick = gtk_widget_add_tick_callback(
        sw,
        [](GtkWidget* w, GdkFrameClock* fc, gpointer ud) -> gboolean {
          auto* s = static_cast<Scroller*>(ud);
          gint64 now = gdk_frame_clock_get_frame_time(fc);
          if (!s->t0) s->t0 = now;
          double t = (now - s->t0) / 1000.0 / 170.0;
          GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(w));
          gtk_adjustment_set_value(adj, ease::lerp(s->from, s->to, ease::outCubic(t)));
          if (t >= 1) {
            s->tick = 0;
            return G_SOURCE_REMOVE;
          }
          return G_SOURCE_CONTINUE;
        },
        this, nullptr);
  }
};

// Keeps `child` visible inside a scrolled window whose content is `container` (no animation; used in popups).
void scrollIntoView(GtkWidget* scrolled, GtkWidget* container, GtkWidget* child) {
  graphene_rect_t b;
  if (!gtk_widget_compute_bounds(child, container, &b)) return;
  GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
  if (b.origin.y < v) gtk_adjustment_set_value(adj, b.origin.y);
  else if (b.origin.y + b.size.height > v + page) gtk_adjustment_set_value(adj, b.origin.y + b.size.height - page);
}

// ---------------------------------------------------------------------------------------------------------------
// model types
// ---------------------------------------------------------------------------------------------------------------

struct View {
  enum Kind { Smart, Area, Project } kind = Smart;
  std::string name = "Today";  // smart list name
  int id = 0;                  // area / project id
  std::string title = "Today";
  bool operator==(const View& o) const { return kind == o.kind && name == o.name && id == o.id; }
  bool isLogLike() const {
    return kind == Smart && (name == "Logbook" || name == "Logged Projects" || name == "Archived Areas");
  }
};

Icon smartIcon(const std::string& n) {
  if (n == "Inbox") return Icon::Inbox;
  if (n == "Today" || n == "Tomorrow") return Icon::Today;
  if (n == "Upcoming") return Icon::Upcoming;
  if (n == "Anytime") return Icon::Anytime;
  if (n == "Someday") return Icon::Someday;
  if (n == "Deadlines") return Icon::Deadlines;
  if (n == "Logbook" || n == "Logged Projects") return Icon::Logbook;
  return Icon::Archive;
}

class Gui;

struct Row {
  enum Type { Task, Section, Heading, More, LogToggle } type = Task;
  Item item;
  std::string text;  // section / heading text
  char groupKind = 0;  // Section rows of a grouped list: 'a' = area header, 'p' = project header, 0 = a plain section
  int groupId = 0;
  bool logged = false;  // a completed to-do in a project's logged timeline
  double pie = -1;      // projects: share of to-dos done, for the pie inside the circle
  Gui* gui = nullptr;
  GtkWidget *revealer = nullptr, *box = nullptr, *check = nullptr, *strike = nullptr, *title = nullptr;
  // state
  bool done = false;            // current (DB) completion state
  bool hot = false;             // pointer over the checkbox
  bool pendingRemoval = false;  // completed, lingering a moment before it collapses away
  bool leaving = false;         // collapse started
  bool animating = false;
  bool animToDone = true;
  double animT = 0;
  gint64 t0 = 0;
  guint tickId = 0;
  guint timer = 0;
  bool doneClass = false;
  bool selectable() const { return type == Task || type == Heading || type == More || type == LogToggle; }
  int level() const { return groupKind == 'p' ? 1 : 0; }  // nesting of a section header, for pruning
  ~Row() {
    if (timer) g_source_remove(timer);
    if (tickId && check) gtk_widget_remove_tick_callback(check, tickId);
  }
};

struct PalItem {
  std::string label, sub, kind;
  Icon icon = Icon::Search;
  double iconParam = 0;
  bool hiddenWhenEmpty = false;  // e.g. individual tasks: only surface once you type
  std::function<void()> act;
};

struct ListRef {
  bool found = false, inbox = false;
  int areaId = 0, projectId = 0;
  std::string label;  // "Home Renovation" / "Health"
  std::string kind;   // "Project" / "Area"
};

// One checklist item in the editor: a small circle you tick, and its text.
struct ClRow {
  GtkWidget *row = nullptr, *circle = nullptr, *entry = nullptr;
  bool done = false, hot = false;
  double fill = 0, from = 0;
  gint64 t0 = 0;
  guint tick = 0;
};

struct Editor {
  bool isNew = false;
  Item orig;
  int headingId = 0;
  int afterSort = -1;  // insert after this sort order (new tasks)
  GtkWidget *card = nullptr, *title = nullptr, *notes = nullptr, *clBox = nullptr, *clAddRow = nullptr;
  std::vector<std::unique_ptr<ClRow>> cl;
  GtkWidget *when = nullptr, *deadline = nullptr, *tags = nullptr, *list = nullptr, *hint = nullptr;
  GtkWidget *whenField = nullptr, *deadlineField = nullptr, *listField = nullptr;
  Row* row = nullptr;
};

}  // namespace

namespace {

const std::vector<std::string> kSmart = {"Inbox", "Today", "Upcoming", "Anytime", "Someday", "Logbook"};

std::string deadlineLabel(const std::string& iso, bool& due) {
  due = false;
  if (!guidates::validIso(iso)) return iso;
  int d = guidates::daysFromToday(iso);
  if (d < 0) { due = true; return d == -1 ? "Yesterday" : std::to_string(-d) + "d overdue"; }
  if (d == 0) { due = true; return "Due today"; }
  if (d == 1) return "Tomorrow";
  if (d <= 7) return std::to_string(d) + "d left";
  return guidates::friendly(iso);
}

// "Mon 5 Oct" / "Yesterday" -- when a logged item was checked off.
std::string completedLabel(const Item& it) {
  std::string pre = it.status == "cancelled" ? "Canceled" : "";
  if (it.completedAt.size() < 10) return pre;
  std::string f = guidates::friendly(it.completedAt.substr(0, 10));
  return pre.empty() ? f : pre + "  ·  " + f;
}

double projectFraction(Store& s, int projectId) {
  auto all = s.viewProject(projectId, "", false);
  if (all.empty()) return 0.0;
  int done = 0, n = 0;
  for (auto& t : all) {
    if (t.kind != 't') continue;
    ++n;
    if (t.status == "done") ++done;
  }
  return n ? (double)done / n : 0.0;
}

// The order lists appear in the sidebar -- loose projects, then each area followed by its projects -- as one running
// number, so a grouped to-do list can follow the sidebar exactly.
struct Ord {
  struct Info {
    char kind = 'a';  // 'a' area, 'p' project
    int id = 0, areaId = 0;
    std::string name, areaName;
  };
  std::map<int, int> proj, area;  // id -> position
  std::vector<Info> info;
};

Ord buildOrd(Store& s) {
  Ord o;
  auto add = [&](Ord::Info i) {
    (i.kind == 'a' ? o.area : o.proj)[i.id] = (int)o.info.size();
    o.info.push_back(std::move(i));
  };
  auto projects = s.projects();
  for (auto& p : projects)
    if (s.projectAreaId(p.id) == 0) add({'p', p.id, 0, p.name, ""});
  for (auto& a : s.areas()) {
    add({'a', a.id, 0, a.name, ""});
    for (auto& p : s.projectsInArea(a.id, false)) add({'p', p.id, a.id, p.name, a.name});
  }
  return o;
}

// -1 = on no list at all (sorts first, no header).
int ordKey(const Ord& o, const Item& it) {
  if (it.kind == 't' && it.projectId) {
    auto f = o.proj.find(it.projectId);
    if (f != o.proj.end()) return f->second;
  }
  if (it.areaId) {
    auto f = o.area.find(it.areaId);
    if (f != o.area.end()) return f->second;
  }
  return -1;
}

class Gui {
 public:
  explicit Gui(Store& s) : s_(s) {}
  ~Gui() {
    if (worker_.joinable()) worker_.join();
  }
  void activate(GtkApplication* app);

 private:
  // ---- state ----------------------------------------------------------------------------------------------
  Store& s_;
  GtkApplication* app_ = nullptr;
  GtkWidget *win_ = nullptr, *overlay_ = nullptr, *sbRevealer_ = nullptr, *sbBox_ = nullptr, *mainScroll_ = nullptr,
            *content_ = nullptr, *headerIcon_ = nullptr, *titleLabel_ = nullptr, *crumbLabel_ = nullptr,
            *notesBox_ = nullptr, *listBox_ = nullptr, *syncIcon_ = nullptr, *syncDot_ = nullptr,
            *syncBtn_ = nullptr, *toastBox_ = nullptr, *toastLabel_ = nullptr, *toastKey_ = nullptr,
            *modalLayer_ = nullptr, *modalHost_ = nullptr, *emptyState_ = nullptr;
  View view_;
  std::vector<View> history_;
  std::vector<Item> items_;
  size_t hiddenCount_ = 0, limit_ = kRowLimit;
  std::vector<std::unique_ptr<Row>> rows_;
  Row* sel_ = nullptr;
  std::unique_ptr<Editor> editor_;
  std::string pendingEditorSigilsHint_;

  struct SbEntry {
    View view;
    GtkWidget *box = nullptr, *count = nullptr, *icon = nullptr;
  };
  std::vector<SbEntry> sb_;
  int sbCursor_ = 0;
  bool sbFocus_ = false;

  struct Undo {
    int id;
    char kind;
  };
  std::vector<Undo> undo_;

  int lastDataVersion_ = 0;
  bool pendingReload_ = false;
  int pendingSelId_ = 0;
  char pendingSelKind_ = 't';
  int justAddedId_ = 0, justRestoredId_ = 0;
  Scroller scroller_;
  guint toastShowSrc_ = 0, toastHideSrc_ = 0, toastGoneSrc_ = 0;
  guint themeApplied_ = 0, themeDebounce_ = 0;
  unsigned modalGen_ = 0;
  std::string notesShown_;
  long themeStamp_ = 0;
  int pollTick_ = 0;
  bool grouped_ = false;                 // Shift+A: group Today / Tomorrow / Anytime / Someday by area and project
  std::map<int, bool> loggedShown_;      // per project: is the logged-to-dos timeline expanded?
  std::vector<Item> logged_;             // completed to-dos of the project on screen, newest first
  size_t loggedHidden_ = 0;

  // shared so the handler that is currently running can close its own modal without destroying itself mid-call
  std::shared_ptr<std::function<bool(guint, GdkModifierType)>> modalKey_;
  struct Pal {
    std::vector<PalItem> items;
    std::vector<int> shown;
    int sel = 0;
    GtkWidget *entry = nullptr, *results = nullptr, *scroll = nullptr;
  };
  std::shared_ptr<Pal> pal_;

  // sync
  std::thread worker_;
  std::atomic<bool> workerBusy_{false};
  bool syncing_ = false, dirty_ = false;
  std::string remote_;
  double spinStop_ = 0;
  int behind_ = 0;
  double spin_ = 0;
  guint spinTick_ = 0;
  std::optional<SyncMode> queuedSync_;
  GtkWidget *syncStatLabel_ = nullptr, *syncStat2_ = nullptr;  // live lines in the open sync dialog

  // ---- plumbing -------------------------------------------------------------------------------------------
  GtkCssProvider* css_ = nullptr;
  void applyTheme(bool force = false);
  void scheduleTheme();
  void buildWindow();
  void buildSidebar();
  void refreshSidebar();
  void updateSidebarSelection();
  GtkWidget* buildFooter();

  // ---- navigation -----------------------------------------------------------------------------------------
  void goTo(const View& v, bool push = true);
  void back();
  void goSmart(const std::string& name) { goTo(View{View::Smart, name, 0, name}); }
  View homeViewFor(const Item& t);
  void jumpToTask(const Item& t);

  // ---- data -> widgets ------------------------------------------------------------------------------------
  void fetchItems();
  void reload(bool keepSel = true);
  void rebuildRows();
  void buildTaskRow(Row& r, bool animateIn);
  GtkWidget* buildSimpleRow(Row& r);
  void buildEmptyState();
  void updateHeader();
  void refreshCounts();
  bool groupable() const;
  bool groupActive() const { return grouped_ && groupable(); }
  void toggleGrouping();
  void toggleLogged();
  bool loggedExpanded() const;
  GtkWidget* buildGroupHeader(Row& r);
  GtkWidget* buildLogToggle(Row& r);
  void openLink(const std::string& url);
  void linkFallback(const std::string& url);
  void newProject();
  void newArea();
  void chooseNewList();
  void editList();
  void completeList();
  std::string sectionLabel(const Item& x) const;
  void addSectionClasses();

  // ---- selection ------------------------------------------------------------------------------------------
  int indexOf(const Row* r) const;
  Row* nextSelectable(int from, int dir) const;
  void select(Row* r, bool scroll = true);
  void moveSel(int dir);
  void scrollToSel();
  void reindex();

  // ---- task actions ---------------------------------------------------------------------------------------
  void toggleRow(Row* r);
  void animateRow(Row* r, bool toDone);
  void scheduleCollapse(Row* r, guint delayMs);
  void collapseRow(Row* r);
  void dropRow(Row* r);
  void pruneEmptySections();
  void undo();
  void activateSel();
  void promptDate(bool deadline);
  void promptTags();
  void pickList();
  void confirmDelete();
  void reorder(int dir);
  Item forSave(Item it);
  void saveItem(Item it);
  void markDirty() { dirty_ = true; updateSyncDot(); }
  void updateSyncDot();

  // ---- editor ---------------------------------------------------------------------------------------------
  void startEditor(std::unique_ptr<Editor> e, GtkWidget* after, const std::string& whenInit, const std::string& listInit);
  void openEditor(Row* r);
  void newTask();
  void newHeading();
  bool applyEditor(int& savedId);
  bool finishEditor(bool save, bool thenNew = false);
  void destroyEditorWidgets();
  ClRow* addChecklistRow(int index, const std::string& text, bool done, bool focus);
  void removeChecklistRow(ClRow* c);
  void toggleChecklistRow(ClRow* c);
  bool editorKey(guint keyval, GdkModifierType state);
  Row* lastOpenRow() const;
  void updateEditorHint();
  ListRef resolveList(const std::string& q);
  GtkWidget* editorField(const char* cap, GtkWidget** entry, const std::string& text, const char* placeholder, GtkWidget** fieldBox);

  // ---- overlays -------------------------------------------------------------------------------------------
  void showModal(GtkWidget* card, std::function<bool(guint, GdkModifierType)> keyFn, GtkWidget* focus = nullptr);
  void closeModal();
  void openPalette(const std::string& placeholder, std::vector<PalItem> items);
  void palRefresh();
  void palMove(int d);
  void palRun();
  std::vector<PalItem> commandItems();
  std::vector<PalItem> listPickItems(const std::function<void(int, int)>& pick);
  void openPrompt(const std::string& title, const std::string& placeholder, const std::string& initial,
                  std::function<std::string(const std::string&, bool&)> hint, std::function<bool(const std::string&)> submit,
                  const std::string& confirmKeyLabel = "Save");
  void openConfirm(const std::string& title, const std::string& body, const std::string& action, bool danger,
                   std::function<void()> go);
  void openHelp();
  void toast(const std::string& msg, const std::string& key = "", bool error = false, int ms = 2600);
  void toggleSidebar();

  // ---- sync -----------------------------------------------------------------------------------------------
  void openSyncDialog();
  void startSync(SyncMode mode);
  void probeSync();
  void runWorker(std::function<std::function<void()>()> job);
  void confirmSync(SyncMode mode);
  void updateSyncLabels();
  void onSyncDone(SyncMode mode, SyncOutcome out, const std::string& err);
  void setSyncing(bool on);
  std::string remoteUrl();

  // ---- input ----------------------------------------------------------------------------------------------
  bool onKey(guint keyval, GdkModifierType state);
  bool onListKey(guint keyval, GdkModifierType state);
  bool onSidebarKey(guint keyval, GdkModifierType state);
  void poll();
};

// ---------------------------------------------------------------------------------------------------------------
// activation, theme, window
// ---------------------------------------------------------------------------------------------------------------

void Gui::applyTheme(bool force) {
  Palette np = resolvePalette();
  themeStamp_ = themeCssStamp();
  if (!force && css_ && samePalette(np, gPal)) return;
  gPal = np;
  if (!css_) {
    css_ = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_),
                                                GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
  }
  std::string css = buildCss(gPal);
  css += ".section.selected { background-color: " + cssColor(gPal.select) + "; border-radius: 8px; border-bottom-color: transparent; }\n";
  gtk_css_provider_load_from_string(css_, css.c_str());
  md::repaintAll();
  if (win_) gtk_widget_queue_draw(win_);
}

// GTK reloads a changed theme asynchronously, so wait a beat before reading its colours back.
void Gui::scheduleTheme() {
  if (themeDebounce_) g_source_remove(themeDebounce_);
  themeDebounce_ = g_timeout_add(140, +[](gpointer p) -> gboolean {
    auto* g = static_cast<Gui*>(p);
    g->themeDebounce_ = 0;
    g->applyTheme(false);
    return G_SOURCE_REMOVE;
  }, this);
}

void Gui::activate(GtkApplication* app) {
  app_ = app;
  if (GtkSettings* st = gtk_settings_get_default()) {
    // follow the active GTK theme live: a new theme name, a dark/light flip, a new colour scheme
    for (const char* prop : {"gtk-theme-name", "gtk-application-prefer-dark-theme", "gtk-interface-color-scheme"}) {
      if (!g_object_class_find_property(G_OBJECT_GET_CLASS(st), prop)) continue;
      std::string sig = std::string("notify::") + prop;
      g_signal_connect_swapped(st, sig.c_str(), G_CALLBACK(+[](gpointer self) { static_cast<Gui*>(self)->scheduleTheme(); }), this);
    }
  }
  {
    Config cfg((dataDir() / "config").string());
    remote_ = cfg.get("mirror_remote").value_or("");
  }
  md::gOpenLink = [this](const std::string& u) { openLink(u); };
  applyTheme(true);
  buildWindow();
  lastDataVersion_ = s_.dataVersion();
  goTo(View{View::Smart, "Today", 0, "Today"}, false);
  refreshSidebar();
  gtk_window_present(GTK_WINDOW(win_));
  g_timeout_add(750, +[](gpointer p) -> gboolean { static_cast<Gui*>(p)->poll(); return G_SOURCE_CONTINUE; }, this);
  if (!remote_.empty()) g_timeout_add_seconds(300, +[](gpointer p) -> gboolean { static_cast<Gui*>(p)->probeSync(); return G_SOURCE_CONTINUE; }, this);
  if (!remote_.empty()) g_timeout_add(900, +[](gpointer p) -> gboolean { static_cast<Gui*>(p)->probeSync(); return G_SOURCE_REMOVE; }, this);
}

void Gui::buildWindow() {
  win_ = gtk_application_window_new(app_);
  addClass(win_, "stride");
  gtk_window_set_title(GTK_WINDOW(win_), "Stride");
  gtk_window_set_default_size(GTK_WINDOW(win_), 1040, 740);
  const char* deco = std::getenv("STRIDE_DECORATIONS");
  gtk_window_set_decorated(GTK_WINDOW(win_), deco && std::string(deco) == "1");

  overlay_ = gtk_overlay_new();
  gtk_window_set_child(GTK_WINDOW(win_), overlay_);
  GtkWidget* root = hbox(0);
  gtk_overlay_set_child(GTK_OVERLAY(overlay_), root);

  // sidebar
  sbRevealer_ = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(sbRevealer_), GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
  gtk_revealer_set_transition_duration(GTK_REVEALER(sbRevealer_), 220);
  gtk_revealer_set_reveal_child(GTK_REVEALER(sbRevealer_), TRUE);
  gtk_widget_set_hexpand(sbRevealer_, FALSE);  // a hexpand label deep inside would otherwise make the sidebar claim half the window
  GtkWidget* side = vbox(0);
  gtk_widget_set_hexpand(side, FALSE);
  addClass(side, "sidebar");
  gtk_widget_set_size_request(side, 246, -1);
  GtkWidget* handle = gtk_window_handle_new();
  GtkWidget* topPad = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(topPad, -1, 18);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), topPad);
  gtk_box_append(GTK_BOX(side), handle);
  GtkWidget* sbScroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sbScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(sbScroll, TRUE);
  gtk_widget_set_hexpand(sbScroll, FALSE);
  sbBox_ = vbox(0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sbScroll), sbBox_);
  gtk_box_append(GTK_BOX(side), sbScroll);
  GtkWidget* sbFoot = hbox(0);
  addClass(sbFoot, "sb-foot");
  GtkWidget* nl = hbox(8);
  addClass(nl, "sb-new");
  GtkWidget* nlIcon = iconWidget(Icon::Plus, 15, 0, true);
  iconRole(nlIcon, Role::Accent);
  gtk_box_append(GTK_BOX(nl), nlIcon);
  gtk_box_append(GTK_BOX(nl), label("New List", "sb-new-label"));
  gtk_widget_set_tooltip_text(nl, "New project or area  (P / A)");
  onClick(nl, [this] { chooseNewList(); });
  gtk_box_append(GTK_BOX(sbFoot), nl);
  gtk_box_append(GTK_BOX(side), sbFoot);
  gtk_revealer_set_child(GTK_REVEALER(sbRevealer_), side);
  gtk_box_append(GTK_BOX(root), sbRevealer_);

  // main column
  GtkWidget* main = vbox(0);
  gtk_widget_set_hexpand(main, TRUE);
  mainScroll_ = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(mainScroll_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(mainScroll_, TRUE);
  content_ = vbox(0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mainScroll_), content_);

  // page header (draggable on an undecorated window)
  GtkWidget* hdrHandle = gtk_window_handle_new();
  GtkWidget* hdr = vbox(2);
  gtk_widget_set_margin_top(hdr, 34);
  gtk_widget_set_margin_start(hdr, 36);
  gtk_widget_set_margin_end(hdr, 36);
  gtk_widget_set_margin_bottom(hdr, 10);
  crumbLabel_ = label("", "page-crumb");
  gtk_box_append(GTK_BOX(hdr), crumbLabel_);
  GtkWidget* titleRow = hbox(12);
  headerIcon_ = iconWidget(Icon::Today, 26);
  gtk_box_append(GTK_BOX(titleRow), headerIcon_);
  titleLabel_ = label("Today", "page-title");
  gtk_box_append(GTK_BOX(titleRow), titleLabel_);
  gtk_box_append(GTK_BOX(hdr), titleRow);
  notesBox_ = vbox(0);
  gtk_widget_set_margin_top(notesBox_, 6);
  gtk_box_append(GTK_BOX(hdr), notesBox_);
  {  // double-click the title or the description to edit the project / rename the area
    GtkGesture* dc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dc), GDK_BUTTON_PRIMARY);
    g_signal_connect(dc, "pressed", G_CALLBACK(+[](GtkGestureClick*, int n, double, double, gpointer p) {
      if (n == 2) static_cast<Gui*>(p)->editList();
    }), this);
    gtk_widget_add_controller(hdr, GTK_EVENT_CONTROLLER(dc));
  }
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(hdrHandle), hdr);
  gtk_box_append(GTK_BOX(content_), hdrHandle);

  listBox_ = vbox(0);
  gtk_box_append(GTK_BOX(content_), listBox_);
  GtkWidget* bottomPad = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(bottomPad, -1, 90);
  gtk_box_append(GTK_BOX(content_), bottomPad);

  gtk_box_append(GTK_BOX(main), mainScroll_);
  gtk_box_append(GTK_BOX(main), buildFooter());
  gtk_box_append(GTK_BOX(root), main);

  // toast
  toastBox_ = hbox(10);
  addClass(toastBox_, "toast");
  gtk_widget_set_halign(toastBox_, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(toastBox_, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(toastBox_, 74);
  gtk_widget_set_can_target(toastBox_, FALSE);
  gtk_widget_set_visible(toastBox_, FALSE);
  toastLabel_ = label("");
  toastKey_ = keycap("");
  addClass(toastKey_, "toast-key");
  rmClass(toastKey_, "keycap");
  gtk_box_append(GTK_BOX(toastBox_), toastLabel_);
  gtk_box_append(GTK_BOX(toastBox_), toastKey_);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), toastBox_);

  // modal layer
  modalLayer_ = vbox(0);
  addClass(modalLayer_, "scrim");
  gtk_widget_set_visible(modalLayer_, FALSE);
  modalHost_ = vbox(0);
  gtk_widget_set_halign(modalHost_, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(modalHost_, GTK_ALIGN_START);
  gtk_widget_set_margin_top(modalHost_, 84);
  gtk_box_append(GTK_BOX(modalLayer_), modalHost_);
  onClick(modalLayer_, [this] { closeModal(); });
  gtk_widget_set_cursor_from_name(modalLayer_, nullptr);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), modalLayer_);

  // keyboard: one capture-phase controller on the window; it decides who gets each key
  GtkEventController* kc = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
  g_signal_connect(kc, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType st, gpointer p) -> gboolean {
                     return static_cast<Gui*>(p)->onKey(kv, st) ? GDK_EVENT_STOP : GDK_EVENT_PROPAGATE;
                   }),
                   this);
  gtk_widget_add_controller(win_, kc);

  g_signal_connect(win_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer p) -> gboolean {
                     auto* self = static_cast<Gui*>(p);
                     if (self->editor_) self->finishEditor(true);  // never lose a half-typed to-do
                     return FALSE;
                   }),
                   this);
}

GtkWidget* Gui::buildFooter() {
  GtkWidget* f = hbox(4);
  addClass(f, "footer");

  GtkWidget* nb = hbox(8);
  addClass(nb, "foot-btn");
  GtkWidget* plus = iconWidget(Icon::Plus, 16, 0, true);
  iconRole(plus, Role::Accent);
  gtk_box_append(GTK_BOX(nb), plus);
  gtk_box_append(GTK_BOX(nb), label("New To-Do", "new-label"));
  gtk_widget_set_tooltip_text(nb, "New to-do  (N)");
  onClick(nb, [this] { newTask(); });
  gtk_box_append(GTK_BOX(f), nb);

  GtkWidget* sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(sp, TRUE);
  gtk_box_append(GTK_BOX(f), sp);

  auto iconBtn = [&](Icon ic, const char* tip, std::function<void()> fn, GtkWidget** keep = nullptr) {
    GtkWidget* b = gtk_overlay_new();
    addClass(b, "foot-btn");
    GtkWidget* i = iconWidget(ic, 18, 0, false);
    gtk_overlay_set_child(GTK_OVERLAY(b), i);
    gtk_widget_set_tooltip_text(b, tip);
    onClick(b, std::move(fn));
    gtk_box_append(GTK_BOX(f), b);
    if (keep) *keep = i;
    return b;
  };
  iconBtn(Icon::Search, "Quick find  (Ctrl+K)", [this] { openPalette("Jump to a list, project or to-do, or run a command…", commandItems()); });
  syncBtn_ = iconBtn(Icon::Sync, "Sync with remote  (Shift+S)", [this] { openSyncDialog(); }, &syncIcon_);
  syncDot_ = gtk_drawing_area_new();
  gtk_widget_set_size_request(syncDot_, 8, 8);
  gtk_widget_set_halign(syncDot_, GTK_ALIGN_END);
  gtk_widget_set_valign(syncDot_, GTK_ALIGN_START);
  gtk_widget_set_can_target(syncDot_, FALSE);
  gtk_widget_set_visible(syncDot_, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(syncDot_), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    setSource(cr, gPal.accent);
    cairo_arc(cr, w / 2.0, h / 2.0, 3.4, 0, 2 * M_PI);
    cairo_fill(cr);
  }, nullptr, nullptr);
  gtk_overlay_add_overlay(GTK_OVERLAY(syncBtn_), syncDot_);
  iconBtn(Icon::Help, "Keyboard shortcuts  (?)", [this] { openHelp(); });
  return f;
}

void Gui::updateSyncDot() {
  if (!syncDot_) return;
  bool show = !remoteUrl().empty() && (dirty_ || behind_ > 0);
  gtk_widget_set_visible(syncDot_, show);
  std::string tip = "Sync with remote  (Shift+S)";
  if (behind_ > 0) tip += "\nRemote has newer changes";
  if (dirty_) tip += "\nLocal changes not yet pushed";
  gtk_widget_set_tooltip_text(syncBtn_, tip.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// sidebar
// ---------------------------------------------------------------------------------------------------------------

void Gui::refreshSidebar() {
  clearChildren(sbBox_);
  sb_.clear();
  auto add = [&](const View& v, GtkWidget* icon, bool indent) {
    GtkWidget* row = hbox(10);
    addClass(row, "sb-row");
    if (indent) addClass(row, "indent");
    gtk_box_append(GTK_BOX(row), icon);
    GtkWidget* l = label(v.title, "sb-label");
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(row), l);
    GtkWidget* cnt = label("", "sb-count", 1.f);
    gtk_box_append(GTK_BOX(row), cnt);
    View vv = v;
    onClick(row, [this, vv] { sbFocus_ = false; goTo(vv); });
    gtk_box_append(GTK_BOX(sbBox_), row);
    sb_.push_back({v, row, cnt, icon});
  };
  for (size_t i = 0; i < kSmart.size(); ++i) {
    if (kSmart[i] == "Logbook") {
      GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      addClass(gap, "sb-gap");
      gtk_box_append(GTK_BOX(sbBox_), gap);
    }
    add(View{View::Smart, kSmart[i], 0, kSmart[i]}, iconWidget(smartIcon(kSmart[i]), 19), false);
  }
  // loose projects (no area), then each area with its projects
  auto fraction = [&](int pid) {
    auto all = s_.viewProject(pid, "", false);
    if (all.empty()) return 0.0;
    int done = 0;
    for (auto& t : all) if (t.status == "done") ++done;
    return (double)done / all.size();
  };
  auto projects = s_.projects(false, true);
  bool anyLoose = false;
  for (auto& p : projects)
    if (p.sub.empty()) {
      if (!anyLoose) {
        GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        addClass(gap, "sb-gap");
        gtk_box_append(GTK_BOX(sbBox_), gap);
        anyLoose = true;
      }
      GtkWidget* ic = iconWidget(Icon::Project, 19, fraction(p.id), false);
      add(View{View::Project, "", p.id, p.name}, ic, false);
    }
  for (auto& a : s_.areas()) {
    GtkWidget* hdr = hbox(8);
    addClass(hdr, "sb-row");
    GtkWidget* ai = iconWidget(Icon::Area, 19, 0, false);
    gtk_box_append(GTK_BOX(hdr), ai);
    GtkWidget* l = label(a.name, "sb-label");
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_widget_set_margin_top(hdr, 8);
    gtk_box_append(GTK_BOX(hdr), l);
    View av{View::Area, "", a.id, a.name};
    onClick(hdr, [this, av] { sbFocus_ = false; goTo(av); });
    gtk_box_append(GTK_BOX(sbBox_), hdr);
    sb_.push_back({av, hdr, nullptr, ai});
    for (auto& p : s_.projectsInArea(a.id, true)) {
      GtkWidget* ic = iconWidget(Icon::Project, 17, fraction(p.id), false);
      add(View{View::Project, "", p.id, p.name}, ic, true);
    }
  }
  refreshCounts();
  updateSidebarSelection();
}

void Gui::refreshCounts() {
  for (auto& e : sb_) {
    if (e.view.kind == View::Project && e.icon) {  // keep the progress pies current without rebuilding the sidebar
      auto all = s_.viewProject(e.view.id, "", false);
      int done = 0;
      for (auto& t : all) if (t.status == "done") ++done;
      iconSet(e.icon, Icon::Project, all.empty() ? 0.0 : (double)done / all.size());
    }
    if (!e.count) continue;
    if (e.view.kind == View::Smart && (e.view.name == "Inbox" || e.view.name == "Today")) {
      size_t n = e.view.name == "Inbox" ? s_.viewInbox("").size() : s_.viewDay(false, "").size();
      gtk_label_set_text(GTK_LABEL(e.count), n ? std::to_string(n).c_str() : "");
    }
  }
}

void Gui::updateSidebarSelection() {
  for (size_t i = 0; i < sb_.size(); ++i) {
    setClass(sb_[i].box, "current", sb_[i].view == view_);
    setClass(sb_[i].box, "cursor", sbFocus_ && (int)i == sbCursor_);
  }
}

void Gui::toggleSidebar() {
  bool shown = gtk_revealer_get_reveal_child(GTK_REVEALER(sbRevealer_));
  gtk_revealer_set_reveal_child(GTK_REVEALER(sbRevealer_), !shown);
  if (shown) { sbFocus_ = false; updateSidebarSelection(); }
}

// ---------------------------------------------------------------------------------------------------------------
// navigation
// ---------------------------------------------------------------------------------------------------------------

void Gui::goTo(const View& v, bool push) {
  if (editor_ && !finishEditor(true)) return;
  if (push && !(v == view_)) {
    history_.push_back(view_);
    if (history_.size() > 30) history_.erase(history_.begin());
  }
  view_ = v;
  limit_ = kRowLimit;
  sel_ = nullptr;
  updateSidebarSelection();
  reload(false);
  GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(mainScroll_));
  gtk_adjustment_set_value(adj, 0);
}

void Gui::back() {
  while (!history_.empty()) {
    View v = history_.back();
    history_.pop_back();
    if (v == view_) continue;
    goTo(v, false);
    return;
  }
}

View Gui::homeViewFor(const Item& t) {
  if (t.projectId) return View{View::Project, "", t.projectId, t.projectName};
  if (t.areaId) return View{View::Area, "", t.areaId, t.areaName};
  std::string td = today();
  if (t.someday) return View{View::Smart, "Someday", 0, "Someday"};
  if (t.doDate.empty() && t.deadline.empty()) return View{View::Smart, "Inbox", 0, "Inbox"};
  if ((!t.doDate.empty() && t.doDate <= td) || (!t.deadline.empty() && t.deadline <= td)) return View{View::Smart, "Today", 0, "Today"};
  return View{View::Smart, "Upcoming", 0, "Upcoming"};
}

void Gui::jumpToTask(const Item& t) {
  pendingSelId_ = t.id;
  pendingSelKind_ = t.kind;
  goTo(homeViewFor(t));
}

// ---------------------------------------------------------------------------------------------------------------
// data -> widgets
// ---------------------------------------------------------------------------------------------------------------

bool Gui::groupable() const {
  return view_.kind == View::Smart &&
         (view_.name == "Today" || view_.name == "Tomorrow" || view_.name == "Anytime" || view_.name == "Someday");
}

bool Gui::loggedExpanded() const {
  auto it = loggedShown_.find(view_.id);
  if (it != loggedShown_.end()) return it->second;
  return !const_cast<Store&>(s_).projectIsOpen(view_.id);  // a finished project opens with its history showing
}

void Gui::fetchItems() {
  items_.clear();
  logged_.clear();
  if (view_.kind == View::Project) {
    for (auto& t : s_.viewProject(view_.id, "", false)) {
      if (t.kind == 't' && t.status != "open") logged_.push_back(t);
      else items_.push_back(t);
    }
    std::stable_sort(logged_.begin(), logged_.end(), [](const Item& a, const Item& b) { return a.completedAt > b.completedAt; });
  } else if (view_.kind == View::Area) items_ = s_.viewArea(view_.id, "");
  else {
    const auto& n = view_.name;
    if (n == "Inbox") items_ = s_.viewInbox("");
    else if (n == "Today") items_ = s_.viewDay(false, "");
    else if (n == "Tomorrow") items_ = s_.viewDay(true, "");
    else if (n == "Upcoming") items_ = s_.viewUpcoming("");
    else if (n == "Anytime") items_ = s_.viewAnytime("");
    else if (n == "Someday") items_ = s_.viewSomeday("");
    else if (n == "Logbook") items_ = s_.viewLogbook();
    else if (n == "Deadlines") items_ = s_.viewDeadlines("");
    else if (n == "Logged Projects") items_ = s_.viewLoggedProjects();
    else if (n == "Archived Areas") items_ = s_.viewArchivedAreas();
  }
  if (groupActive()) {  // same order as the sidebar; to-dos keep their own order within a list
    Ord o = buildOrd(s_);
    std::stable_sort(items_.begin(), items_.end(), [&](const Item& a, const Item& b) { return ordKey(o, a) < ordKey(o, b); });
  }
  hiddenCount_ = 0;
  if (items_.size() > limit_) {
    hiddenCount_ = items_.size() - limit_;
    items_.resize(limit_);
  }
}

void Gui::toggleGrouping() {
  if (!groupable()) {
    toast("Grouping works in Today, Tomorrow, Anytime and Someday");
    return;
  }
  grouped_ = !grouped_;
  reload(true);
  toast(grouped_ ? "Grouped by area and project" : "Grouping off", "\xe2\x87\xa7" "A");
}

void Gui::toggleLogged() {
  if (view_.kind != View::Project) return;
  loggedShown_[view_.id] = !loggedExpanded();
  reload(true);
}

std::string Gui::sectionLabel(const Item& x) const {
  if (view_.kind == View::Area) return x.section;
  if (view_.kind == View::Project) return "";
  const auto& n = view_.name;
  if (n == "Upcoming") return guidates::friendly(x.doDate.empty() ? x.deadline : x.doDate);
  if (n == "Deadlines") return guidates::friendly(x.deadline);
  if (view_.isLogLike()) return bucketLabel(x.completedAt);
  return "";
}

void Gui::reload(bool keepSel) {
  if (editor_) { pendingReload_ = true; return; }
  int selId = 0;
  char selKind = 't';
  if (pendingSelId_) {
    selId = pendingSelId_;
    selKind = pendingSelKind_;
  } else if (keepSel && sel_ && sel_->type == Row::Task) {
    selId = sel_->item.id;
    selKind = sel_->item.kind;
  }
  int oldIdx = sel_ ? indexOf(sel_) : -1;
  pendingSelId_ = 0;
  pendingReload_ = false;

  fetchItems();
  rebuildRows();
  updateHeader();

  sel_ = nullptr;
  Row* found = nullptr;
  if (selId)
    for (auto& r : rows_)
      if (r->type == Row::Task && r->item.id == selId && r->item.kind == selKind) found = r.get();
  if (!found && keepSel && oldIdx >= 0) {
    Row* r = nullptr;
    for (int i = std::min<int>(oldIdx, (int)rows_.size() - 1); i >= 0 && !r; --i) if (rows_[i]->selectable()) r = rows_[i].get();
    if (!r) r = nextSelectable(-1, +1);
    found = r;
  }
  if (!found) found = nextSelectable(-1, +1);
  if (found) select(found, selId != 0 || keepSel);
  refreshCounts();
  justAddedId_ = justRestoredId_ = 0;
}

void Gui::rebuildRows() {
  for (auto& r : rows_) {
    if (r->tickId && r->check) gtk_widget_remove_tick_callback(r->check, r->tickId);
    r->tickId = 0;
    if (r->timer) g_source_remove(r->timer);
    r->timer = 0;
  }
  rows_.clear();
  sel_ = nullptr;
  emptyState_ = nullptr;
  loggedHidden_ = 0;
  clearChildren(listBox_);

  auto pushSection = [&](const std::string& text, char kind, int id) {
    auto hdr = std::make_unique<Row>();
    hdr->gui = this;
    hdr->type = Row::Section;
    hdr->text = text;
    hdr->groupKind = kind;
    hdr->groupId = id;
    gtk_box_append(GTK_BOX(listBox_), buildSimpleRow(*hdr));
    rows_.push_back(std::move(hdr));
  };

  bool grp = groupActive();
  Ord ord;
  if (grp) ord = buildOrd(s_);
  int lastKey = INT_MIN, lastAreaHeader = 0;
  std::string lastSection;
  for (auto& it : items_) {
    auto row = std::make_unique<Row>();
    row->gui = this;
    row->item = it;
    if (it.kind == 'h') {
      row->type = Row::Heading;
      row->text = it.title;
      lastSection.clear();
    } else {
      if (grp) {
        int k = ordKey(ord, it);
        if (k != lastKey) {
          lastKey = k;
          if (k < 0) {
            lastAreaHeader = 0;
          } else {
            const auto& inf = ord.info[k];
            if (inf.kind == 'a') {
              pushSection(inf.name, 'a', inf.id);
              lastAreaHeader = inf.id;
            } else {
              if (inf.areaId && inf.areaId != lastAreaHeader) pushSection(inf.areaName, 'a', inf.areaId);
              lastAreaHeader = inf.areaId;
              pushSection(inf.name, 'p', inf.id);
            }
          }
        }
      } else {
        std::string sec = sectionLabel(it);
        if (!sec.empty() && sec != lastSection) {
          pushSection(sec, 0, 0);
          lastSection = sec;
        }
      }
      row->type = Row::Task;
      row->done = it.status != "open";
    }
    if (row->type == Row::Heading) gtk_box_append(GTK_BOX(listBox_), buildSimpleRow(*row));
    else buildTaskRow(*row, false);
    rows_.push_back(std::move(row));
  }

  // the logged to-dos of a project: a quiet toggle, and under it a plain timeline, newest check-off first
  if (view_.kind == View::Project && !logged_.empty()) {
    auto tg = std::make_unique<Row>();
    tg->gui = this;
    tg->type = Row::LogToggle;
    bool open = loggedExpanded();
    tg->text = std::string(open ? "Hide " : "Show ") + std::to_string(logged_.size()) + (logged_.size() == 1 ? " logged to-do" : " logged to-dos");
    gtk_box_append(GTK_BOX(listBox_), buildSimpleRow(*tg));
    rows_.push_back(std::move(tg));
    if (open) {
      size_t shown = 0;
      for (auto& it : logged_) {
        if (shown >= limit_) {
          loggedHidden_ = logged_.size() - shown;
          break;
        }
        auto row = std::make_unique<Row>();
        row->gui = this;
        row->item = it;
        row->type = Row::Task;
        row->done = true;
        row->logged = true;
        buildTaskRow(*row, false);
        rows_.push_back(std::move(row));
        ++shown;
      }
    }
  }
  if (hiddenCount_ + loggedHidden_) {
    auto more = std::make_unique<Row>();
    more->gui = this;
    more->type = Row::More;
    more->text = "Show " + std::to_string(hiddenCount_ + loggedHidden_) + " more…";
    gtk_box_append(GTK_BOX(listBox_), buildSimpleRow(*more));
    rows_.push_back(std::move(more));
  }
  addSectionClasses();
  if (items_.empty() && logged_.empty()) buildEmptyState();
}

void Gui::addSectionClasses() {
  bool first = true;
  for (auto& r : rows_) {
    if (r->type != Row::Section && r->type != Row::Heading) continue;
    if (first && r.get() == rows_.front().get()) addClass(r->box, "first");
    first = false;
  }
}

// Section / heading / "show more" rows: plain labels inside a revealer (so they can collapse like task rows).
GtkWidget* Gui::buildGroupHeader(Row& r) {
  bool area = r.groupKind == 'a';
  GtkWidget* h = hbox(8);
  addClass(h, area ? "group-area" : "group-proj");
  GtkWidget* ic = area ? iconWidget(Icon::Area, 16, 0, false) : iconWidget(Icon::Project, 15, projectFraction(s_, r.groupId), false);
  iconRole(ic, area ? Role::Fg2 : Role::Fg3);
  gtk_box_append(GTK_BOX(h), ic);
  GtkWidget* l = label(r.text);
  gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(h), l);
  return h;
}

GtkWidget* Gui::buildLogToggle(Row& r) {
  GtkWidget* h = hbox(8);
  addClass(h, "log-toggle");
  gtk_box_append(GTK_BOX(h), label(r.text, "log-toggle-label"));
  return h;
}

GtkWidget* Gui::buildSimpleRow(Row& r) {
  GtkWidget* l;
  if (r.type == Row::Section && r.groupKind) l = buildGroupHeader(r);
  else if (r.type == Row::LogToggle) l = buildLogToggle(r);
  else {
    l = label(r.text, r.type == Row::More ? "more-row" : "section");
    if (r.type == Row::Heading) addClass(l, "heading");
    if (r.type == Row::More) { gtk_widget_set_margin_start(l, 28); gtk_widget_set_margin_top(l, 8); gtk_widget_set_margin_bottom(l, 8); }
  }
  r.box = l;
  GtkWidget* rev = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_transition_duration(GTK_REVEALER(rev), 220);
  gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
  gtk_revealer_set_child(GTK_REVEALER(rev), l);
  r.revealer = rev;
  if (r.type != Row::Section) {
    Row* rp = &r;
    onClick(l, [this, rp] {
      if (editor_ && !finishEditor(true)) return;
      select(rp, false);
      // the list is rebuilt by these two, so let the click finish before its own row goes away
      if (rp->type == Row::More || rp->type == Row::LogToggle) later([this] { activateSel(); });
    });
  }
  return rev;
}

void Gui::buildEmptyState() {
  emptyState_ = vbox(6);
  gtk_widget_set_margin_top(emptyState_, 70);
  gtk_widget_set_halign(emptyState_, GTK_ALIGN_CENTER);
  std::string t = "Nothing here", s = "Press N to add a to-do.";
  Icon ic = Icon::Checklist;
  const auto& n = view_.name;
  if (view_.kind == View::Smart) {
    ic = smartIcon(n);
    if (n == "Inbox") { t = "Inbox zero"; s = "Capture anything with N. Sort it out later."; }
    else if (n == "Today") { t = "A clear day"; s = "Nothing is due. Press N to plan something."; }
    else if (n == "Upcoming") { t = "Nothing scheduled"; s = "To-dos with a future date show up here."; }
    else if (n == "Anytime") { t = "Nothing to do"; s = "Every open to-do appears here."; }
    else if (n == "Someday") { t = "No someday items"; s = "Park ideas here with S → \"someday\"."; }
    else if (n == "Logbook") { t = "Nothing logged yet"; s = "Completed to-dos collect here."; }
    else { t = "Nothing here"; s = ""; }
  } else if (view_.kind == View::Project) { t = "An empty project"; s = "Press N to add the first to-do, Shift+N for a heading."; ic = Icon::Project; }
  else { t = "An empty area"; s = "Press N to add a to-do to this area."; ic = Icon::Area; }
  GtkWidget* i = iconWidget(ic, 46, 0, false);
  iconRole(i, Role::Fg3Faint);
  gtk_box_append(GTK_BOX(emptyState_), i);
  gtk_widget_set_margin_bottom(i, 8);
  gtk_box_append(GTK_BOX(emptyState_), label(t, "page-empty-title", 0.5f));
  if (!s.empty()) gtk_box_append(GTK_BOX(emptyState_), label(s, "page-empty", 0.5f));
  gtk_box_append(GTK_BOX(listBox_), emptyState_);
}

void Gui::updateHeader() {
  std::string title = view_.title, crumb, notes;
  Icon ic = view_.kind == View::Smart ? smartIcon(view_.name) : (view_.kind == View::Area ? Icon::Area : Icon::Project);
  double param = 0;
  if (view_.kind == View::Project) {
    Item p = s_.getProject(view_.id);
    title = p.title.empty() ? view_.title : p.title;
    crumb = p.areaName;
    notes = p.notes;
    auto all = s_.viewProject(view_.id, "", false);
    int done = 0;
    for (auto& t : all) if (t.status == "done") ++done;
    param = all.empty() ? 0 : (double)done / all.size();
    if (!s_.projectIsOpen(view_.id)) crumb = (crumb.empty() ? "" : crumb + "  ·  ") + "Logged";
  }
  if (groupActive()) crumb = "Grouped by area and project  ·  \xe2\x87\xa7" "A to turn off";
  gtk_label_set_text(GTK_LABEL(titleLabel_), title.c_str());
  gtk_label_set_text(GTK_LABEL(crumbLabel_), crumb.c_str());
  gtk_widget_set_visible(crumbLabel_, !crumb.empty());
  if (notes != notesShown_ || (!notes.empty() && !gtk_widget_get_first_child(notesBox_))) {  // keep the view if nothing changed
    notesShown_ = notes;
    clearChildren(notesBox_);
    if (!notes.empty()) {
      GtkWidget* l = label("", "page-notes");
      gtk_label_set_markup(GTK_LABEL(l), md::toPango(notes).c_str());
      gtk_label_set_wrap(GTK_LABEL(l), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_xalign(GTK_LABEL(l), 0);
      g_signal_connect(l, "activate-link", G_CALLBACK(+[](GtkLabel*, const char* uri, gpointer) -> gboolean {
        if (md::gOpenLink) md::gOpenLink(uri);
        return TRUE;
      }), nullptr);
      gtk_box_append(GTK_BOX(notesBox_), l);
    }
  }
  gtk_widget_set_visible(notesBox_, !notes.empty());
  iconSet(headerIcon_, ic, param);
  iconRole(headerIcon_, view_.kind == View::Smart ? Role::Identity : Role::Fg2);
  gtk_window_set_title(GTK_WINDOW(win_), ("Stride — " + title).c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------------------------------------------

namespace {
void revealLater(GtkWidget* rev) {
  g_object_ref(rev);
  g_timeout_add(24, +[](gpointer p) -> gboolean {
    auto* w = GTK_WIDGET(p);
    if (GTK_IS_REVEALER(w)) gtk_revealer_set_reveal_child(GTK_REVEALER(w), TRUE);
    g_object_unref(w);
    return G_SOURCE_REMOVE;
  }, rev);
}
double strikeProgress(const Row* r) {
  if (r->animating) {
    return r->animToDone ? ease::outCubic((r->animT - 120) / 280.0) : 1 - ease::outQuad(r->animT / 170.0);
  }
  return r->pendingRemoval ? 1 : 0;
}
}  // namespace

void Gui::buildTaskRow(Row& r, bool animateIn) {
  const Item& it = r.item;
  bool isProject = it.kind == 'p';
  bool fresh = animateIn || (it.kind == 't' && it.id == justAddedId_) || (it.id == justRestoredId_ && justRestoredId_ != 0);

  GtkWidget* box = hbox(12);
  addClass(box, "task-row");
  if (it.someday) addClass(box, "someday");
  if (isProject) addClass(box, "project");
  if (r.logged) addClass(box, "logged");
  if (r.done) { addClass(box, "done"); r.doneClass = true; }  // finished items read grey
  if (isProject && !r.done) r.pie = projectFraction(s_, it.id);
  r.box = box;

  // -- checkbox --
  GtkWidget* check = gtk_drawing_area_new();
  gtk_widget_set_size_request(check, 26, 26);
  gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
  r.check = check;
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(check), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer ud) {
    auto* rr = static_cast<Row*>(ud);
    auto pose = draw::checkPose(rr->done, rr->animT, rr->animating, rr->animToDone);
    pose.muted = rr->done && !rr->animating && !rr->pendingRemoval;  // finished and settled: grey, not accent
    draw::checkbox(cr, w, h, pose, rr->hot || rr->gui->sel_ == rr,
                   rr->item.kind == 't' ? draw::Shape::Square : draw::Shape::Circle, rr->pie);
  }, &r, nullptr);
  GtkEventController* mc = gtk_event_controller_motion_new();
  g_signal_connect(mc, "enter", G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer p) {
    auto* rr = static_cast<Row*>(p); rr->hot = true; gtk_widget_queue_draw(rr->check);
  }), &r);
  g_signal_connect(mc, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer p) {
    auto* rr = static_cast<Row*>(p); rr->hot = false; gtk_widget_queue_draw(rr->check);
  }), &r);
  gtk_widget_add_controller(check, mc);
  Row* rp = &r;
  onClick(check, [this, rp] { select(rp, false); toggleRow(rp); });
  gtk_box_append(GTK_BOX(box), check);

  // -- title (+ the strike-through that draws itself across it) and context line --
  GtkWidget* mid = vbox(1);
  gtk_widget_set_hexpand(mid, TRUE);
  gtk_widget_set_valign(mid, GTK_ALIGN_CENTER);
  GtkWidget* ov = gtk_overlay_new();
  GtkWidget* title = label(it.title, "task-title");
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(title), TRUE);
  r.title = title;
  gtk_overlay_set_child(GTK_OVERLAY(ov), title);
  GtkWidget* strike = gtk_drawing_area_new();
  gtk_widget_set_can_target(strike, FALSE);
  r.strike = strike;
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(strike), +[](GtkDrawingArea*, cairo_t* cr, int, int, gpointer ud) {
    auto* rr = static_cast<Row*>(ud);
    double p = strikeProgress(rr);
    if (p <= 0.001) return;
    int tw = 0, th = 0, ox = 0, oy = 0;
    PangoLayout* lay = gtk_label_get_layout(GTK_LABEL(rr->title));
    pango_layout_get_pixel_size(lay, &tw, &th);
    gtk_label_get_layout_offsets(GTK_LABEL(rr->title), &ox, &oy);
    double y = std::round(oy + th * 0.56) + 0.5;
    cairo_set_line_width(cr, 1.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    setSource(cr, gPal.fg3);
    cairo_move_to(cr, ox, y);
    cairo_line_to(cr, ox + tw * ease::clamp01(p), y);
    cairo_stroke(cr);
  }, &r, nullptr);
  gtk_overlay_add_overlay(GTK_OVERLAY(ov), strike);
  gtk_box_append(GTK_BOX(mid), ov);

  std::string ctx;
  bool showCtx = view_.kind == View::Smart && view_.name != "Inbox" && !groupActive();
  if (showCtx) ctx = isProject ? it.areaName : (!it.projectName.empty() ? it.projectName : it.areaName);
  if (!ctx.empty()) gtk_box_append(GTK_BOX(mid), label(ctx, "task-sub"));
  gtk_box_append(GTK_BOX(box), mid);

  // -- meta: tags, checklist progress, notes, do date, deadline --
  GtkWidget* meta = hbox(11);
  gtk_widget_set_valign(meta, GTK_ALIGN_CENTER);
  int shownTags = 0;
  for (auto& t : splitComma(it.tags)) {
    std::string v = trimmed(t);
    if (v.empty() || shownTags >= 3) continue;
    gtk_box_append(GTK_BOX(meta), label(v, "chip"));
    ++shownTags;
  }
  auto cl = parseChecklist(it.checklist);
  if (!cl.empty()) {
    int dn = 0;
    for (auto& c : cl) dn += c.done ? 1 : 0;
    GtkWidget* g = hbox(4);
    GtkWidget* i = iconWidget(Icon::Checklist, 13, 0, false);
    iconRole(i, Role::Fg3);
    gtk_box_append(GTK_BOX(g), i);
    gtk_box_append(GTK_BOX(g), label(std::to_string(dn) + "/" + std::to_string(cl.size()), "meta faint"));
    gtk_box_append(GTK_BOX(meta), g);
  }
  if (!it.notes.empty() && !isProject) {
    GtkWidget* i = iconWidget(Icon::Note, 13, 0, false);
    iconRole(i, Role::Fg3);
    gtk_box_append(GTK_BOX(meta), i);
  }
  std::string dd;
  if (it.someday || it.doDate == "someday") { if (view_.name != "Someday") dd = "Someday"; }
  else if (!it.doDate.empty()) {
    bool hide = view_.isLogLike() || r.logged || view_.name == "Upcoming" || (view_.name == "Today" && it.doDate == today()) ||
                (view_.name == "Tomorrow" && it.doDate == todayPlus(1));
    if (!hide) dd = guidates::friendly(it.doDate);
  }
  if (!dd.empty()) {
    GtkWidget* g = hbox(4);
    GtkWidget* i = iconWidget(Icon::Calendar, 13, 0, false);
    iconRole(i, Role::Fg3);
    gtk_box_append(GTK_BOX(g), i);
    gtk_box_append(GTK_BOX(g), label(dd, "meta"));
    gtk_box_append(GTK_BOX(meta), g);
  }
  if (!it.deadline.empty() && !view_.isLogLike() && !r.logged) {
    bool due = false;
    std::string txt = deadlineLabel(it.deadline, due);
    GtkWidget* g = hbox(4);
    GtkWidget* i = iconWidget(Icon::Flag, 13, 0, false);
    iconRole(i, due ? Role::Red : Role::Fg3);
    gtk_box_append(GTK_BOX(g), i);
    GtkWidget* l = label(txt, "meta");
    if (due) addClass(l, "due");
    gtk_box_append(GTK_BOX(g), l);
    gtk_box_append(GTK_BOX(meta), g);
  }
  if (it.status != "open" || r.logged) {  // when it was checked off
    std::string when = completedLabel(it);
    if (!when.empty()) gtk_box_append(GTK_BOX(meta), label(when, "meta logged"));
  }
  gtk_box_append(GTK_BOX(box), meta);

  // -- mouse: click selects, double click edits --
  GtkGesture* g = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
  g_signal_connect(g, "pressed", G_CALLBACK(+[](GtkGestureClick* gc, int n, double, double, gpointer ud) {
    auto* rr = static_cast<Row*>(ud);
    Gui* self = rr->gui;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gc));
    if (self->editor_ && !self->finishEditor(true)) return;
    // finishing an edit reloads the list; find this row again by identity
    Row* target = nullptr;
    for (auto& x : self->rows_) if (x->item.id == rr->item.id && x->item.kind == rr->item.kind && x->type == rr->type) target = x.get();
    if (!target) return;
    self->select(target, false);
    if (btn == GDK_BUTTON_PRIMARY && n == 2) self->activateSel();
  }), &r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(g));

  // -- revealer wrapper: new rows grow in, completed rows shrink away --
  GtkWidget* rev = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_transition_duration(GTK_REVEALER(rev), 240);
  gtk_revealer_set_child(GTK_REVEALER(rev), box);
  r.revealer = rev;
  if (fresh) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
    addClass(box, r.item.id == justRestoredId_ ? "row-back" : "row-new");
    revealLater(rev);
  } else {
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
  }
  g_signal_connect(rev, "notify::child-revealed", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer ud) {
    auto* self = static_cast<Gui*>(ud);
    GtkWidget* w = GTK_WIDGET(o);
    if (gtk_revealer_get_child_revealed(GTK_REVEALER(w))) return;
    // collapsed fully: drop the row (deferred -- we're inside the revealer's own notify)
    g_object_ref(w);
    later([self, w] {
      for (auto& r : self->rows_)
        if (r->revealer == w && r->leaving) { self->dropRow(r.get()); break; }
      g_object_unref(w);
    });
  }), this);
  gtk_box_append(GTK_BOX(listBox_), rev);
}

int Gui::indexOf(const Row* r) const {
  for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i].get() == r) return (int)i;
  return -1;
}

Row* Gui::nextSelectable(int from, int dir) const {
  for (int i = from + dir; i >= 0 && i < (int)rows_.size(); i += dir) {
    Row* r = rows_[i].get();
    if (r->selectable() && !r->pendingRemoval && !r->leaving) return r;
  }
  return nullptr;
}

void Gui::select(Row* r, bool scroll) {
  if (sel_ == r) { if (scroll) scrollToSel(); return; }
  Row* old = sel_;
  sel_ = r;
  if (old && old->box) { rmClass(old->box, "selected"); if (old->check) gtk_widget_queue_draw(old->check); }
  if (r && r->box) { addClass(r->box, "selected"); if (r->check) gtk_widget_queue_draw(r->check); }
  if (scroll) scrollToSel();
}

void Gui::scrollToSel() {
  later([this] {
    if (!sel_ || !sel_->revealer) return;
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(sel_->revealer, content_, &b)) return;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(mainScroll_));
    double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj), margin = 44;
    double top = b.origin.y, bottom = top + b.size.height, target = v;
    if (indexOf(sel_) == indexOf(nextSelectable(-1, +1))) target = 0;  // first row: show the title again
    else if (top - margin < v) target = top - margin;
    else if (bottom + margin + 70 > v + page) target = bottom + margin + 70 - page;
    else return;
    scroller_.go(mainScroll_, target);
  });
}

void Gui::moveSel(int dir) {
  if (!sel_) {
    Row* r = dir > 0 ? nextSelectable(-1, +1) : nextSelectable((int)rows_.size(), -1);
    if (r) select(r);
    return;
  }
  if (Row* n = nextSelectable(indexOf(sel_), dir)) select(n);
}

// ---------------------------------------------------------------------------------------------------------------
// completing, undoing, dropping
// ---------------------------------------------------------------------------------------------------------------

void Gui::animateRow(Row* r, bool toDone) {
  r->animToDone = toDone;
  r->animating = true;
  r->t0 = 0;
  r->animT = 0;
  if (r->tickId) return;  // the running tick restarts from the new t0
  r->tickId = gtk_widget_add_tick_callback(r->check, +[](GtkWidget*, GdkFrameClock* fc, gpointer ud) -> gboolean {
    auto* rr = static_cast<Row*>(ud);
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!rr->t0) rr->t0 = now;
    rr->animT = (now - rr->t0) / 1000.0;
    if (rr->animToDone && !rr->doneClass && rr->animT > 140) { addClass(rr->box, "done"); rr->doneClass = true; }
    if (!rr->animToDone && rr->doneClass) { rmClass(rr->box, "done"); rr->doneClass = false; }
    gtk_widget_queue_draw(rr->check);
    gtk_widget_queue_draw(rr->strike);
    double total = rr->animToDone ? draw::kCompleteMs : draw::kReopenMs;
    if (rr->animT >= total) {
      rr->animating = false;
      rr->tickId = 0;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }, r, nullptr);
}

void Gui::scheduleCollapse(Row* r, guint delayMs) {
  if (r->timer) g_source_remove(r->timer);
  r->timer = g_timeout_add(delayMs, +[](gpointer p) -> gboolean {
    auto* rr = static_cast<Row*>(p);
    rr->timer = 0;
    rr->gui->collapseRow(rr);
    return G_SOURCE_REMOVE;
  }, r);
}

void Gui::collapseRow(Row* r) {
  r->leaving = true;
  addClass(r->box, "leaving");
  gtk_revealer_set_transition_type(GTK_REVEALER(r->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_transition_duration(GTK_REVEALER(r->revealer), 300);
  gtk_revealer_set_reveal_child(GTK_REVEALER(r->revealer), FALSE);
}

void Gui::dropRow(Row* r) {
  int idx = indexOf(r);
  if (idx < 0) return;
  bool wasSel = sel_ == r;
  Row* next = nullptr;
  if (wasSel) {
    next = nextSelectable(idx, +1);
    if (!next) next = nextSelectable(idx, -1);
    sel_ = nullptr;
  }
  gtk_box_remove(GTK_BOX(listBox_), r->revealer);
  rows_.erase(rows_.begin() + idx);
  pruneEmptySections();
  if (wasSel && next) select(next);
  bool any = false;
  for (auto& x : rows_) any = any || x->type == Row::Task || x->type == Row::More || x->type == Row::LogToggle;
  bool anyHeading = false;
  for (auto& x : rows_) anyHeading = anyHeading || x->type == Row::Heading;
  if (!any && !anyHeading && !emptyState_) buildEmptyState();
  refreshCounts();
  if (view_.kind == View::Project) {  // once everything has settled, let the finished to-dos join the logged timeline
    bool busy = false;
    for (auto& x : rows_) busy = busy || x->pendingRemoval || x->leaving || x->animating;
    if (!busy) later([this] { if (!editor_) reload(true); });
  }
}

void Gui::pruneEmptySections() {
  for (int i = (int)rows_.size() - 1; i >= 0; --i) {  // backwards, so an emptied project header takes its area with it
    Row* r = rows_[i].get();
    if (r->type != Row::Section) continue;
    Row* n = i + 1 < (int)rows_.size() ? rows_[i + 1].get() : nullptr;
    bool orphan = !n || n->type == Row::Heading || n->type == Row::LogToggle ||
                  (n->type == Row::Section && n->level() <= r->level());
    if (orphan) {
      gtk_box_remove(GTK_BOX(listBox_), r->revealer);
      rows_.erase(rows_.begin() + i);
    }
  }
}

void Gui::toggleRow(Row* r) {
  if (!r || r->type != Row::Task || r->leaving) return;
  if (editor_) return;
  Item& it = r->item;
  if (!r->done) {
    s_.complete(it);
    undo_.push_back({it.id, it.kind});
    r->done = true;
    r->pendingRemoval = true;
    animateRow(r, true);
    scheduleCollapse(r, 980);
    if (sel_ == r) {  // keep the cursor moving, so a run of x / Space sweeps down the list
      Row* n = nextSelectable(indexOf(r), +1);
      if (!n) n = nextSelectable(indexOf(r), -1);
      if (n) select(n);
    }
    toast(it.kind == 'p' ? "Project completed" : "Completed", "Z", false, 2600);
    markDirty();
    refreshCounts();
  } else {
    if (r->logged) {
      s_.reopen(it);
      markDirty();
      justRestoredId_ = it.id;
      pendingSelId_ = it.id;
      pendingSelKind_ = it.kind;
      toast("Reopened");
      refreshSidebar();
      reload(true);
      return;
    }
    s_.reopen(it);
    bool wasPending = r->pendingRemoval;
    r->done = false;
    r->pendingRemoval = false;
    if (r->timer) { g_source_remove(r->timer); r->timer = 0; }
    r->leaving = false;
    rmClass(r->box, "leaving");
    animateRow(r, false);
    if (!wasPending) scheduleCollapse(r, 620);  // reopened from a log view: it no longer belongs here
    if (!undo_.empty() && undo_.back().id == it.id && undo_.back().kind == it.kind) undo_.pop_back();
    toast("Reopened");
    markDirty();
    refreshCounts();
  }
}

void Gui::undo() {
  if (undo_.empty()) { toast("Nothing to undo"); return; }
  Undo u = undo_.back();
  undo_.pop_back();
  for (auto& r : rows_)
    if (r->type == Row::Task && r->item.id == u.id && r->item.kind == u.kind && r->done && !r->leaving) {
      toggleRow(r.get());  // still lingering on screen: just reverse it in place
      select(r.get());
      return;
    }
  Item it;
  it.id = u.id;
  it.kind = u.kind;
  s_.reopen(it);
  justRestoredId_ = u.id;
  pendingSelId_ = u.id;
  pendingSelKind_ = u.kind;
  markDirty();
  if (u.kind != 't') refreshSidebar();
  reload(true);
  toast("Restored");
}

Item Gui::forSave(Item it) {
  if (it.kind == 't' && it.someday && it.doDate.empty()) it.doDate = "someday";  // Store::saveTask reads the sentinel
  return it;
}

void Gui::saveItem(Item it) {
  if (it.kind == 'p') s_.saveProject(it, it.areaId);
  else s_.saveTask(forSave(it), it.areaId, it.projectId);
  markDirty();
  pendingSelId_ = it.id;
  pendingSelKind_ = it.kind;
  reload(true);
}

void Gui::activateSel() {
  if (!sel_) return;
  Row* r = sel_;
  if (r->type == Row::More) {
    limit_ += 400;
    reload(true);
    return;
  }
  if (r->type == Row::LogToggle) {
    toggleLogged();
    return;
  }
  if (r->type == Row::Heading) {
    int id = r->item.id;
    openPrompt("Rename heading", "Heading", r->item.title, nullptr, [this, id](const std::string& v) {
      s_.renameHeading(id, trimmed(v));
      markDirty();
      reload(true);
      return true;
    });
    return;
  }
  if (r->item.kind == 'p') {
    goTo(View{View::Project, "", r->item.id, r->item.title});
  } else if (r->item.kind == 'a') {
    goTo(View{View::Area, "", r->item.id, r->item.title});
  } else {
    openEditor(r);
  }
}

void Gui::promptDate(bool deadline) {
  if (!sel_ || sel_->type != Row::Task || (sel_->item.kind != 't' && sel_->item.kind != 'p')) return;
  Item it = sel_->item;
  std::string cur = deadline ? it.deadline : (it.someday ? "someday" : it.doDate);
  openPrompt(deadline ? "Deadline" : "When", deadline ? "today, fri, +3d, 2026-12-24 — empty clears" : "today, tomorrow, fri, +3d, someday — empty clears", cur,
             [deadline](const std::string& t, bool& ok) {
               auto p = guidates::parse(t, !deadline);
               ok = p.ok;
               return p.ok ? "→ " + p.label : "Couldn't read “" + trimmed(t) + "” as a date";
             },
             [this, it, deadline](const std::string& t) mutable {
               auto p = guidates::parse(t, !deadline);
               if (!p.ok) return false;
               if (deadline) it.deadline = p.value;
               else { it.doDate = p.value; it.someday = p.value == "someday"; }
               saveItem(it);
               return true;
             });
}

void Gui::promptTags() {
  if (!sel_ || sel_->type != Row::Task || sel_->item.kind != 't') return;
  Item it = sel_->item;
  std::string existing;
  for (auto& t : s_.allTags()) existing += (existing.empty() ? "" : ", ") + t;
  std::string cur;
  for (auto& t : splitComma(it.tags)) cur += (cur.empty() ? "" : ", ") + trimmed(t);
  openPrompt("Tags", "comma, separated", cur,
             [existing](const std::string&, bool& ok) { ok = true; return existing.empty() ? "" : "Existing: " + existing; },
             [this, it](const std::string& t) mutable {
               it.tags = joinTags(t);
               saveItem(it);
               return true;
             });
}

std::vector<PalItem> Gui::listPickItems(const std::function<void(int, int)>& pick) {
  std::vector<PalItem> out;
  PalItem inbox{"Inbox", "No list", "", Icon::Inbox, 0, false, [pick] { pick(0, 0); }};
  out.push_back(inbox);
  for (auto& a : s_.areas()) {
    int id = a.id;
    out.push_back({a.name, "", "Area", Icon::Area, 0, false, [pick, id] { pick(id, 0); }});
  }
  for (auto& p : s_.projects()) {
    int id = p.id, area = s_.projectAreaId(p.id);
    out.push_back({p.name, p.sub, "Project", Icon::Project, 0, false, [pick, id, area] { pick(area, id); }});
  }
  return out;
}

void Gui::pickList() {
  if (!sel_ || sel_->type != Row::Task || sel_->item.kind != 't') return;
  int id = sel_->item.id;
  openPalette("Move to…", listPickItems([this, id](int area, int project) {
    s_.moveTask(id, area, project);
    markDirty();
    pendingSelId_ = id;
    pendingSelKind_ = 't';
    reload(true);
    refreshSidebar();
    toast("Moved");
  }));
}

void Gui::confirmDelete() {
  if (!sel_ || sel_->type == Row::More || sel_->type == Row::Section || sel_->type == Row::LogToggle) return;
  Item it = sel_->item;
  bool heading = sel_->type == Row::Heading;
  std::string what = heading ? "heading" : (it.kind == 'p' ? "project" : "to-do");
  openConfirm("Delete this " + what + "?", "“" + it.title + "” will be removed permanently." +
                  (heading ? " Its to-dos stay in the project." : ""), "Delete", true, [this, it, heading] {
    if (heading) s_.deleteHeading(it.id);
    else s_.erase(it);
    markDirty();
    reload(true);
    refreshSidebar();
    toast("Deleted");
  });
}

void Gui::reorder(int dir) {
  if (!sel_ || sel_->type != Row::Task || sel_->logged) return;
  Row* n = nextSelectable(indexOf(sel_), dir);
  if (!n || n->type != Row::Task || n->logged || n->item.kind != sel_->item.kind) { toast("Can't move further"); return; }
  s_.swapOrder(sel_->item, n->item);
  markDirty();
  pendingSelId_ = sel_->item.id;
  pendingSelKind_ = sel_->item.kind;
  reload(true);
}

// ---------------------------------------------------------------------------------------------------------------
// the inline editor (a card that opens in place, like Things)
// ---------------------------------------------------------------------------------------------------------------

ListRef Gui::resolveList(const std::string& raw) {
  ListRef r;
  std::string q = lower(trimmed(raw));
  if (q.empty() || q == "inbox" || q == "none") { r.found = r.inbox = true; r.label = "Inbox"; return r; }
  auto projects = s_.projects();
  auto areas = s_.areas();
  for (auto& p : projects)
    if (lower(p.name) == q) { r = {true, false, s_.projectAreaId(p.id), p.id, p.name, "Project"}; return r; }
  for (auto& a : areas)
    if (lower(a.name) == q) { r = {true, false, a.id, 0, a.name, "Area"}; return r; }
  // unique substring match across projects then areas ("reno" -> "Home Renovation")
  std::vector<ListRef> hits;
  for (auto& p : projects)
    if (lower(p.name).find(q) != std::string::npos) hits.push_back({true, false, s_.projectAreaId(p.id), p.id, p.name, "Project"});
  for (auto& a : areas)
    if (lower(a.name).find(q) != std::string::npos) hits.push_back({true, false, a.id, 0, a.name, "Area"});
  if (hits.size() == 1) return hits[0];
  return r;
}

GtkWidget* Gui::editorField(const char* cap, GtkWidget** entry, const std::string& text, const char* placeholder, GtkWidget** fieldBox) {
  GtkWidget* col = vbox(3);
  gtk_widget_set_hexpand(col, TRUE);
  gtk_box_append(GTK_BOX(col), label(cap, "field-cap"));
  GtkWidget* f = hbox(0);
  addClass(f, "field");
  GtkWidget* e = gtk_entry_new();
  addClass(e, "bare");
  gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
  gtk_editable_set_text(GTK_EDITABLE(e), text.c_str());
  gtk_widget_set_hexpand(e, TRUE);
  gtk_box_append(GTK_BOX(f), e);
  gtk_box_append(GTK_BOX(col), f);
  *entry = e;
  if (fieldBox) *fieldBox = f;
  return col;
}

void Gui::startEditor(std::unique_ptr<Editor> ep, GtkWidget* after, const std::string& whenInit, const std::string& listInit) {
  editor_ = std::move(ep);
  Editor& e = *editor_;
  const Item& it = e.orig;

  GtkWidget* card = vbox(8);
  addClass(card, "editor");
  e.card = card;

  GtkWidget* titleRow = hbox(12);
  GtkWidget* cb = gtk_drawing_area_new();
  gtk_widget_set_size_request(cb, 26, 26);
  gtk_widget_set_valign(cb, GTK_ALIGN_START);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(cb), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    draw::checkbox(cr, w, h, draw::checkPose(false, 0, false, true), false, draw::Shape::Square);
  }, nullptr, nullptr);
  gtk_box_append(GTK_BOX(titleRow), cb);
  e.title = gtk_entry_new();
  addClass(e.title, "bare");
  addClass(e.title, "editor-title");
  gtk_entry_set_placeholder_text(GTK_ENTRY(e.title), e.isNew ? "New to-do   (add @list #tag !when inline)" : "To-do");
  gtk_editable_set_text(GTK_EDITABLE(e.title), it.title.c_str());
  gtk_widget_set_hexpand(e.title, TRUE);
  gtk_widget_set_valign(e.title, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(titleRow), e.title);
  gtk_box_append(GTK_BOX(card), titleRow);

  GtkWidget* body = vbox(8);
  gtk_widget_set_margin_start(body, 38);
  GtkWidget* notesW = textArea(it.notes, "Notes", &e.notes);
  gtk_box_append(GTK_BOX(body), notesW);
  e.clBox = vbox(0);
  gtk_box_append(GTK_BOX(body), e.clBox);
  e.clAddRow = hbox(8);
  addClass(e.clAddRow, "cl-add");
  {
    GtkWidget* pi = iconWidget(Icon::Plus, 13, 0, true);
    iconRole(pi, Role::Fg3);
    gtk_widget_set_size_request(pi, 18, -1);
    gtk_box_append(GTK_BOX(e.clAddRow), pi);
    gtk_box_append(GTK_BOX(e.clAddRow), label("Add checklist item"));
    gtk_widget_set_tooltip_text(e.clAddRow, "Ctrl+L");
    onClick(e.clAddRow, [this] { if (editor_) addChecklistRow(-1, "", false, true); });
  }
  gtk_box_append(GTK_BOX(body), e.clAddRow);
  for (auto& c : parseChecklist(it.checklist)) addChecklistRow(-1, c.text, c.done, false);

  GtkWidget* grid = hbox(8);
  std::string tagsText;
  for (auto& t : splitComma(it.tags)) tagsText += (tagsText.empty() ? "" : ", ") + trimmed(t);
  std::string deadlineText = it.deadline;
  gtk_box_append(GTK_BOX(grid), editorField("WHEN", &e.when, whenInit, "today, fri, +3d, someday", &e.whenField));
  gtk_box_append(GTK_BOX(grid), editorField("DEADLINE", &e.deadline, deadlineText, "a hard date", &e.deadlineField));
  gtk_box_append(GTK_BOX(grid), editorField("TAGS", &e.tags, tagsText, "work, home", nullptr));
  gtk_box_append(GTK_BOX(grid), editorField("LIST", &e.list, listInit, "Inbox", &e.listField));
  gtk_box_append(GTK_BOX(body), grid);

  e.hint = label("", "hint");
  gtk_box_append(GTK_BOX(body), e.hint);
  gtk_box_append(GTK_BOX(card), body);

  for (GtkWidget* w : {e.title, e.when, e.deadline, e.tags, e.list})
    g_signal_connect_swapped(w, "changed", G_CALLBACK(+[](gpointer self) { static_cast<Gui*>(self)->updateEditorHint(); }), this);

  if (emptyState_) gtk_widget_set_visible(emptyState_, FALSE);
  if (after) {
    gtk_box_insert_child_after(GTK_BOX(listBox_), card, after);
  } else {
    gtk_box_prepend(GTK_BOX(listBox_), card);
  }
  updateEditorHint();
  gtk_widget_grab_focus(e.title);
  gtk_editable_set_position(GTK_EDITABLE(e.title), -1);
  later([this] {
    if (!editor_) return;
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(editor_->card, content_, &b)) return;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(mainScroll_));
    double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    double bottom = b.origin.y + b.size.height + 80;
    if (b.origin.y - 40 < v) scroller_.go(mainScroll_, b.origin.y - 40);
    else if (bottom > v + page) scroller_.go(mainScroll_, bottom - page);
  });
}

void Gui::toggleChecklistRow(ClRow* c) {
  c->done = !c->done;
  setClass(c->row, "done", c->done);
  c->from = c->fill;
  c->t0 = 0;
  if (c->tick) return;
  c->tick = gtk_widget_add_tick_callback(c->circle, +[](GtkWidget* w, GdkFrameClock* fc, gpointer ud) -> gboolean {
    auto* cr = static_cast<ClRow*>(ud);
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!cr->t0) cr->t0 = now;
    double t = (now - cr->t0) / 1000.0 / 190.0;
    cr->fill = ease::lerp(cr->from, cr->done ? 1.0 : 0.0, ease::outCubic(t));
    gtk_widget_queue_draw(w);
    if (t >= 1) {
      cr->tick = 0;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }, c, nullptr);
}

ClRow* Gui::addChecklistRow(int index, const std::string& text, bool done, bool focus) {
  Editor& e = *editor_;
  auto owned = std::make_unique<ClRow>();
  ClRow* c = owned.get();
  c->done = done;
  c->fill = done ? 1 : 0;
  c->row = hbox(8);
  addClass(c->row, "cl-row");
  if (done) addClass(c->row, "done");
  c->circle = gtk_drawing_area_new();
  gtk_widget_set_size_request(c->circle, 18, 24);
  gtk_widget_set_valign(c->circle, GTK_ALIGN_CENTER);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(c->circle), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer ud) {
    auto* x = static_cast<ClRow*>(ud);
    draw::checklistCircle(cr, w, h, x->fill, x->hot);
  }, c, nullptr);
  GtkEventController* mc = gtk_event_controller_motion_new();
  g_signal_connect(mc, "enter", G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer p) {
    auto* x = static_cast<ClRow*>(p); x->hot = true; gtk_widget_queue_draw(x->circle);
  }), c);
  g_signal_connect(mc, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer p) {
    auto* x = static_cast<ClRow*>(p); x->hot = false; gtk_widget_queue_draw(x->circle);
  }), c);
  gtk_widget_add_controller(c->circle, mc);
  onClick(c->circle, [this, c] { toggleChecklistRow(c); });
  gtk_box_append(GTK_BOX(c->row), c->circle);
  c->entry = gtk_entry_new();
  addClass(c->entry, "bare");
  gtk_entry_set_placeholder_text(GTK_ENTRY(c->entry), "Checklist item");
  gtk_editable_set_text(GTK_EDITABLE(c->entry), text.c_str());
  gtk_widget_set_hexpand(c->entry, TRUE);
  gtk_widget_set_valign(c->entry, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(c->row), c->entry);

  if (index < 0 || index > (int)e.cl.size()) index = (int)e.cl.size();
  if (index == 0) gtk_box_prepend(GTK_BOX(e.clBox), c->row);
  else gtk_box_insert_child_after(GTK_BOX(e.clBox), c->row, e.cl[index - 1]->row);
  e.cl.insert(e.cl.begin() + index, std::move(owned));
  if (focus) gtk_widget_grab_focus(c->entry);
  return c;
}

void Gui::removeChecklistRow(ClRow* c) {
  Editor& e = *editor_;
  for (size_t i = 0; i < e.cl.size(); ++i)
    if (e.cl[i].get() == c) {
      if (c->tick) gtk_widget_remove_tick_callback(c->circle, c->tick);
      c->tick = 0;
      gtk_box_remove(GTK_BOX(e.clBox), c->row);
      e.cl.erase(e.cl.begin() + i);
      return;
    }
}

// Keys that only mean something inside the open editor: the checklist, and Tab walking the fields.
bool Gui::editorKey(guint kv, GdkModifierType st) {
  Editor& e = *editor_;
  bool ctrl = st & GDK_CONTROL_MASK, shift = st & GDK_SHIFT_MASK;
  GtkWidget* f = gtk_window_get_focus(GTK_WINDOW(win_));
  auto within = [&](GtkWidget* w) { return f && (f == w || gtk_widget_is_ancestor(f, w)); };
  int cur = -1;
  for (size_t i = 0; i < e.cl.size(); ++i)
    if (within(e.cl[i]->entry)) cur = (int)i;

  if (ctrl && (kv == GDK_KEY_l || kv == GDK_KEY_L)) {
    addChecklistRow(cur >= 0 ? cur + 1 : -1, "", false, true);
    return true;
  }
  if (ctrl) return false;

  if (cur >= 0) {
    ClRow* c = e.cl[cur].get();
    bool empty = trimmed(entryText(c->entry)).empty();
    if ((kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) && !shift) {
      if (empty && cur + 1 == (int)e.cl.size()) {  // Enter on an empty last item ends the list
        removeChecklistRow(c);
        gtk_widget_grab_focus(e.when);
      } else {
        addChecklistRow(cur + 1, "", false, true);
      }
      return true;
    }
    if (kv == GDK_KEY_BackSpace && empty) {
      GtkWidget* to = cur > 0 ? e.cl[cur - 1]->entry : e.notes;
      removeChecklistRow(c);
      gtk_widget_grab_focus(to);
      if (GTK_IS_EDITABLE(to)) gtk_editable_set_position(GTK_EDITABLE(to), -1);
      return true;
    }
    if (kv == GDK_KEY_Up) {
      gtk_widget_grab_focus(cur > 0 ? e.cl[cur - 1]->entry : e.notes);
      return true;
    }
    if (kv == GDK_KEY_Down) {
      gtk_widget_grab_focus(cur + 1 < (int)e.cl.size() ? e.cl[cur + 1]->entry : e.when);
      return true;
    }
  }

  if (kv == GDK_KEY_Tab || kv == GDK_KEY_ISO_Left_Tab) {
    if (f == e.notes && md::wantsTab(e.notes)) return false;  // Tab nests a bullet
    bool back = kv == GDK_KEY_ISO_Left_Tab || shift;
    std::vector<GtkWidget*> stops = {e.title, e.notes};
    if (!e.cl.empty()) stops.push_back(e.cl.front()->entry);
    for (GtkWidget* w : {e.when, e.deadline, e.tags, e.list}) stops.push_back(w);
    int at = 0;
    for (size_t i = 0; i < stops.size(); ++i) {
      bool here = within(stops[i]) || (!e.cl.empty() && i == 2 && cur >= 0);
      if (here) at = (int)i;
    }
    int n = (int)stops.size();
    gtk_widget_grab_focus(stops[((at + (back ? -1 : 1)) % n + n) % n]);
    return true;
  }
  return false;
}

void Gui::openEditor(Row* r) {
  if (!r || r->type != Row::Task || r->item.kind != 't') return;
  auto e = std::make_unique<Editor>();
  e->isNew = false;
  e->orig = r->item;
  e->row = r;
  std::string when = r->item.someday || r->item.doDate == "someday" ? "someday" : r->item.doDate;
  std::string list = !r->item.projectName.empty() ? r->item.projectName : r->item.areaName;
  GtkWidget* rev = r->revealer;
  gtk_widget_set_visible(rev, FALSE);
  startEditor(std::move(e), rev, when, list);
}

void Gui::newTask() {
  if (editor_ && !finishEditor(true)) return;
  auto e = std::make_unique<Editor>();
  e->isNew = true;
  std::string when, list;
  if (view_.kind == View::Project) list = view_.title;
  else if (view_.kind == View::Area) list = view_.title;
  else if (view_.name == "Today") when = "today";
  else if (view_.name == "Tomorrow") when = "tomorrow";
  else if (view_.name == "Someday") when = "someday";
  GtkWidget* after = nullptr;
  if (sel_ && (sel_->type == Row::Task || sel_->type == Row::Heading) && !sel_->logged) {
    after = sel_->revealer;
    e->headingId = sel_->type == Row::Heading ? sel_->item.id : sel_->item.headingId;
    e->afterSort = sel_->type == Row::Task ? sel_->item.sortOrder : -1;
  } else if (!rows_.empty()) {
    Row* lr = lastOpenRow();
    after = lr ? lr->revealer : nullptr;
  }
  startEditor(std::move(e), after, when, list);
}

Row* Gui::lastOpenRow() const {
  for (int i = (int)rows_.size() - 1; i >= 0; --i) {
    Row* r = rows_[i].get();
    if (r->logged || r->type == Row::LogToggle || r->type == Row::More) continue;
    return r;
  }
  return nullptr;
}

void Gui::newHeading() {
  if (view_.kind != View::Project) { toast("Headings live inside projects"); return; }
  int pid = view_.id;
  openPrompt("New heading", "Heading", "", nullptr, [this, pid](const std::string& v) {
    std::string t = trimmed(v);
    if (t.empty()) return false;
    s_.addHeading(pid, t);
    markDirty();
    reload(true);
    return true;
  });
}

void Gui::updateEditorHint() {
  if (!editor_) return;
  Editor& e = *editor_;
  std::string title = entryText(e.title);
  guidates::Sigils sg;
  if (e.isNew) sg = guidates::parseSigils(title);
  std::string whenT = sg.hasWhen ? sg.when : entryText(e.when);
  std::string listT = !sg.list.empty() ? sg.list : entryText(e.list);
  auto w = guidates::parse(whenT, true);
  auto d = guidates::parse(entryText(e.deadline), false);
  auto l = resolveList(listT);
  setClass(e.whenField, "invalid", !w.ok);
  setClass(e.deadlineField, "invalid", !d.ok);
  setClass(e.listField, "invalid", !l.found);
  std::string msg;
  bool err = false;
  if (!w.ok) { msg = "When: couldn't read “" + trimmed(whenT) + "”"; err = true; }
  else if (!d.ok) { msg = "Deadline: couldn't read “" + trimmed(entryText(e.deadline)) + "”"; err = true; }
  else if (!l.found) { msg = "No list called “" + trimmed(listT) + "”"; err = true; }
  else {
    std::string parts;
    if (!w.value.empty()) parts += "When " + w.label;
    if (!d.value.empty()) parts += std::string(parts.empty() ? "" : "  ·  ") + "Deadline " + d.label;
    if (!l.inbox) parts += std::string(parts.empty() ? "" : "  ·  ") + l.label;
    if (!parts.empty()) msg = parts + "   ";
    msg += "↵ save   Ctrl+↵ save & new   Ctrl+L checklist   Esc done";
  }
  gtk_label_set_text(GTK_LABEL(e.hint), msg.c_str());
  setClass(e.hint, "error", err);
}

bool Gui::applyEditor(int& savedId) {
  Editor& e = *editor_;
  savedId = 0;
  std::string rawTitle = entryText(e.title);
  guidates::Sigils sg;
  if (e.isNew) sg = guidates::parseSigils(rawTitle);
  std::string title = trimmed(e.isNew ? sg.title : rawTitle);
  if (title.empty()) {
    if (e.isNew) return true;  // nothing typed: just discard
    gtk_label_set_text(GTK_LABEL(e.hint), "A to-do needs a title");
    addClass(e.hint, "error");
    return false;
  }
  std::string whenT = sg.hasWhen ? sg.when : entryText(e.when);
  std::string listT = !sg.list.empty() ? sg.list : entryText(e.list);
  auto w = guidates::parse(whenT, true);
  auto d = guidates::parse(entryText(e.deadline), false);
  auto l = resolveList(listT);
  if (!w.ok || !d.ok || !l.found) { updateEditorHint(); return false; }

  Item it = e.isNew ? Item{} : e.orig;
  it.kind = 't';
  it.title = title;
  std::string notes = trimmed(md::markdownOf(e.notes));
  while (!notes.empty() && (notes.back() == '\n' || notes.back() == ' ')) notes.pop_back();
  it.notes = notes;
  std::vector<ChecklistItem> cl;
  for (auto& c : e.cl) {
    std::string t = trimmed(entryText(c->entry));
    if (!t.empty()) cl.push_back({c->done, t});
  }
  it.checklist = serializeChecklist(cl);
  it.doDate = w.value;  // "" | "someday" | ISO -- Store::saveTask understands the sentinel
  it.deadline = d.value;
  std::string tags = joinTags(entryText(e.tags));
  if (!sg.tags.empty()) tags = joinTags(tags.empty() ? sg.tags : tags + "," + sg.tags);
  it.tags = tags;
  int oldProject = e.orig.projectId;
  int id = s_.saveTask(it, l.areaId, l.projectId);
  if (e.isNew) {
    if (e.headingId && l.projectId == view_.id && view_.kind == View::Project) s_.setTaskHeading(id, e.headingId);
    if (e.afterSort >= 0) s_.insertTaskAfter(id, e.afterSort);
    justAddedId_ = id;
  } else if (oldProject != l.projectId) {
    s_.setTaskHeading(id, 0);
  }
  savedId = id;
  markDirty();
  return true;
}

void Gui::destroyEditorWidgets() {
  if (editor_ && editor_->card) gtk_box_remove(GTK_BOX(listBox_), editor_->card);
}

bool Gui::finishEditor(bool save, bool thenNew) {
  if (!editor_) return true;
  int id = 0;
  if (save && !applyEditor(id)) return false;  // validation failed: stay open, hint explains
  destroyEditorWidgets();
  bool wasNew = editor_->isNew;
  editor_.reset();
  if (id) { pendingSelId_ = id; pendingSelKind_ = 't'; }
  reload(true);
  if (wasNew && id) {
    // a new to-do in a view it doesn't belong to (e.g. dated tomorrow, added in Today) quietly lives elsewhere
    bool visible = false;
    for (auto& r : rows_) if (r->type == Row::Task && r->item.id == id) visible = true;
    if (!visible) {
      Item t;
      t.id = id;
      for (auto& x : s_.searchIndex()) if (x.kind == 't' && x.id == id) t = x;
      toast("Added to " + homeViewFor(t).title);
      refreshSidebar();
    }
  }
  if (thenNew) newTask();
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// modals
// ---------------------------------------------------------------------------------------------------------------

void Gui::showModal(GtkWidget* card, std::function<bool(guint, GdkModifierType)> keyFn, GtkWidget* focus) {
  syncStatLabel_ = syncStat2_ = nullptr;  // about to be destroyed with the old card
  ++modalGen_;
  clearChildren(modalHost_);
  addClass(card, "modal");
  onClick(card, [] {});  // swallow clicks so they don't reach the scrim
  gtk_widget_set_cursor_from_name(card, "default");
  gtk_box_append(GTK_BOX(modalHost_), card);
  modalKey_ = std::make_shared<std::function<bool(guint, GdkModifierType)>>(std::move(keyFn));
  gtk_widget_set_visible(modalLayer_, TRUE);
  if (focus) gtk_widget_grab_focus(focus);
}

void Gui::closeModal() {
  modalKey_.reset();
  pal_.reset();
  syncStatLabel_ = syncStat2_ = nullptr;
  gtk_widget_set_visible(modalLayer_, FALSE);
  unsigned gen = ++modalGen_;
  later([this, gen] { if (gen == modalGen_) clearChildren(modalHost_); });
  gtk_window_set_focus(GTK_WINDOW(win_), nullptr);
}

void Gui::toast(const std::string& msg, const std::string& key, bool error, int ms) {
  if (toastShowSrc_) { g_source_remove(toastShowSrc_); toastShowSrc_ = 0; }
  if (toastHideSrc_) { g_source_remove(toastHideSrc_); toastHideSrc_ = 0; }
  if (toastGoneSrc_) { g_source_remove(toastGoneSrc_); toastGoneSrc_ = 0; }
  gtk_label_set_text(GTK_LABEL(toastLabel_), msg.c_str());
  gtk_label_set_text(GTK_LABEL(toastKey_), key.c_str());
  gtk_widget_set_visible(toastKey_, !key.empty());
  setClass(toastBox_, "error", error);
  rmClass(toastBox_, "show");
  gtk_widget_set_visible(toastBox_, TRUE);
  toastShowSrc_ = g_timeout_add(16, +[](gpointer p) -> gboolean {
    auto* s = static_cast<Gui*>(p);
    s->toastShowSrc_ = 0;
    addClass(s->toastBox_, "show");
    return G_SOURCE_REMOVE;
  }, this);
  toastHideSrc_ = g_timeout_add(ms, +[](gpointer p) -> gboolean {
    auto* s = static_cast<Gui*>(p);
    s->toastHideSrc_ = 0;
    rmClass(s->toastBox_, "show");
    s->toastGoneSrc_ = g_timeout_add(260, +[](gpointer q) -> gboolean {
      auto* t = static_cast<Gui*>(q);
      t->toastGoneSrc_ = 0;
      gtk_widget_set_visible(t->toastBox_, FALSE);
      return G_SOURCE_REMOVE;
    }, s);
    return G_SOURCE_REMOVE;
  }, this);
}

// -- command palette / pickers ----------------------------------------------------------------------------------

void Gui::palRefresh() {
  Pal& p = *pal_;
  std::string q = trimmed(entryText(p.entry));
  std::vector<std::pair<int, int>> scored;
  for (int i = 0; i < (int)p.items.size(); ++i) {
    const PalItem& it = p.items[i];
    if (q.empty()) {
      if (!it.hiddenWhenEmpty) scored.push_back({0, i});
    } else {
      int sc = fuzzyScore(q, it.label);
      int sc2 = fuzzyScore(q, it.label + " " + it.sub);
      if (sc >= 0) sc += 40;
      else sc = sc2;
      if (sc >= 0) scored.push_back({sc, i});
    }
  }
  if (!q.empty()) std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
  if (scored.size() > 40) scored.resize(40);
  p.shown.clear();
  for (auto& s : scored) p.shown.push_back(s.second);
  p.sel = 0;
  clearChildren(p.results);
  if (p.shown.empty()) gtk_box_append(GTK_BOX(p.results), label("No matches", "pal-empty"));
  for (size_t k = 0; k < p.shown.size(); ++k) {
    const PalItem& it = p.items[p.shown[k]];
    GtkWidget* row = hbox(12);
    addClass(row, "pal-row");
    GtkWidget* ic = iconWidget(it.icon, 18, it.iconParam, it.icon != Icon::Project && it.icon != Icon::Area && it.icon != Icon::Search && it.icon != Icon::Plus && it.icon != Icon::Sync);
    gtk_box_append(GTK_BOX(row), ic);
    GtkWidget* l = label(it.label, "pal-label");
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), l);
    if (!it.sub.empty()) gtk_box_append(GTK_BOX(row), label(it.sub, "pal-sub"));
    GtkWidget* sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(row), sp);
    if (!it.kind.empty()) gtk_box_append(GTK_BOX(row), label(it.kind, "pal-kind"));
    int idx = (int)k;
    onClick(row, [this, idx] { if (pal_) { pal_->sel = idx; palRun(); } });
    gtk_box_append(GTK_BOX(p.results), row);
  }
  palMove(0);
}

void Gui::palMove(int d) {
  Pal& p = *pal_;
  if (p.shown.empty()) return;
  int n = (int)p.shown.size();
  p.sel = ((p.sel + d) % n + n) % n;
  int k = 0;
  for (GtkWidget* c = gtk_widget_get_first_child(p.results); c; c = gtk_widget_get_next_sibling(c), ++k) {
    setClass(c, "sel", k == p.sel);
    if (k == p.sel) scrollIntoView(p.scroll, p.results, c);
  }
}

void Gui::palRun() {
  if (!pal_ || pal_->shown.empty()) return;
  auto act = pal_->items[pal_->shown[pal_->sel]].act;
  closeModal();
  if (act) act();
}

void Gui::openPalette(const std::string& placeholder, std::vector<PalItem> items) {
  if (editor_ && !finishEditor(true)) return;
  pal_ = std::make_shared<Pal>();
  pal_->items = std::move(items);
  GtkWidget* card = vbox(0);
  gtk_widget_set_size_request(card, 560, -1);
  GtkWidget* eb = hbox(10);
  gtk_widget_set_margin_start(eb, 16);
  gtk_widget_set_margin_end(eb, 16);
  gtk_widget_set_margin_top(eb, 14);
  gtk_widget_set_margin_bottom(eb, 12);
  GtkWidget* si = iconWidget(Icon::Search, 18, 0, false);
  iconRole(si, Role::Fg3);
  gtk_box_append(GTK_BOX(eb), si);
  pal_->entry = gtk_entry_new();
  addClass(pal_->entry, "bare");
  addClass(pal_->entry, "pal-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(pal_->entry), placeholder.c_str());
  gtk_widget_set_hexpand(pal_->entry, TRUE);
  gtk_box_append(GTK_BOX(eb), pal_->entry);
  gtk_box_append(GTK_BOX(card), eb);
  GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  addClass(sep, "pal-sep");
  gtk_box_append(GTK_BOX(card), sep);
  pal_->scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pal_->scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(pal_->scroll), 380);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(pal_->scroll), TRUE);
  pal_->results = vbox(2);
  gtk_widget_set_margin_top(pal_->results, 8);
  gtk_widget_set_margin_bottom(pal_->results, 8);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pal_->scroll), pal_->results);
  gtk_box_append(GTK_BOX(card), pal_->scroll);
  g_signal_connect_swapped(pal_->entry, "changed", G_CALLBACK(+[](gpointer self) {
    auto* g = static_cast<Gui*>(self);
    if (g->pal_) g->palRefresh();
  }), this);
  showModal(card, [this](guint kv, GdkModifierType st) {
    if (!pal_) return false;
    bool ctrl = st & GDK_CONTROL_MASK;
    switch (kv) {
      case GDK_KEY_Escape: closeModal(); return true;
      case GDK_KEY_Return: case GDK_KEY_KP_Enter: palRun(); return true;
      case GDK_KEY_Down: palMove(+1); return true;
      case GDK_KEY_Up: palMove(-1); return true;
      case GDK_KEY_Tab: palMove(+1); return true;
      case GDK_KEY_ISO_Left_Tab: palMove(-1); return true;
      case GDK_KEY_n: case GDK_KEY_j: if (ctrl) { palMove(+1); return true; } return false;
      case GDK_KEY_p: case GDK_KEY_k: if (ctrl) { palMove(-1); return true; } return false;
    }
    return false;
  }, pal_->entry);
  palRefresh();
}

std::vector<PalItem> Gui::commandItems() {
  std::vector<PalItem> out;
  for (const char* n : {"Inbox", "Today", "Upcoming", "Anytime", "Someday", "Logbook", "Tomorrow", "Deadlines", "Logged Projects", "Archived Areas"}) {
    std::string name = n;
    out.push_back({name, "", "List", smartIcon(name), 0, false, [this, name] { goSmart(name); }});
  }
  for (auto& a : s_.areas()) {
    View v{View::Area, "", a.id, a.name};
    out.push_back({a.name, "", "Area", Icon::Area, 0, false, [this, v] { goTo(v); }});
  }
  for (auto& p : s_.projects()) {
    View v{View::Project, "", p.id, p.name};
    out.push_back({p.name, p.sub, "Project", Icon::Project, 0, false, [this, v] { goTo(v); }});
  }
  out.push_back({"New To-Do", "", "N", Icon::Plus, 0, false, [this] { later([this] { newTask(); }); }});
  if (view_.kind == View::Project) out.push_back({"New Heading", "", "Shift+N", Icon::Plus, 0, false, [this] { newHeading(); }});
  out.push_back({"New Project", "", "P", Icon::Plus, 0, false, [this] { newProject(); }});
  out.push_back({"New Area", "", "A", Icon::Plus, 0, false, [this] { newArea(); }});
  if (view_.kind == View::Project) {
    out.push_back({"Edit Project…", "", "Shift+E", Icon::Note, 0, false, [this] { editList(); }});
    if (s_.projectIsOpen(view_.id)) out.push_back({"Complete Project", "", "", Icon::Logbook, 0, false, [this] { completeList(); }});
    out.push_back({"Show / Hide Logged To-Dos", "", "Shift+L", Icon::Logbook, 0, false, [this] { toggleLogged(); }});
  }
  if (view_.kind == View::Area) {
    out.push_back({"Rename Area…", "", "Shift+E", Icon::Note, 0, false, [this] { editList(); }});
    out.push_back({"Archive Area", "", "", Icon::Archive, 0, false, [this] { completeList(); }});
  }
  if (groupable()) out.push_back({"Group by Area & Project", "", "Shift+A", Icon::Anytime, 0, false, [this] { toggleGrouping(); }});
  out.push_back({"Sync with Remote…", "", "Shift+S", Icon::Sync, 0, false, [this] { openSyncDialog(); }});
  out.push_back({"Undo", "", "Z", Icon::Logbook, 0, false, [this] { undo(); }});
  out.push_back({"Toggle Sidebar", "", "B", Icon::Anytime, 0, false, [this] { toggleSidebar(); }});
  out.push_back({"Keyboard Shortcuts", "", "?", Icon::Help, 0, false, [this] { openHelp(); }});
  for (auto& t : s_.searchIndex()) {
    if (t.kind != 't') continue;
    Item tt = t;
    std::string sub = !t.projectName.empty() ? t.projectName : t.areaName;
    out.push_back({t.title, sub, "To-Do", Icon::Checklist, 0, true, [this, tt] { jumpToTask(tt); }});
  }
  return out;
}

void Gui::openPrompt(const std::string& title, const std::string& placeholder, const std::string& initial,
                     std::function<std::string(const std::string&, bool&)> hint, std::function<bool(const std::string&)> submit,
                     const std::string& confirmKeyLabel) {
  if (editor_ && !finishEditor(true)) return;
  (void)confirmKeyLabel;
  GtkWidget* card = vbox(10);
  gtk_widget_set_size_request(card, 460, -1);
  gtk_widget_set_margin_top(card, 0);
  GtkWidget* inner = vbox(10);
  gtk_widget_set_margin_start(inner, 20);
  gtk_widget_set_margin_end(inner, 20);
  gtk_widget_set_margin_top(inner, 18);
  gtk_widget_set_margin_bottom(inner, 16);
  gtk_box_append(GTK_BOX(inner), label(title, "modal-title"));
  GtkWidget* f = hbox(0);
  addClass(f, "field");
  GtkWidget* entry = gtk_entry_new();
  addClass(entry, "bare");
  addClass(entry, "pal-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder.c_str());
  gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
  gtk_editable_set_position(GTK_EDITABLE(entry), -1);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(f), entry);
  gtk_box_append(GTK_BOX(inner), f);
  GtkWidget* hintL = label("", "hint");
  gtk_label_set_wrap(GTK_LABEL(hintL), TRUE);
  gtk_box_append(GTK_BOX(inner), hintL);
  GtkWidget* keys = hbox(14);
  GtkWidget* k1 = hbox(6); gtk_box_append(GTK_BOX(k1), keycap("↵")); gtk_box_append(GTK_BOX(k1), label("Save", "btn-note"));
  GtkWidget* k2 = hbox(6); gtk_box_append(GTK_BOX(k2), keycap("Esc")); gtk_box_append(GTK_BOX(k2), label("Cancel", "btn-note"));
  gtk_box_append(GTK_BOX(keys), k1);
  gtk_box_append(GTK_BOX(keys), k2);
  gtk_box_append(GTK_BOX(inner), keys);
  gtk_box_append(GTK_BOX(card), inner);

  auto hintFn = std::make_shared<std::function<std::string(const std::string&, bool&)>>(hint);
  auto refresh = [hintFn, entry, hintL, f] {
    bool ok = true;
    std::string msg = *hintFn ? (*hintFn)(entryText(entry), ok) : "";
    gtk_label_set_text(GTK_LABEL(hintL), msg.c_str());
    setClass(hintL, "error", !ok);
    setClass(f, "invalid", !ok);
  };
  auto* rf = new std::function<void()>(refresh);
  g_object_set_data_full(G_OBJECT(entry), "refresh", rf, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
  g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable* e, gpointer) {
    (*static_cast<std::function<void()>*>(g_object_get_data(G_OBJECT(e), "refresh")))();
  }), nullptr);
  refresh();
  showModal(card, [this, entry, submit](guint kv, GdkModifierType) {
    if (kv == GDK_KEY_Escape) { closeModal(); return true; }
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
      std::string v = entryText(entry);
      if (submit(v)) closeModal();
      return true;
    }
    return false;
  }, entry);
}

void Gui::openConfirm(const std::string& title, const std::string& body, const std::string& action, bool danger,
                      std::function<void()> go) {
  if (editor_ && !finishEditor(true)) return;
  GtkWidget* card = vbox(12);
  gtk_widget_set_size_request(card, 430, -1);
  GtkWidget* inner = vbox(12);
  gtk_widget_set_margin_start(inner, 22);
  gtk_widget_set_margin_end(inner, 22);
  gtk_widget_set_margin_top(inner, 20);
  gtk_widget_set_margin_bottom(inner, 18);
  gtk_box_append(GTK_BOX(inner), label(title, "modal-title"));
  GtkWidget* b = label(body, "modal-body");
  gtk_label_set_wrap(GTK_LABEL(b), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(b), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(b), 52);
  gtk_label_set_selectable(GTK_LABEL(b), FALSE);
  gtk_box_append(GTK_BOX(inner), b);
  GtkWidget* row = hbox(10);
  gtk_widget_set_halign(row, GTK_ALIGN_END);
  gtk_widget_set_margin_top(row, 6);
  auto goShared = std::make_shared<std::function<void()>>(std::move(go));
  gtk_box_append(GTK_BOX(row), button("Cancel", "Esc", "", [this] { closeModal(); }));
  gtk_box_append(GTK_BOX(row), button(action, "↵", danger ? "danger" : "primary", [this, goShared] { closeModal(); (*goShared)(); }));
  gtk_box_append(GTK_BOX(inner), row);
  gtk_box_append(GTK_BOX(card), inner);
  showModal(card, [this, goShared](guint kv, GdkModifierType) {
    if (kv == GDK_KEY_Escape || kv == GDK_KEY_n) { closeModal(); return true; }
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter || kv == GDK_KEY_y) { closeModal(); (*goShared)(); return true; }
    return true;  // a confirmation swallows everything else
  });
}

void Gui::openHelp() {
  if (editor_ && !finishEditor(true)) return;
  GtkWidget* card = vbox(0);
  GtkWidget* inner = vbox(4);
  gtk_widget_set_margin_start(inner, 26);
  gtk_widget_set_margin_end(inner, 26);
  gtk_widget_set_margin_top(inner, 20);
  gtk_widget_set_margin_bottom(inner, 20);
  gtk_box_append(GTK_BOX(inner), label("Keyboard", "modal-title"));
  struct G { const char* name; std::vector<std::pair<const char*, const char*>> keys; };
  std::vector<G> groups = {
      {"MOVE", {{"j  k  ↓  ↑", "Select next / previous"}, {"1 – 6", "Inbox · Today · Upcoming · Anytime · Someday · Logbook"},
                {"h  ←", "Focus the sidebar (j k to browse, ↵ to return)"}, {"Esc", "Back out of a project or area"},
                {"Ctrl+K  /", "Quick find: jump to anything, run commands"}, {"b", "Toggle the sidebar"}}},
      {"TO-DOS", {{"n", "New to-do   (type @list #tag !when inline)"}, {"Shift+N", "New heading in a project"},
                  {"Space  x", "Complete / reopen"}, {"z  Ctrl+Z", "Undo the last completion"},
                  {"↵  e", "Edit, or open a project"}, {"s", "When"}, {"d", "Deadline"}, {"t", "Tags"},
                  {"m", "Move to a list"}, {"J  K", "Reorder"}, {"Backspace", "Delete (asks first)"}}},
      {"LISTS", {{"p", "New project (in the current area)"}, {"a", "New area"}, {"Shift+E", "Edit the project / rename the area"},
                 {"Shift+A", "Group by area and project (Today, Tomorrow, Anytime, Someday)"},
                 {"Shift+L", "Show / hide a project's logged to-dos"}}},
      {"EDITOR", {{"Tab  Shift+Tab", "Next / previous field (nests a bullet in notes)"}, {"↵", "Save and close"}, {"Ctrl+↵", "Save and add another"},
                  {"Ctrl+L", "Add a checklist item   (↵ next item, ⌫ on an empty one removes it)"},
                  {"Notes", "Markdown: * or - makes a bullet, `code`, **bold**, *italic*, # heading, [text](url); links and phone numbers are clickable"},
                  {"Esc", "Save and close (empty new to-do is discarded)"}}},
      {"SYNC", {{"Shift+S", "Sync dialog: pull, push or both, each confirmed first"}, {"?", "This sheet"}, {"Ctrl+Q", "Quit"}}},
  };
  for (auto& g : groups) {
    gtk_box_append(GTK_BOX(inner), label(g.name, "help-group"));
    for (auto& [k, d] : g.keys) {
      GtkWidget* r = hbox(12);
      GtkWidget* kk = label(k, "keycap", 0.f);
      gtk_widget_set_size_request(kk, 108, -1);
      gtk_widget_set_halign(kk, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(r), kk);
      gtk_box_append(GTK_BOX(r), label(d, "help-text"));
      gtk_box_append(GTK_BOX(inner), r);
    }
  }
  GtkWidget* sw = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 560);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sw), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), inner);
  gtk_box_append(GTK_BOX(card), sw);
  showModal(card, [this](guint kv, GdkModifierType) {
    if (kv == GDK_KEY_Escape || kv == GDK_KEY_question || kv == GDK_KEY_Return || kv == GDK_KEY_q) closeModal();
    return true;
  });
}

// ---------------------------------------------------------------------------------------------------------------
// projects, areas, links
// ---------------------------------------------------------------------------------------------------------------

void Gui::chooseNewList() {
  std::vector<PalItem> items;
  items.push_back({"New Project", "A list of to-dos with an end", "P", Icon::Project, 0, false, [this] { newProject(); }});
  items.push_back({"New Area", "An ongoing part of your life or work", "A", Icon::Area, 0, false, [this] { newArea(); }});
  openPalette("New list…", std::move(items));
}

void Gui::newProject() {
  int area = 0;
  std::string areaName;
  if (view_.kind == View::Area) { area = view_.id; areaName = view_.title; }
  else if (view_.kind == View::Project) { Item cur = s_.getProject(view_.id); area = cur.areaId; areaName = cur.areaName; }
  openPrompt("New project", "Project name", "", [areaName](const std::string&, bool& ok) {
    ok = true;
    return areaName.empty() ? std::string() : "Goes in " + areaName;
  }, [this, area](const std::string& v) {
    std::string n = trimmed(v);
    if (n.empty()) return false;
    for (auto& p : s_.projects(false)) if (lower(p.name) == lower(n)) { toast("A project with that name already exists", "", true); return false; }
    for (auto& p : s_.projects(true)) if (lower(p.name) == lower(n)) { toast("A logged project has that name", "", true); return false; }
    Item p;
    p.title = n;
    int id = s_.saveProject(p, area);
    markDirty();
    refreshSidebar();
    goTo(View{View::Project, "", id, n});
    return true;
  });
}

void Gui::newArea() {
  openPrompt("New area", "Area name", "", nullptr, [this](const std::string& v) {
    std::string n = trimmed(v);
    if (n.empty()) return false;
    for (auto& a : s_.areas(false)) if (lower(a.name) == lower(n)) { toast("That area already exists", "", true); return false; }
    for (auto& a : s_.areas(true)) if (lower(a.name) == lower(n)) { toast("An archived area has that name", "", true); return false; }
    int id = s_.addArea(n);
    markDirty();
    refreshSidebar();
    goTo(View{View::Area, "", id, n});
    return true;
  });
}

// Double-click a project's title (or Shift+E): title, notes, dates and area. For an area: rename it.
void Gui::editList() {
  if (view_.kind == View::Area) {
    int id = view_.id;
    openPrompt("Rename area", "Area name", view_.title, nullptr, [this, id](const std::string& v) {
      std::string n = trimmed(v);
      if (n.empty()) return false;
      s_.renameArea(id, n);
      markDirty();
      view_.title = n;
      refreshSidebar();
      reload(true);
      return true;
    });
    return;
  }
  if (view_.kind != View::Project) return;
  if (editor_ && !finishEditor(true)) return;
  Item p = s_.getProject(view_.id);
  int pid = p.id;

  GtkWidget* card = vbox(0);
  gtk_widget_set_size_request(card, 580, -1);
  GtkWidget* inner = vbox(10);
  gtk_widget_set_margin_start(inner, 22);
  gtk_widget_set_margin_end(inner, 22);
  gtk_widget_set_margin_top(inner, 18);
  gtk_widget_set_margin_bottom(inner, 16);
  gtk_box_append(GTK_BOX(inner), label("Edit project", "modal-title"));

  GtkWidget* tField = hbox(0);
  addClass(tField, "field");
  GtkWidget* te = gtk_entry_new();
  addClass(te, "bare");
  addClass(te, "editor-title");
  gtk_editable_set_text(GTK_EDITABLE(te), p.title.c_str());
  gtk_widget_set_hexpand(te, TRUE);
  gtk_box_append(GTK_BOX(tField), te);
  gtk_box_append(GTK_BOX(inner), tField);

  GtkWidget* nv = nullptr;
  GtkWidget* nf = hbox(0);
  addClass(nf, "field");
  GtkWidget* nw = textArea(p.notes, "Notes — markdown, links and phone numbers work", &nv, 96);
  gtk_widget_set_hexpand(nw, TRUE);
  gtk_box_append(GTK_BOX(nf), nw);
  gtk_box_append(GTK_BOX(inner), nf);

  GtkWidget *we = nullptr, *de = nullptr, *ae = nullptr;
  GtkWidget* grid = hbox(8);
  std::string whenText = p.doDate;
  gtk_box_append(GTK_BOX(grid), editorField("WHEN", &we, whenText, "today, fri, +3d, someday", nullptr));
  gtk_box_append(GTK_BOX(grid), editorField("DEADLINE", &de, p.deadline, "a hard date", nullptr));
  gtk_box_append(GTK_BOX(grid), editorField("AREA", &ae, p.areaName, "None", nullptr));
  gtk_box_append(GTK_BOX(inner), grid);

  auto save = std::make_shared<std::function<bool()>>([this, pid, te, nv, we, de, ae]() {
    std::string title = trimmed(entryText(te));
    if (title.empty()) { toast("A project needs a name", "", true); return false; }
    auto w = guidates::parse(entryText(we), true);
    auto d = guidates::parse(entryText(de), false);
    if (!w.ok) { toast("When: couldn't read “" + trimmed(entryText(we)) + "”", "", true); return false; }
    if (!d.ok) { toast("Deadline: couldn't read “" + trimmed(entryText(de)) + "”", "", true); return false; }
    int areaId = 0;
    std::string an = lower(trimmed(entryText(ae)));
    if (!an.empty() && an != "none") {
      for (auto& a : s_.areas()) if (lower(a.name) == an) areaId = a.id;
      if (!areaId) { toast("No area called “" + trimmed(entryText(ae)) + "”", "", true); return false; }
    }
    Item q = s_.getProject(pid);
    q.title = title;
    q.notes = trimmed(md::markdownOf(nv));
    q.doDate = w.value;
    q.deadline = d.value;
    s_.saveProject(q, areaId);
    markDirty();
    view_.title = title;
    refreshSidebar();
    reload(true);
    return true;
  });
  GtkWidget* row = hbox(10);
  gtk_widget_set_halign(row, GTK_ALIGN_END);
  gtk_widget_set_margin_top(row, 4);
  gtk_box_append(GTK_BOX(row), button("Cancel", "Esc", "", [this] { closeModal(); }));
  gtk_box_append(GTK_BOX(row), button("Save", "Ctrl+↵", "primary", [this, save] { if ((*save)()) closeModal(); }));
  gtk_box_append(GTK_BOX(inner), row);
  gtk_box_append(GTK_BOX(card), inner);
  showModal(card, [this, save, nv](guint kv, GdkModifierType st) {
    if (kv == GDK_KEY_Escape) { closeModal(); return true; }
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
      GtkWidget* f = gtk_window_get_focus(GTK_WINDOW(win_));
      bool inNotes = f && f == nv;
      if (inNotes && !(st & GDK_CONTROL_MASK)) return false;  // a newline (or the next bullet)
      if ((*save)()) closeModal();
      return true;
    }
    return false;
  }, te);
  gtk_editable_set_position(GTK_EDITABLE(te), -1);
}

void Gui::completeList() {
  bool project = view_.kind == View::Project;
  if (!project && view_.kind != View::Area) return;
  if (project && !s_.projectIsOpen(view_.id)) { toast("Already logged"); return; }
  Item it;
  it.id = view_.id;
  it.kind = project ? 'p' : 'a';
  std::string title = view_.title;
  openConfirm(project ? "Complete this project?" : "Archive this area?",
              "“" + title + "” moves to " + (project ? "Logged Projects" : "Archived Areas") + ". You can undo it right after.",
              project ? "Complete" : "Archive", false, [this, it, project] {
                s_.complete(it);
                undo_.push_back({it.id, it.kind});
                markDirty();
                refreshSidebar();
                goSmart("Today");
                toast(project ? "Project completed" : "Area archived", "Z");
              });
}

void Gui::openLink(const std::string& url) {
  struct Ctx { Gui* g; std::string url; };
  GtkUriLauncher* l = gtk_uri_launcher_new(url.c_str());
  gtk_uri_launcher_launch(l, GTK_WINDOW(win_), nullptr, +[](GObject* src, GAsyncResult* res, gpointer d) {
    auto* c = static_cast<Ctx*>(d);
    GError* err = nullptr;
    gboolean ok = gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(src), res, &err);
    bool dismissed = err && g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED);
    if (err) g_error_free(err);
    if (!ok && !dismissed) c->g->linkFallback(c->url);
    g_object_unref(src);
    delete c;
  }, new Ctx{this, url});
}

// No handler for the scheme (typically tel: on a desktop with no phone app): put the number on the clipboard instead.
void Gui::linkFallback(const std::string& url) {
  auto copy = [&](const std::string& text, const char* msg) {
    gdk_clipboard_set_text(gtk_widget_get_clipboard(win_), text.c_str());
    toast(msg);
  };
  if (url.rfind("tel:", 0) == 0) copy(url.substr(4), "No phone app found — number copied");
  else if (url.rfind("mailto:", 0) == 0) copy(url.substr(7), "No mail app found — address copied");
  else toast("Couldn't open " + url, "", true);
}

// ---------------------------------------------------------------------------------------------------------------
// sync
// ---------------------------------------------------------------------------------------------------------------

std::string Gui::remoteUrl() { return remote_; }

void Gui::setSyncing(bool on) {
  syncing_ = on;
  if (on) {
    spinStop_ = 0;
    if (!spinTick_) {
      spinTick_ = gtk_widget_add_tick_callback(syncIcon_, +[](GtkWidget*, GdkFrameClock* fc, gpointer ud) -> gboolean {
        auto* g = static_cast<Gui*>(ud);
        static gint64 last = 0;
        gint64 now = gdk_frame_clock_get_frame_time(fc);
        double dt = last ? (now - last) / 1e6 : 0.016;
        last = now;
        g->spin_ += dt * 2 * M_PI * 1.15;
        if (!g->syncing_) {
          if (g->spinStop_ == 0) g->spinStop_ = std::ceil(g->spin_ / (2 * M_PI)) * 2 * M_PI;  // finish the current turn
          if (g->spin_ >= g->spinStop_) {
            g->spin_ = 0;
            iconSet(g->syncIcon_, Icon::Sync, 0);
            g->spinTick_ = 0;
            last = 0;
            return G_SOURCE_REMOVE;
          }
        }
        iconSet(g->syncIcon_, Icon::Sync, g->spin_);
        return G_SOURCE_CONTINUE;
      }, this, nullptr);
    }
  }
}

void Gui::runWorker(std::function<std::function<void()>()> job) {
  if (worker_.joinable()) worker_.join();
  workerBusy_ = true;
  worker_ = std::thread([this, job] {
    std::function<void()> fin = job();
    auto* cb = new std::function<void()>([this, fin] {
      workerBusy_ = false;
      if (fin) fin();
    });
    g_idle_add(+[](gpointer p) -> gboolean {
      auto* f = static_cast<std::function<void()>*>(p);
      (*f)();
      delete f;
      return G_SOURCE_REMOVE;
    }, cb);
  });
}

void Gui::probeSync() {
  if (workerBusy_ || remote_.empty()) return;
  runWorker([this]() -> std::function<void()> {
    GitSync::Status st;
    try {
      auto dir = dataDir();
      Config cfg((dir / "config").string());
      GitSync gs(dir, cfg);
      Store store((dir / "stride.db").string());
      st = gs.status(store);
    } catch (const std::exception& e) {
      st.error = e.what();
    }
    return [this, st] {
      if (st.busy) return;
      behind_ = st.reachable ? st.behind : behind_;
      dirty_ = st.localDirty;
      updateSyncDot();
      updateSyncLabels();
    };
  });
}

void Gui::updateSyncLabels() {
  if (!syncStatLabel_ || !syncStat2_) return;
  std::string a = behind_ > 0 ? std::to_string(behind_) + (behind_ == 1 ? " new change" : " new changes") + " on the remote" : "Nothing new on the remote";
  std::string b = dirty_ ? "This device has changes the remote hasn't seen" : "Nothing waiting to push";
  gtk_label_set_text(GTK_LABEL(syncStatLabel_), a.c_str());
  gtk_label_set_text(GTK_LABEL(syncStat2_), b.c_str());
  setClass(syncStatLabel_, "good", behind_ == 0);
  setClass(syncStat2_, "good", !dirty_);
}

void Gui::confirmSync(SyncMode mode) {
  std::string title, body, action;
  std::string host = remote_;
  switch (mode) {
    case SyncMode::PullOnly:
      title = "Pull from remote?";
      body = "Changes from your other devices are merged into this one. Nothing is sent to the remote.";
      action = "Pull";
      break;
    case SyncMode::PushOnly:
      title = "Push to remote?";
      body = "This device's changes are encrypted and sent to the remote. Nothing is pulled; if the remote has newer changes, "
             "the push is refused and nothing is lost.";
      action = "Push";
      break;
    default:
      title = "Sync both ways?";
      body = "Pulls other devices' changes into this one, then pushes this device's changes to the remote.";
      action = "Sync";
  }
  openConfirm(title, body + "\n\n" + host, action, false, [this, mode] { startSync(mode); });
}

void Gui::openSyncDialog() {
  if (editor_ && !finishEditor(true)) return;
  GtkWidget* card = vbox(0);
  gtk_widget_set_size_request(card, 470, -1);
  GtkWidget* inner = vbox(8);
  gtk_widget_set_margin_start(inner, 22);
  gtk_widget_set_margin_end(inner, 22);
  gtk_widget_set_margin_top(inner, 20);
  gtk_widget_set_margin_bottom(inner, 18);
  gtk_box_append(GTK_BOX(inner), label("Sync", "modal-title"));

  if (remote_.empty()) {
    GtkWidget* b = label("No remote is configured. Set programs.stride.mirrorRemote in your NixOS config, or add "
                          "mirror_remote=<git url> to the config file in Stride's data directory.", "modal-body");
    gtk_label_set_wrap(GTK_LABEL(b), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(b), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(b), 56);
    gtk_box_append(GTK_BOX(inner), b);
    GtkWidget* row = hbox(10);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), button("Close", "Esc", "", [this] { closeModal(); }));
    gtk_box_append(GTK_BOX(inner), row);
    gtk_box_append(GTK_BOX(card), inner);
    showModal(card, [this](guint kv, GdkModifierType) {
      if (kv == GDK_KEY_Escape || kv == GDK_KEY_Return) closeModal();
      return true;
    });
    return;
  }

  GtkWidget* remoteL = label(remote_, "modal-mono");
  gtk_label_set_ellipsize(GTK_LABEL(remoteL), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(inner), remoteL);
  GtkWidget* stats = vbox(2);
  gtk_widget_set_margin_top(stats, 4);
  gtk_widget_set_margin_bottom(stats, 8);
  syncStatLabel_ = label("Checking the remote…", "stat");
  syncStat2_ = label("", "stat");
  gtk_box_append(GTK_BOX(stats), syncStatLabel_);
  gtk_box_append(GTK_BOX(stats), syncStat2_);
  gtk_box_append(GTK_BOX(inner), stats);

  auto option = [&](Icon ic, const char* t, const char* d, const char* key, SyncMode mode, bool primary) {
    GtkWidget* r = hbox(14);
    addClass(r, "pal-row");
    gtk_widget_set_margin_start(r, 0);
    gtk_widget_set_margin_end(r, 0);
    GtkWidget* i = iconWidget(ic, 20, 0, false);
    if (primary) iconRole(i, Role::Accent);
    gtk_box_append(GTK_BOX(r), i);
    GtkWidget* col = vbox(1);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_box_append(GTK_BOX(col), label(t, "pal-label"));
    GtkWidget* dl = label(d, "pal-sub");
    gtk_label_set_wrap(GTK_LABEL(dl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dl), 46);
    gtk_box_append(GTK_BOX(col), dl);
    gtk_box_append(GTK_BOX(r), col);
    GtkWidget* kcap = keycap(key);
    gtk_widget_set_valign(kcap, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(r), kcap);
    onClick(r, [this, mode] { confirmSync(mode); });
    gtk_box_append(GTK_BOX(inner), r);
    return r;
  };
  option(Icon::Archive, "Pull from remote", "Bring in changes from your other devices. Sends nothing.", "P", SyncMode::PullOnly, false);
  option(Icon::Inbox, "Push to remote", "Send this device's changes. Pulls nothing.", "U", SyncMode::PushOnly, false);
  option(Icon::Sync, "Sync both ways", "Pull, then push. This is what the background timer does.", "S", SyncMode::Both, true);

  GtkWidget* keys = hbox(6);
  gtk_widget_set_margin_top(keys, 6);
  gtk_box_append(GTK_BOX(keys), keycap("Esc"));
  gtk_box_append(GTK_BOX(keys), label("Close   ·   every action asks you to confirm", "btn-note"));
  gtk_box_append(GTK_BOX(inner), keys);
  gtk_box_append(GTK_BOX(card), inner);

  showModal(card, [this](guint kv, GdkModifierType st) {
    if (st & GDK_CONTROL_MASK) return false;
    switch (kv) {
      case GDK_KEY_Escape: closeModal(); return true;
      case GDK_KEY_p: case GDK_KEY_P: confirmSync(SyncMode::PullOnly); return true;
      case GDK_KEY_u: case GDK_KEY_U: confirmSync(SyncMode::PushOnly); return true;
      case GDK_KEY_s: case GDK_KEY_S: case GDK_KEY_Return: case GDK_KEY_KP_Enter: confirmSync(SyncMode::Both); return true;
    }
    return true;
  });
  updateSyncLabels();
  probeSync();
}

void Gui::startSync(SyncMode mode) {
  if (workerBusy_) { toast("A sync is already running"); return; }
  if (remote_.empty()) { toast("No remote configured", "", true); return; }
  setSyncing(true);
  toast(mode == SyncMode::PullOnly ? "Pulling…" : mode == SyncMode::PushOnly ? "Pushing…" : "Syncing…", "", false, 90000);
  runWorker([this, mode]() -> std::function<void()> {
    SyncOutcome out;
    std::string err;
    try {
      auto dir = dataDir();
      Config cfg((dir / "config").string());
      GitSync gs(dir, cfg);
      Store store((dir / "stride.db").string());
      out = gs.sync(store, mode);
    } catch (const std::exception& e) {
      err = e.what();
    }
    return [this, mode, out, err] { onSyncDone(mode, out, err); };
  });
}

void Gui::onSyncDone(SyncMode mode, SyncOutcome out, const std::string& err) {
  setSyncing(false);
  auto trunc = [](std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t nl = s.find('\n');
    if (nl != std::string::npos) s = s.substr(0, nl);
    if (s.size() > 140) s = s.substr(0, 137) + "…";
    return s;
  };
  if (!err.empty()) {
    toast("Sync failed — " + trunc(err), "", true, 8000);
    probeSync();
    return;
  }
  if (out.skipped) { toast("Another sync is running — try again in a moment"); return; }

  if (out.pulled) {
    lastDataVersion_ = s_.dataVersion();
    refreshSidebar();
    reload(true);
  }
  auto& ps = out.pulledStats;
  std::string pulledTxt;
  if (out.pulled) {
    int n = ps.tasks;
    pulledTxt = "pulled " + std::to_string(n) + (n == 1 ? " to-do" : " to-dos");
    if (ps.projects) pulledTxt += ", " + std::to_string(ps.projects) + (ps.projects == 1 ? " project" : " projects");
  }
  std::string msg;
  bool bad = false;
  if (mode == SyncMode::PullOnly) {
    if (out.fetchFailed) { msg = "Couldn't reach the remote — " + trunc(out.message); bad = true; }
    else msg = out.pulled ? "Done — " + pulledTxt : "Already up to date";
    if (!bad) behind_ = 0;
  } else if (out.pushRejected) {
    msg = "The remote has newer changes — pull first";
    bad = true;
  } else if (!out.message.empty() && out.changed && !out.pushed) {
    msg = "Committed, but the push failed — " + trunc(out.message);
    bad = true;
  } else {
    std::vector<std::string> bits;
    if (!pulledTxt.empty()) bits.push_back(pulledTxt);
    if (out.pushed) bits.push_back("pushed");
    else if (mode == SyncMode::PushOnly || !out.changed) bits.push_back(out.changed ? "committed" : "nothing new to push");
    msg = "Done";
    for (size_t i = 0; i < bits.size(); ++i) msg += (i ? ", " : " — ") + bits[i];
    if (out.fetchFailed) { msg += " (couldn't check the remote for incoming changes)"; }
    if (mode != SyncMode::PushOnly && !out.fetchFailed) behind_ = 0;
    if (out.pushed || !out.changed) dirty_ = false;
  }
  toast(msg, "", bad, bad ? 7000 : 3200);
  updateSyncDot();
  if (out.keyJustGenerated) {
    std::string key = out.generatedKeyHex;
    openConfirm("New encryption key", "Everything on the remote is encrypted with this key. Other devices need the same one, and it is shown only once:\n\n" + key,
                "Copy key", false, [this, key] {
                  gdk_clipboard_set_text(gtk_widget_get_clipboard(win_), key.c_str());
                  toast("Key copied");
                });
  }
  probeSync();
}

// ---------------------------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------------------------

bool Gui::onKey(guint kv, GdkModifierType st) {
  bool ctrl = st & GDK_CONTROL_MASK, alt = st & GDK_ALT_MASK, super = st & GDK_SUPER_MASK, shift = st & GDK_SHIFT_MASK;
  if (ctrl && (kv == GDK_KEY_q || kv == GDK_KEY_Q)) { g_application_quit(G_APPLICATION(app_)); return true; }
  if (ctrl && (kv == GDK_KEY_w || kv == GDK_KEY_W)) { gtk_window_close(GTK_WINDOW(win_)); return true; }
  if (auto keep = modalKey_) return (*keep)(kv, st);

  if (editor_) {
    if (editorKey(kv, st)) return true;
    GtkWidget* f = gtk_window_get_focus(GTK_WINDOW(win_));
    bool inTextView = f && GTK_IS_TEXT_VIEW(f);
    if (kv == GDK_KEY_Escape) { finishEditor(true); return true; }
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
      if (ctrl) {
        bool again = editor_->isNew && !trimmed(entryText(editor_->title)).empty();
        finishEditor(true, again);
        return true;
      }
      if (inTextView && !shift) return false;  // newline in notes / checklist
      finishEditor(true);
      return true;
    }
    if (ctrl && (kv == GDK_KEY_k || kv == GDK_KEY_K)) return true;  // not mid-edit
    return false;
  }
  if (ctrl && (kv == GDK_KEY_k || kv == GDK_KEY_K)) {
    openPalette("Jump to a list, project or to-do, or run a command…", commandItems());
    return true;
  }
  if (ctrl && (kv == GDK_KEY_z || kv == GDK_KEY_Z)) { undo(); return true; }
  if (alt && (kv == GDK_KEY_Left)) { back(); return true; }
  if (ctrl || alt || super) return false;
  return sbFocus_ ? onSidebarKey(kv, st) : onListKey(kv, st);
}

bool Gui::onSidebarKey(guint kv, GdkModifierType) {
  int n = (int)sb_.size();
  auto step = [&](int d) {
    if (!n) return;
    sbCursor_ = ((sbCursor_ + d) % n + n) % n;
    goTo(sb_[sbCursor_].view, false);
  };
  switch (kv) {
    case GDK_KEY_j: case GDK_KEY_Down: step(+1); return true;
    case GDK_KEY_k: case GDK_KEY_Up: step(-1); return true;
    case GDK_KEY_l: case GDK_KEY_Right: case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_space:
    case GDK_KEY_Escape: case GDK_KEY_h: case GDK_KEY_Left:
      sbFocus_ = false;
      updateSidebarSelection();
      return true;
    case GDK_KEY_slash:
      openPalette("Jump to a list, project or to-do, or run a command…", commandItems());
      return true;
    case GDK_KEY_b: toggleSidebar(); return true;
  }
  if (kv >= GDK_KEY_1 && kv <= GDK_KEY_6) { sbFocus_ = false; goSmart(kSmart[kv - GDK_KEY_1]); return true; }
  return true;
}

bool Gui::onListKey(guint kv, GdkModifierType) {
  switch (kv) {
    case GDK_KEY_j: case GDK_KEY_Down: moveSel(+1); return true;
    case GDK_KEY_k: case GDK_KEY_Up: moveSel(-1); return true;
    case GDK_KEY_Page_Down: for (int i = 0; i < 8; ++i) moveSel(+1); return true;
    case GDK_KEY_Page_Up: for (int i = 0; i < 8; ++i) moveSel(-1); return true;
    case GDK_KEY_g: case GDK_KEY_Home: if (Row* r = nextSelectable(-1, +1)) select(r); return true;
    case GDK_KEY_G: case GDK_KEY_End: if (Row* r = nextSelectable((int)rows_.size(), -1)) select(r); return true;
    case GDK_KEY_J: reorder(+1); return true;
    case GDK_KEY_K: reorder(-1); return true;
    case GDK_KEY_space: case GDK_KEY_x:
      if (sel_ && sel_->type == Row::Task) toggleRow(sel_);
      return true;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_e: activateSel(); return true;
    case GDK_KEY_n: newTask(); return true;
    case GDK_KEY_N: newHeading(); return true;
    case GDK_KEY_s: promptDate(false); return true;
    case GDK_KEY_d: promptDate(true); return true;
    case GDK_KEY_t: promptTags(); return true;
    case GDK_KEY_m: pickList(); return true;
    case GDK_KEY_BackSpace: case GDK_KEY_Delete: confirmDelete(); return true;
    case GDK_KEY_z: undo(); return true;
    case GDK_KEY_slash: case GDK_KEY_f:
      openPalette("Jump to a list, project or to-do, or run a command…", commandItems());
      return true;
    case GDK_KEY_question: openHelp(); return true;
    case GDK_KEY_b: toggleSidebar(); return true;
    case GDK_KEY_S: openSyncDialog(); return true;
    case GDK_KEY_p: newProject(); return true;
    case GDK_KEY_a: newArea(); return true;
    case GDK_KEY_A: toggleGrouping(); return true;
    case GDK_KEY_E: editList(); return true;
    case GDK_KEY_L: toggleLogged(); return true;
    case GDK_KEY_h: case GDK_KEY_Left: {
      if (!gtk_revealer_get_reveal_child(GTK_REVEALER(sbRevealer_))) toggleSidebar();
      sbFocus_ = true;
      sbCursor_ = 0;
      for (size_t i = 0; i < sb_.size(); ++i) if (sb_[i].view == view_) sbCursor_ = (int)i;
      updateSidebarSelection();
      return true;
    }
    case GDK_KEY_Escape:
      if (view_.kind != View::Smart) back();
      return true;
  }
  if (kv >= GDK_KEY_1 && kv <= GDK_KEY_6) { goSmart(kSmart[kv - GDK_KEY_1]); return true; }
  return false;
}

void Gui::poll() {
  // Themes can change by routes GTK doesn't announce (a portal colour-scheme flip, an edited stylesheet), so look again
  // every few ticks; applyTheme() does nothing unless a colour actually moved.
  if (themeCssStamp() != themeStamp_ || ++pollTick_ % 3 == 0) applyTheme(false);
  int v = s_.dataVersion();
  if (v == lastDataVersion_) return;
  for (auto& r : rows_) if (r->pendingRemoval || r->animating) return;  // let a check-off finish first; try again next tick
  lastDataVersion_ = v;
  if (editor_) { pendingReload_ = true; return; }
  refreshSidebar();
  reload(true);
}

}  // namespace

int runGui(Store& store) {
  if (!gtk_init_check()) {
    std::fputs("stride: no display is reachable for --gui\n", stderr);
    return kGuiUnavailable;
  }
  Gui gui(store);
  GtkApplication* app = gtk_application_new(kAppId, G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* a, gpointer p) { static_cast<Gui*>(p)->activate(a); }), &gui);
  int rc = g_application_run(G_APPLICATION(app), 0, nullptr);
  g_object_unref(app);
  return rc;
}
