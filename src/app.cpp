#include "app.h"

#include <algorithm>
#include <clocale>
#include <cstdio>

#include "finder.h"
#include "form.h"
#include "input.h"

// ---------------------------------------------------------------------------
// view state
// ---------------------------------------------------------------------------

std::string App::active() const { return hidden_.empty() ? views_[view_] : hidden_; }

std::string App::name() const {
  if (scopeKind_ == 'p') return scopeAreaName_.empty() ? scopeName_ : scopeAreaName_ + " / " + scopeName_;
  if (scopeKind_ == 'a') return scopeName_;
  return active();
}

void App::load() {
  areaOrder_ = s_.areaOrder();
  if (scopeKind_ == 'p') {
    scopeDescription_ = s_.getProject(scope_).notes;
    list_ = s_.viewProject(scope_, tags_, s_.projectIsOpen(scope_));
  } else if (scopeKind_ == 'a') {
    scopeDescription_.clear();
    list_ = s_.viewArea(scope_, tags_);
  } else if (hidden_ == "Tomorrow") {
    list_ = s_.viewDay(true, tags_);
  } else if (hidden_ == "Deadlines") {
    list_ = s_.viewDeadlines(tags_);
  } else if (hidden_ == "Logged Projects") {
    list_ = s_.viewLoggedProjects();
  } else if (hidden_ == "Archived Areas") {
    list_ = s_.viewArchivedAreas();
  } else {
    auto& v = views_[view_];
    if (v == "Inbox") list_ = s_.viewInbox(tags_);
    else if (v == "Today") list_ = s_.viewDay(false, tags_);
    else if (v == "Upcoming") list_ = s_.viewUpcoming(tags_);
    else if (v == "Anytime") list_ = s_.viewAnytime(tags_);
    else if (v == "Someday") list_ = s_.viewSomeday(tags_);
    else if (v == "Logbook") list_ = s_.viewLogbook();
  }
  if (group_ && scopeKind_ == 0) applyGrouping();
  pick_ = std::clamp(pick_, 0, std::max(0, (int)list_.size() - 1));
  visualAnchor_ = std::clamp(visualAnchor_, 0, std::max(0, (int)list_.size() - 1));
}

// rank 0: no area, no project (sorts first). rank 1: has an area (grouped by
// area, then by project within it). rank 2: standalone project, no area.
GroupKey App::groupKeyFor(const Item& x) const {
  if (x.projectName.empty() && x.areaName.empty()) return {0, "", ""};
  if (x.areaName.empty()) return {2, x.projectName, ""};
  return {1, x.areaName, x.projectName};
}

void App::applyGrouping() {
  std::stable_sort(list_.begin(), list_.end(), [this](const Item& a, const Item& b) {
    GroupKey ka = groupKeyFor(a), kb = groupKeyFor(b);
    if (ka.rank != kb.rank) return ka.rank < kb.rank;
    if (ka.primary != kb.primary) return ka.primary < kb.primary;
    return ka.secondary < kb.secondary;
  });
}

std::string App::dateGroupLabel(const Item& x) const {
  if (scopeKind_ == 'a') return x.section;
  if (scopeKind_ == 'p') return "";
  std::string v = active();
  if (v == "Upcoming") return friendlyDate(x.doDate.empty() ? x.deadline : x.doDate);
  if (v == "Deadlines") return friendlyDate(x.deadline);
  if (v == "Logbook" || v == "Logged Projects" || v == "Archived Areas") return bucketLabel(x.completedAt);
  return "";
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

void App::draw() {
  erase();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  if (rows < 10 || cols < 40) {
    mvprintw(0, 0, "Terminal too small");
    refresh();
    return;
  }
  int off = sidebar_ ? 26 : 0;
  if (sidebar_) drawSidebar(rows, off);
  drawMain(rows, cols, off);
  refresh();
}

void App::drawSidebar(int rows, int width) {
  sidebarRows_.clear();
  attron(A_BOLD | COLOR_PAIR(1));
  mvprintw(1, 2, "\xe2\x97\x89 STRIDE");
  attroff(A_BOLD | COLOR_PAIR(1));
  mvprintw(3, 2, "FOCUS");
  for (int i = 0; i < (int)views_.size(); ++i) {
    bool cur = scopeKind_ == 0 && hidden_.empty() && i == view_;
    if (cur) attron(A_REVERSE);
    mvprintw(5 + i, 2, "%s %s", cur ? "\xe2\x96\xa3" : "\xe2\x97\x8b", clip(views_[i], width - 6).c_str());
    if (cur) attroff(A_REVERSE);
    sidebarRows_.push_back({5 + i, {'v', i}});
  }
  int y = 5 + (int)views_.size() + 1;
  mvprintw(y++, 2, "AREAS");
  for (auto& a : s_.areas()) {
    if (y >= rows - 3) break;
    bool cur = scopeKind_ == 'a' && scope_ == a.id;
    if (cur) attron(A_REVERSE);
    mvprintw(y, 2, "\xe2\x97\x88 %s", clip(a.name, width - 6).c_str());
    if (cur) attroff(A_REVERSE);
    sidebarRows_.push_back({y, {'a', a.id}});
    ++y;
    for (auto& p : s_.projectsInArea(a.id)) {
      if (y >= rows - 3) break;
      bool curp = scopeKind_ == 'p' && scope_ == p.id;
      if (curp) attron(A_REVERSE);
      mvprintw(y, 4, "\xe2\x96\xb9 %s", clip(p.name, width - 8).c_str());
      if (curp) attroff(A_REVERSE);
      sidebarRows_.push_back({y, {'p', p.id}});
      ++y;
    }
  }
  for (auto& p : s_.projects()) {
    if (!p.sub.empty()) continue;
    if (y >= rows - 3) break;
    bool curp = scopeKind_ == 'p' && scope_ == p.id;
    if (curp) attron(A_REVERSE);
    mvprintw(y, 2, "\xe2\x97\x87 %s", clip(p.name, width - 6).c_str());
    if (curp) attroff(A_REVERSE);
    sidebarRows_.push_back({y, {'p', p.id}});
    ++y;
  }
  attron(A_DIM);
  mvprintw(rows - 2, 2, "%s", clip("click to jump \xc2\xb7 right-click item: actions", width - 4).c_str());
  attroff(A_DIM);
  mvvline(0, width, ACS_VLINE, rows);
}

bool App::rowSelected(int index) const {
  if (!visual_) return index == pick_;
  int lo = std::min(visualAnchor_, pick_), hi = std::max(visualAnchor_, pick_);
  return index >= lo && index <= hi;
}

void App::drawIconStrip(int y, int cols, const Item& x) {
  int stripW = 22;
  int cx = cols - stripW;
  if (cx < 0) return;
  std::string tdy = today();
  if (x.deadline.size() >= 10) {
    bool overdue = x.deadline <= tdy;
    if (overdue) attron(COLOR_PAIR(2));
    mvprintw(y, cx, "\xe2\x9a\x91%s", x.deadline.substr(5, 5).c_str());
    if (overdue) attroff(COLOR_PAIR(2));
  }
  if (x.doDate.size() >= 10) mvprintw(y, cx + 8, "\xe2\x97\xb7%s", x.doDate.substr(5, 5).c_str());
  if (!x.notes.empty()) mvprintw(y, cx + 16, "\xe2\x89\xa1");
  if (!parseChecklist(x.checklist).empty()) mvprintw(y, cx + 18, "\xe2\x98\x91");
  if (!x.tags.empty()) mvprintw(y, cx + 20, "#");
}

void App::drawMain(int rows, int cols, int off) {
  mainRows_.clear();
  int innerW = cols - off - 4;
  attron(A_BOLD);
  mvprintw(1, off + 3, "%s", clip(name(), innerW - 20).c_str());
  attroff(A_BOLD);
  if (!tags_.empty()) {
    attron(A_DIM);
    std::string tagLabel = clip("tags: " + tags_, 24);
    mvprintw(1, std::max(off + 3, cols - (int)tagLabel.size() - 2), "%s", tagLabel.c_str());
    attroff(A_DIM);
  }
  int headerRow = 2;
  if (scopeKind_ == 'p' && !scopeDescription_.empty()) {
    auto lines = wrapText(scopeDescription_, innerW);
    attron(A_DIM);
    for (int i = 0; i < (int)lines.size() && i < 3; ++i) mvprintw(2 + i, off + 3, "%s", clip(lines[i], innerW).c_str());
    attroff(A_DIM);
    headerRow = 2 + std::min((int)lines.size(), 3);
  }
  mvhline(headerRow, off + 2, ACS_HLINE, std::max(0, cols - off - 4));

  if (list_.empty()) {
    attron(A_DIM);
    mvprintw(headerRow + 2, off + 4, "%s", clip("Nothing here yet. n captures a next action.", innerW).c_str());
    attroff(A_DIM);
  }

  bool twoLevel = group_ && scopeKind_ == 0;
  std::string lastPrimary, lastSecondary;
  bool primarySet = false;
  int y = headerRow + 2;
  for (int i = 0; i < (int)list_.size() && y < rows - 2; ++i) {
    auto& x = list_[i];
    if (twoLevel) {
      GroupKey gk = groupKeyFor(x);
      if (!primarySet || gk.primary != lastPrimary) {
        if (!gk.primary.empty()) {
          attron(A_BOLD | COLOR_PAIR(3));
          mvprintw(y++, off + 3, "%s", clip(gk.primary, innerW).c_str());
          attroff(A_BOLD | COLOR_PAIR(3));
        }
        lastPrimary = gk.primary;
        lastSecondary.clear();
        primarySet = true;
        if (y >= rows - 2) break;
      }
      if (!gk.secondary.empty() && gk.secondary != lastSecondary) {
        attron(COLOR_PAIR(3));
        mvprintw(y++, off + 5, "%s", clip(gk.secondary, innerW - 2).c_str());
        attroff(COLOR_PAIR(3));
        lastSecondary = gk.secondary;
        if (y >= rows - 2) break;
      }
    } else {
      std::string grp = dateGroupLabel(x);
      if (!grp.empty() && grp != lastPrimary) {
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(y++, off + 3, "%s", clip(grp, innerW).c_str());
        attroff(A_BOLD | COLOR_PAIR(3));
        lastPrimary = grp;
        if (y >= rows - 2) break;
      }
    }
    bool selected = rowSelected(i);
    mainRows_.push_back({y, i});
    if (selected) attron(A_REVERSE);
    int indent = off + 4;
    if (scopeKind_ == 'p' && x.kind == 't' && x.headingId != 0) indent += 2;
    if (x.kind == 'h') {
      attron(A_BOLD);
      mvprintw(y, off + 4, "%s", clip("\xe2\x80\x94 " + x.title, innerW - 2).c_str());
      attroff(A_BOLD);
    } else {
      bool graySomeday = x.someday && x.status == "open";
      bool dim = (x.status != "open") || graySomeday;
      if (graySomeday) attron(COLOR_PAIR(4));
      else if (dim) attron(A_DIM);
      std::string icon = x.kind == 'p' ? "\xe2\x97\x87" : x.kind == 'a' ? "\xe2\x97\x88" : "\xe2\x97\x8b";
      std::string mark = x.status != "open" ? "\xe2\x9c\x93 " : "";
      mvprintw(y, indent, "%s", clip(icon + " " + mark + x.title, std::max(1, cols - indent - 23)).c_str());
      if (graySomeday) attroff(COLOR_PAIR(4));
      else if (dim) attroff(A_DIM);
      drawIconStrip(y, cols, x);
    }
    if (selected) attroff(A_REVERSE);
    ++y;
  }
  attron(A_DIM);
  mvprintw(rows - 1, off + 3, "%s", clip(visual_ ? "VISUAL -- j/k extend \xc2\xb7 x complete \xc2\xb7 m move \xc2\xb7 T tag \xc2\xb7 s/S dates \xc2\xb7 Esc exit"
                                                  : "? for shortcuts",
                                          std::max(0, cols - off - 5))
                                     .c_str());
  attroff(A_DIM);
}

// ---------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------

int App::findAreaId(const std::string& name) {
  std::string n = lower(trimmed(name));
  if (n.empty()) return 0;
  for (auto& a : s_.areas())
    if (lower(a.name) == n) return a.id;
  return 0;
}
int App::findProjectId(const std::string& name) {
  std::string n = lower(trimmed(name));
  if (n.empty()) return 0;
  for (auto& p : s_.projects())
    if (lower(p.name) == n) return p.id;
  return 0;
}

// ---------------------------------------------------------------------------
// forms
// ---------------------------------------------------------------------------

void App::taskForm(std::optional<Item> e, int presetHeadingId, int insertAfterSortOrder) {
  Item t = e.value_or(Item{});
  std::string areaDefault = t.areaName, projectDefault = t.projectName;
  if (!t.id) {
    if (scopeKind_ == 'a') areaDefault = scopeName_;
    else if (scopeKind_ == 'p') {
      projectDefault = scopeName_;
      areaDefault = scopeAreaName_;
    }
  }
  int w = 70, h = 18;
  WINDOW* win = openDialog(h, w, t.id ? "EDIT TASK" : "NEW TASK");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {
      {"Title", t.title, false, 1},          {"Description", t.notes, true, 3},
      {"Tags", t.tags, false, 1},             {"Do date", t.someday ? "someday" : t.doDate, false, 1},
      {"Deadline", t.deadline, false, 1},     {"Area", areaDefault, false, 1},
      {"Project", projectDefault, false, 1},
  };
  int startY = 3, labelX = 2, valueX = 15, valueW = w - valueX - 3;
  int footerY = startY;
  for (auto& f : fields) footerY += f.multiline ? std::max(2, f.height) : 1;
  wattron(win, A_DIM);
  mvwprintw(win, footerY + 1, 2, "%s",
            clip("YYYY-MM-DD or 'someday'  \xc2\xb7  Tab/Enter next \xc2\xb7 Shift+Enter save \xc2\xb7 Esc cancel", w - 4)
                .c_str());
  wattroff(win, A_DIM);
  wrefresh(win);
  FormResult r = runForm(win, fields, startY, labelX, valueX, valueW);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty()) return;
  t.title = fields[0].value;
  t.notes = fields[1].value;
  t.tags = fields[2].value;
  t.doDate = fields[3].value;
  t.deadline = fields[4].value;
  int areaId = findAreaId(fields[5].value);
  int projectId = findProjectId(fields[6].value);
  if (projectId && !areaId) areaId = s_.projectAreaId(projectId);
  bool wasNew = t.id == 0;
  int newId = s_.saveTask(t, areaId, projectId);
  if (wasNew) {
    if (presetHeadingId) s_.setTaskHeading(newId, presetHeadingId);
    if (insertAfterSortOrder >= 0) s_.insertTaskAfter(newId, insertAfterSortOrder);
  }
}

void App::projectForm(std::optional<Item> e) {
  Item p = e.value_or(Item{});
  std::string areaDefault = p.areaName;
  if (!p.id && scopeKind_ == 'a') areaDefault = scopeName_;
  int w = 70, h = 15;
  WINDOW* win = openDialog(h, w, p.id ? "EDIT PROJECT" : "NEW PROJECT");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {
      {"Name", p.title, false, 1}, {"Description", p.notes, true, 3}, {"Area", areaDefault, false, 1},
      {"Do date", p.doDate, false, 1}, {"Deadline", p.deadline, false, 1},
  };
  int startY = 3, labelX = 2, valueX = 15, valueW = w - valueX - 3;
  int footerY = startY;
  for (auto& f : fields) footerY += f.multiline ? std::max(2, f.height) : 1;
  wattron(win, A_DIM);
  mvwprintw(win, footerY + 1, 2, "%s", clip("Tab/Enter next \xc2\xb7 Shift+Enter save \xc2\xb7 Esc cancel", w - 4).c_str());
  wattroff(win, A_DIM);
  wrefresh(win);
  FormResult r = runForm(win, fields, startY, labelX, valueX, valueW);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty()) return;
  p.title = fields[0].value;
  p.notes = fields[1].value;
  p.doDate = fields[3].value;
  p.deadline = fields[4].value;
  s_.saveProject(p, findAreaId(fields[2].value));
}

void App::areaForm(std::optional<Item> e) {
  Item a = e.value_or(Item{});
  int w = 50, h = 6;
  WINDOW* win = openDialog(h, w, a.id ? "RENAME AREA" : "NEW AREA");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {{"Name", a.title, false, 1}};
  FormResult r = runForm(win, fields, 3, 2, 10, w - 13);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty()) return;
  if (a.id) s_.renameArea(a.id, fields[0].value);
  else s_.addArea(fields[0].value);
}

void App::headingForm() {
  int w = 48, h = 6;
  WINDOW* win = openDialog(h, w, "NEW HEADING");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {{"Title", "", false, 1}};
  FormResult r = runForm(win, fields, 3, 2, 10, w - 13);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty()) return;
  s_.addHeading(scope_, fields[0].value);
}

void App::headingLifecycle(const Item& heading) {
  int w = 46, h = 6;
  WINDOW* win = openDialog(h, w, "HEADING");
  getmaxyx(win, h, w);
  mvwprintw(win, 3, 2, "%s", clip("r rename   d delete   Esc cancel", w - 4).c_str());
  wrefresh(win);
  KeyEvent k = readKey(win);
  delwin(win);
  if (k.type != Key::Char) return;
  if (k.ch == 'r') {
    int w2 = 48, h2 = 6;
    WINDOW* win2 = openDialog(h2, w2, "RENAME HEADING");
    getmaxyx(win2, h2, w2);
    std::vector<FormField> fields = {{"Title", heading.title, false, 1}};
    FormResult r = runForm(win2, fields, 3, 2, 10, w2 - 13);
    delwin(win2);
    if (r == FormResult::Saved && !trimmed(fields[0].value).empty()) s_.renameHeading(heading.id, fields[0].value);
  } else if (k.ch == 'd') {
    if (confirmDialog("Delete heading \"" + heading.title + "\"? Tasks stay, just ungrouped.")) s_.deleteHeading(heading.id);
  }
}

void App::tagFilterForm() { tags_ = runTagPicker(s_.allTags(), tags_); }

void App::lifecycle(const Item& i) {
  int w = 46, h = 6;
  WINDOW* win = openDialog(h, w, i.kind == 'p' ? "PROJECT" : "AREA");
  getmaxyx(win, h, w);
  mvwprintw(win, 3, 2, "%s", clip("c complete   x cancel   d delete", w - 4).c_str());
  wrefresh(win);
  KeyEvent k = readKey(win);
  delwin(win);
  if (k.type != Key::Char) return;
  if (k.ch == 'c') s_.complete(i);
  else if (k.ch == 'x') s_.cancel(i);
  else if (k.ch == 'd' && confirmDialog("Delete \"" + i.title + "\" permanently?")) {
    s_.erase(i);
    if (scopeKind_ == i.kind && scope_ == i.id) {
      scopeKind_ = 0;
      pick_ = 0;
    }
  }
}

void App::checklistEditor(Item t) {
  auto items = parseChecklist(t.checklist);
  int pick = 0;
  bool dirty = false;
  while (true) {
    int w = 56, h = std::clamp((int)items.size() + 6, 8, 20);
    WINDOW* win = openDialog(h, w, "CHECKLIST: " + t.title);
    getmaxyx(win, h, w);
    int maxRows = h - 6;
    for (int i = 0; i < (int)items.size() && i < maxRows; ++i) {
      bool sel = i == pick;
      if (sel) wattron(win, A_REVERSE);
      mvwprintw(win, 3 + i, 2, "%s %s", items[i].done ? "\xe2\x98\x91" : "\xe2\x98\x90",
                clip(items[i].text, w - 6).c_str());
      if (sel) wattroff(win, A_REVERSE);
    }
    if (items.empty()) {
      wattron(win, A_DIM);
      mvwprintw(win, 3, 2, "No items yet.");
      wattroff(win, A_DIM);
    }
    wattron(win, A_DIM);
    mvwprintw(win, h - 2, 2, "%s", clip("n add \xc2\xb7 Enter/space toggle \xc2\xb7 d delete \xc2\xb7 Esc close", w - 4).c_str());
    wattroff(win, A_DIM);
    wrefresh(win);
    KeyEvent k = readKey(win);
    bool closing = false;
    if (k.type == Key::Escape) closing = true;
    else if (k.type == Key::Down || (k.type == Key::Char && k.ch == 'j'))
      pick = std::min(pick + 1, std::max(0, (int)items.size() - 1));
    else if (k.type == Key::Up || (k.type == Key::Char && k.ch == 'k'))
      pick = std::max(pick - 1, 0);
    else if ((k.type == Key::Enter || (k.type == Key::Char && k.ch == ' ')) && !items.empty()) {
      items[pick].done = !items[pick].done;
      dirty = true;
    } else if (k.type == Key::Char && k.ch == 'd' && !items.empty()) {
      items.erase(items.begin() + pick);
      pick = std::min(pick, std::max(0, (int)items.size() - 1));
      dirty = true;
    } else if (k.type == Key::Char && k.ch == 'n') {
      delwin(win);
      int w2 = 52, h2 = 5;
      WINDOW* win2 = openDialog(h2, w2, "NEW ITEM");
      getmaxyx(win2, h2, w2);
      std::vector<FormField> fields = {{"Item", "", false, 1}};
      FormResult r = runForm(win2, fields, 3, 2, 8, w2 - 11);
      delwin(win2);
      if (r == FormResult::Saved && !trimmed(fields[0].value).empty()) {
        items.push_back({false, fields[0].value});
        dirty = true;
      }
      continue;
    }
    delwin(win);
    if (closing) break;
  }
  if (dirty) {
    t.checklist = serializeChecklist(items);
    s_.saveTask(t, t.areaId, t.projectId);
  }
}

void App::help() {
  std::vector<std::string> lines = {
      "Mouse           click to select/navigate \xc2\xb7 click selected row to open",
      "                right-click any row for its actions menu",
      "",
      "j/k            move selection",
      "h/l, Esc       switch list / go back",
      "J/K            reorder (crosses into a heading)",
      "Enter          edit task \xc2\xb7 open project \xc2\xb7 rename/delete heading",
      "n              new... (task / project / area / heading)",
      "c              edit the selected task's checklist",
      "e              edit selected item",
      "x              complete task, or lifecycle for project/area",
      "X              lifecycle for the project/area you're inside",
      "m              move task to another project",
      "f              fuzzy find & jump anywhere",
      "T              filter by tags (fuzzy-searches existing tags)",
      "A              group by area / project",
      "v              visual mode: select a range for bulk actions",
      "u              revive (un-complete/un-cancel) in logbook/archives",
      "d              delete permanently (logbook & archives)",
      "b              toggle sidebar",
      "q              quit",
  };
  int h = (int)lines.size() + 4, w = 58;
  WINDOW* win = openDialog(h, w, "SHORTCUTS");
  getmaxyx(win, h, w);
  for (int i = 0; i < (int)lines.size() && 3 + i < h - 1; ++i) mvwprintw(win, 3 + i, 2, "%s", clip(lines[i], w - 4).c_str());
  wrefresh(win);
  readKey(win);
  delwin(win);
}

// ---------------------------------------------------------------------------
// navigation / finder
// ---------------------------------------------------------------------------

void App::openContainer(char kind, int id, const std::string& title, const std::string& sub) {
  scopeKind_ = kind;
  scope_ = id;
  scopeName_ = title;
  scopeAreaName_ = sub;
  hidden_.clear();
  pick_ = 0;
}

std::string App::homeViewFor(const Item& t) const {
  std::string tdy = today();
  if ((!t.doDate.empty() && t.doDate <= tdy) || (!t.deadline.empty() && t.deadline <= tdy)) return "Today";
  if (!t.doDate.empty()) return "Upcoming";
  if (t.someday) return "Someday";
  if (!t.deadline.empty()) return "Deadlines";
  return "Inbox";
}

void App::jumpToTask(const Item& t) {
  if (t.projectId) {
    openContainer('p', t.projectId, t.projectName, t.areaName);
  } else if (t.areaId) {
    openContainer('a', t.areaId, t.areaName, "");
  } else {
    scopeKind_ = 0;
    std::string v = homeViewFor(t);
    if (v == "Tomorrow" || v == "Deadlines" || v == "Logged Projects" || v == "Archived Areas") {
      hidden_ = v;
    } else {
      hidden_.clear();
      auto it = std::find(views_.begin(), views_.end(), v);
      view_ = it != views_.end() ? (int)(it - views_.begin()) : 0;
    }
  }
  load();
  for (int i = 0; i < (int)list_.size(); ++i)
    if (list_[i].kind == 't' && list_[i].id == t.id) {
      pick_ = i;
      break;
    }
}

void App::find() {
  std::vector<PickerItem> items;
  for (auto& v : views_) items.push_back({"\xe2\x96\xa3", v, "List", 'v', Item{}});
  for (std::string v : {"Tomorrow", "Deadlines", "Logged Projects", "Archived Areas"})
    items.push_back({"\xe2\x96\xa3", v, "List", 'H', Item{}});
  for (auto& idx : s_.searchIndex()) {
    if (idx.kind == 'a') items.push_back({"\xe2\x97\x88", idx.title, "Area", 'a', idx});
    else if (idx.kind == 'p')
      items.push_back(
          {"\xe2\x97\x87", idx.title, idx.areaName.empty() ? "Project" : "Project \xc2\xb7 " + idx.areaName, 'p', idx});
    else {
      std::string sub = !idx.projectName.empty()  ? idx.projectName
                         : !idx.areaName.empty()   ? idx.areaName
                         : idx.someday             ? "Someday"
                                                    : "Task";
      items.push_back({"\xe2\x97\x8b", idx.title, sub, 't', idx});
    }
  }
  int sel = runPicker("FIND", items);
  if (sel < 0) return;
  auto& c = items[sel];
  if (c.kind == 'v') {
    scopeKind_ = 0;
    hidden_.clear();
    auto it = std::find(views_.begin(), views_.end(), c.label);
    view_ = it != views_.end() ? (int)(it - views_.begin()) : 0;
  } else if (c.kind == 'H') {
    scopeKind_ = 0;
    hidden_ = c.label;
  } else if (c.kind == 'a') {
    openContainer('a', c.raw.id, c.raw.title, "");
  } else if (c.kind == 'p') {
    openContainer('p', c.raw.id, c.raw.title, c.raw.areaName);
  } else if (c.kind == 't') {
    jumpToTask(c.raw);
  }
}

int App::pickProject(const std::string& title) {
  std::vector<PickerItem> items;
  Item inbox;
  items.push_back({"\xe2\x97\x87", "Inbox (no project)", "", 'p', inbox});
  for (auto& p : s_.projects()) {
    Item raw;
    raw.id = p.id;
    items.push_back({"\xe2\x97\x87", p.name, p.sub.empty() ? "Project" : "Project \xc2\xb7 " + p.sub, 'p', raw});
  }
  int sel = runPicker(title, items);
  return sel < 0 ? -1 : items[sel].raw.id;
}

// ---------------------------------------------------------------------------
// reordering
// ---------------------------------------------------------------------------

void App::moveItem(int delta) {
  int i = pick_, j = pick_ + delta;
  if (j < 0 || j >= (int)list_.size()) return;
  Item& a = list_[i];
  Item& b = list_[j];
  if (scopeKind_ == 'p' && a.kind == 't' && b.kind == 'h') s_.setTaskHeading(a.id, b.id);
  s_.swapOrder(a, b);
  pick_ = j;
}

// ---------------------------------------------------------------------------
// visual mode / bulk actions
// ---------------------------------------------------------------------------

std::vector<int> App::selectedIndices() const {
  std::vector<int> out;
  if (!visual_) {
    if (!list_.empty() && list_[pick_].kind == 't') out.push_back(pick_);
    return out;
  }
  int lo = std::min(visualAnchor_, pick_), hi = std::max(visualAnchor_, pick_);
  for (int i = lo; i <= hi && i < (int)list_.size(); ++i)
    if (list_[i].kind == 't') out.push_back(i);
  return out;
}

void App::bulkComplete() {
  for (int i : selectedIndices()) s_.complete(list_[i]);
  visual_ = false;
}

void App::bulkMove() {
  auto idxs = selectedIndices();
  visual_ = false;
  if (idxs.empty()) return;
  int pid = pickProject("MOVE TO PROJECT");
  if (pid < 0) return;
  for (int i : idxs) s_.moveTask(list_[i].id, pid ? s_.projectAreaId(pid) : 0, pid);
}

void App::bulkTag() {
  auto idxs = selectedIndices();
  visual_ = false;
  if (idxs.empty()) return;
  std::string chosen = runTagPicker(s_.allTags(), "");
  if (chosen.empty() || chosen == "none") return;
  for (int i : idxs) {
    Item t = list_[i];
    auto tags = splitComma(t.tags);
    for (auto& nt : splitComma(chosen)) {
      std::string nv = trimmed(nt);
      if (nv.empty()) continue;
      bool exists = false;
      for (auto& e : tags)
        if (lower(trimmed(e)) == lower(nv)) exists = true;
      if (!exists) tags.push_back(nv);
    }
    std::string merged;
    for (auto& tg : tags) {
      if (!merged.empty()) merged += ",";
      merged += trimmed(tg);
    }
    t.tags = merged;
    s_.saveTask(t, t.areaId, t.projectId);
  }
}

void App::bulkSetDate(bool deadline) {
  auto idxs = selectedIndices();
  visual_ = false;
  if (idxs.empty()) return;
  int w = 50, h = 6;
  WINDOW* win = openDialog(h, w, deadline ? "SET DEADLINE" : "SET DO DATE");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {{"Date", "", false, 1}};
  FormResult r = runForm(win, fields, 3, 2, 10, w - 13);
  delwin(win);
  if (r != FormResult::Saved || trimmed(fields[0].value).empty()) return;
  for (int i : idxs) {
    Item t = list_[i];
    if (deadline) t.deadline = fields[0].value;
    else t.doDate = fields[0].value;
    s_.saveTask(t, t.areaId, t.projectId);
  }
}

// ---------------------------------------------------------------------------
// input dispatch
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// mouse: click to select/navigate, right-click for a contextual actions menu
// (fewer shortcuts to memorize -- everything reachable by keyboard here is
// still available on the keys documented in help(), this just adds a
// second, discoverable way in, closer to how Things 3 itself works).
// ---------------------------------------------------------------------------

void App::showActionsMenu(int index) {
  if (index < 0 || index >= (int)list_.size()) return;
  pick_ = index;
  showActionsMenuFor(list_[index]);
}

void App::showActionsMenuFor(const Item& x) {
  bool open = x.status == "open";

  std::vector<PickerItem> opts;
  auto add = [&](const char* icon, const std::string& label, char code) { opts.push_back({icon, label, "", code, x}); };

  if (x.kind == 't') {
    add(open ? "\xe2\x9c\x93" : "\xe2\x86\xba", open ? "Complete" : "Reopen", 'x');
    add("\xe2\x9c\x8e", "Edit...", 'e');
    add("\xe2\x86\x92", "Move to project...", 'm');
    add("\xe2\x98\x91", "Edit checklist...", 'c');
    if (!open) add("\xf0\x9f\x97\x91", "Delete permanently", 'd');
  } else if (x.kind == 'p' || x.kind == 'a') {
    if (x.kind == 'p') add("\xe2\x86\xb5", "Open", '\n');
    add("\xe2\x9c\x8e", "Edit...", 'e');
    if (open) add("\xe2\x9a\x99", "Complete / cancel / delete...", 'L');
    else {
      add("\xe2\x86\xba", "Reopen", 'x');
      add("\xf0\x9f\x97\x91", "Delete permanently", 'd');
    }
  } else if (x.kind == 'h') {
    add("\xe2\x9c\x8e", "Rename / delete...", 'H');
  }
  if (opts.empty()) return;

  int r = runPicker(x.title, opts);
  if (r < 0) return;
  switch (opts[r].kind) {
    case 'x':
      if (x.kind == 't') s_.complete(x);
      else s_.reopen(x);
      break;
    case 'e':
      if (x.kind == 't') taskForm(x);
      else if (x.kind == 'p') projectForm(x);
      else if (x.kind == 'a') areaForm(x);
      break;
    case 'm': {
      int pid = pickProject("MOVE TO PROJECT");
      if (pid >= 0) s_.moveTask(x.id, pid ? s_.projectAreaId(pid) : 0, pid);
      break;
    }
    case 'c': checklistEditor(x); break;
    case 'L': lifecycle(x); break;
    case 'd':
      if (x.kind == 't') s_.erase(x);
      else if (confirmDialog("Delete \"" + x.title + "\" permanently?")) s_.erase(x);
      break;
    case '\n': openContainer('p', x.id, x.title, x.areaName); break;
    case 'H': headingLifecycle(x); break;
  }
}

void App::handleMouse() {
  MEVENT ev;
  if (getmouse(&ev) != OK) return;
  bool leftClick = ev.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED);
  bool rightClick = ev.bstate & (BUTTON3_CLICKED | BUTTON3_PRESSED);
  if (!leftClick && !rightClick) return;

  for (auto& [y, target] : sidebarRows_) {
    if (ev.y != y) continue;
    if (target.kind == 'v') {
      view_ = target.idOrView;
      scopeKind_ = 0;
      hidden_.clear();
      pick_ = 0;
    } else if (target.kind == 'a') {
      std::string areaName;
      for (auto& a : s_.areas())
        if (a.id == target.idOrView) areaName = a.name;
      if (rightClick) {
        // drawSidebar only ever lists open areas (areas(false)), so this is
        // always "open" -- no need to look status up.
        Item x;
        x.id = target.idOrView;
        x.kind = 'a';
        x.title = areaName;
        x.status = "open";
        showActionsMenuFor(x);
      } else {
        openContainer('a', target.idOrView, areaName, "");
      }
    } else if (target.kind == 'p') {
      Item p = s_.getProject(target.idOrView);
      if (rightClick) showActionsMenuFor(p);
      else openContainer('p', p.id, p.title, p.areaName);
    }
    return;
  }

  for (auto& [y, idx] : mainRows_) {
    if (ev.y != y) continue;
    if (rightClick) {
      showActionsMenu(idx);
    } else if (ev.bstate & BUTTON1_DOUBLE_CLICKED) {
      pick_ = idx;
      handle('\n');
    } else if (pick_ == idx) {
      handle('\n');  // clicking the already-selected row opens/edits it
    } else {
      pick_ = idx;
    }
    return;
  }
}

void App::handle(int k) {
  if (visual_) {
    if (k == 27 || k == 'v') {
      visual_ = false;
      return;
    }
    if (k == 'j' || k == KEY_DOWN) {
      pick_ = std::min(pick_ + 1, std::max(0, (int)list_.size() - 1));
      return;
    }
    if (k == 'k' || k == KEY_UP) {
      pick_ = std::max(0, pick_ - 1);
      return;
    }
    if (k == 'x') { bulkComplete(); return; }
    if (k == 'm') { bulkMove(); return; }
    if (k == 'T') { bulkTag(); return; }
    if (k == 's') { bulkSetDate(false); return; }
    if (k == 'S') { bulkSetDate(true); return; }
    return;
  }
  if (k == 'q') { on_ = false; return; }
  if (k == 'j' || k == KEY_DOWN) { pick_ = std::min(pick_ + 1, std::max(0, (int)list_.size() - 1)); return; }
  if (k == 'k' || k == KEY_UP) { pick_ = std::max(0, pick_ - 1); return; }
  if ((k == 'h' || k == KEY_LEFT || k == 27) && scopeKind_ != 0) { scopeKind_ = 0; pick_ = 0; return; }
  if (k == 'h' || k == KEY_LEFT) {
    view_ = (view_ + (int)views_.size() - 1) % views_.size();
    hidden_.clear();
    pick_ = 0;
    return;
  }
  if (k == 'l' || k == KEY_RIGHT) {
    if (scopeKind_ != 0) return;
    view_ = (view_ + 1) % views_.size();
    hidden_.clear();
    pick_ = 0;
    return;
  }
  if (k == 'J' && !list_.empty() && pick_ + 1 < (int)list_.size()) { moveItem(1); return; }
  if (k == 'K' && !list_.empty() && pick_ > 0) { moveItem(-1); return; }
  if (k == 'b') { sidebar_ = !sidebar_; return; }
  if (k == 'v' && !list_.empty() && list_[pick_].kind == 't') {
    visual_ = true;
    visualAnchor_ = pick_;
    return;
  }
  if (k == 'n') {
    std::vector<PickerItem> opts = {
        {"\xe2\x97\x8b", "New Task", "", 't', Item{}},
        {"\xe2\x97\x87", "New Project", "", 'p', Item{}},
        {"\xe2\x97\x88", "New Area", "", 'a', Item{}},
    };
    if (scopeKind_ == 'p') opts.push_back({"\xe2\x80\x94", "New Heading", "", 'h', Item{}});
    int r = runPicker("NEW\xe2\x80\xa6", opts);
    if (r < 0) return;
    switch (opts[r].kind) {
      case 't': {
        int presetHeading = 0, afterSort = -1;
        if (scopeKind_ == 'p' && !list_.empty()) {
          Item& sel = list_[pick_];
          if (sel.kind == 'h') presetHeading = sel.id;
          else if (sel.kind == 't') {
            presetHeading = sel.headingId;
            afterSort = sel.sortOrder;
          }
        }
        taskForm({}, presetHeading, afterSort);
        break;
      }
      case 'p': projectForm(); break;
      case 'a': areaForm(); break;
      case 'h': headingForm(); break;
    }
    return;
  }
  if (k == 'c' && !list_.empty() && list_[pick_].kind == 't') { checklistEditor(list_[pick_]); return; }
  if (k == 'f') { find(); return; }
  if (k == 'm' && !list_.empty() && list_[pick_].kind == 't') {
    int pid = pickProject("MOVE TO PROJECT");
    if (pid >= 0) s_.moveTask(list_[pick_].id, pid ? s_.projectAreaId(pid) : 0, pid);
    return;
  }
  if (k == 'T') { tagFilterForm(); return; }
  if (k == 'A') { group_ = !group_; return; }
  if (k == 'x' && !list_.empty()) {
    if (list_[pick_].kind == 't') s_.complete(list_[pick_]);
    else if (list_[pick_].kind != 'h') lifecycle(list_[pick_]);
    return;
  }
  if (k == 'X' && scopeKind_ != 0) {
    Item cur;
    cur.id = scope_;
    cur.kind = scopeKind_;
    cur.title = scopeName_;
    lifecycle(cur);
    return;
  }
  if (k == 'u' && !list_.empty() && (active() == "Logbook" || hidden_ == "Logged Projects" || hidden_ == "Archived Areas")) {
    s_.reopen(list_[pick_]);
    return;
  }
  if (k == 'd' && !list_.empty() && (active() == "Logbook" || hidden_ == "Logged Projects" || hidden_ == "Archived Areas")) {
    auto& item = list_[pick_];
    if (item.kind == 't') s_.erase(item);
    else if (confirmDialog("Delete \"" + item.title + "\" permanently?")) s_.erase(item);
    return;
  }
  if (k == 'e' && !list_.empty()) {
    if (list_[pick_].kind == 't') taskForm(list_[pick_]);
    else if (list_[pick_].kind == 'p') projectForm(list_[pick_]);
    else if (list_[pick_].kind == 'a') areaForm(list_[pick_]);
    return;
  }
  if ((k == '\n' || k == KEY_ENTER) && !list_.empty()) {
    auto& sel = list_[pick_];
    if (sel.kind == 'p') openContainer('p', sel.id, sel.title, sel.areaName);
    else if (sel.kind == 't') taskForm(sel);
    else if (sel.kind == 'h' && scopeKind_ == 'p') headingLifecycle(sel);
    return;
  }
  if (k == '?') { help(); return; }
}

// ---------------------------------------------------------------------------

void App::run() {
  setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  set_escdelay(25);
  curs_set(0);
  start_color();
  use_default_colors();
  init_pair(1, COLOR_CYAN, -1);
  init_pair(2, COLOR_RED, -1);
  init_pair(3, COLOR_YELLOW, -1);
  init_pair(4, COLORS >= 16 ? 8 : COLOR_WHITE, -1);
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
  mouseinterval(0);  // report clicks immediately rather than trying to pair them into one down+up event
  // Opt in to the Kitty keyboard protocol / xterm modifyOtherKeys reporting,
  // which is what lets Shift+Enter be told apart from plain Enter. Terminals
  // that don't understand this simply ignore it.
  fputs("\x1b[>1u", stdout);
  fflush(stdout);
  while (on_) {
    load();
    draw();
    int k = getch();
    if (k == KEY_MOUSE) handleMouse();
    else handle(k);
  }
  fputs("\x1b[<1u", stdout);
  fflush(stdout);
  endwin();
}
