#include "app.h"

#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "finder.h"
#include "form.h"
#include "input.h"

// ---------------------------------------------------------------------------
// link opening (ctrl+click on a task/project's title or notes -- see
// handleMouse and drawMain's descRowRange_)
// ---------------------------------------------------------------------------

namespace {
std::string firstUrl(const std::string &s) {
  for (const char *prefix : {"https://", "http://"}) {
    size_t p = s.find(prefix);
    if (p == std::string::npos)
      continue;
    size_t end = s.find_first_of(" \t\n\r)]}>\"'", p);
    return s.substr(p, end == std::string::npos ? std::string::npos : end - p);
  }
  return "";
}

void openUrl(const std::string &url) {
  if (url.empty())
    return;
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 1);
      dup2(devnull, 2);
    }
#if defined(__APPLE__)
    execlp("open", "open", url.c_str(), (char *)nullptr);
#else
    execlp("xdg-open", "xdg-open", url.c_str(), (char *)nullptr);
#endif
    _exit(127);
  }
}
} // namespace

// ---------------------------------------------------------------------------
// view state
// ---------------------------------------------------------------------------

std::string App::active() const {
  return hidden_.empty() ? views_[view_] : hidden_;
}

std::string App::name() const {
  if (scopeKind_ == 'p')
    return scopeAreaName_.empty() ? scopeName_
                                  : scopeAreaName_ + " / " + scopeName_;
  if (scopeKind_ == 'a')
    return scopeName_;
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
    auto &v = views_[view_];
    if (v == "Inbox")
      list_ = s_.viewInbox(tags_);
    else if (v == "Today")
      list_ = s_.viewDay(false, tags_);
    else if (v == "Upcoming")
      list_ = s_.viewUpcoming(tags_);
    else if (v == "Anytime")
      list_ = s_.viewAnytime(tags_);
    else if (v == "Someday")
      list_ = s_.viewSomeday(tags_);
    else if (v == "Logbook")
      list_ = s_.viewLogbook();
  }
  if (group_ && scopeKind_ == 0)
    applyGrouping();
  pick_ = std::clamp(pick_, 0, std::max(0, (int)list_.size() - 1));
  visualAnchor_ =
      std::clamp(visualAnchor_, 0, std::max(0, (int)list_.size() - 1));
}

// rank 0: no area, no project (sorts first). rank 1: has an area (grouped by
// area, then by project within it). rank 2: standalone project, no area.
GroupKey App::groupKeyFor(const Item &x) const {
  if (x.projectName.empty() && x.areaName.empty())
    return {0, "", ""};
  if (x.areaName.empty())
    return {2, x.projectName, ""};
  return {1, x.areaName, x.projectName};
}

void App::applyGrouping() {
  std::stable_sort(list_.begin(), list_.end(),
                   [this](const Item &a, const Item &b) {
                     GroupKey ka = groupKeyFor(a), kb = groupKeyFor(b);
                     if (ka.rank != kb.rank)
                       return ka.rank < kb.rank;
                     if (ka.primary != kb.primary)
                       return ka.primary < kb.primary;
                     return ka.secondary < kb.secondary;
                   });
}

std::string App::dateGroupLabel(const Item &x) const {
  if (scopeKind_ == 'a')
    return x.section;
  if (scopeKind_ == 'p')
    return "";
  std::string v = active();
  if (v == "Upcoming")
    return friendlyDate(x.doDate.empty() ? x.deadline : x.doDate);
  if (v == "Deadlines")
    return friendlyDate(x.deadline);
  if (v == "Logbook" || v == "Logged Projects" || v == "Archived Areas")
    return bucketLabel(x.completedAt);
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
  if (sidebar_)
    drawSidebar(rows, off);
  drawMain(rows, cols, off);
  refresh();
}

// Prints `label` wrapped to `width` columns starting at (y, x), returning
// the row after the last line printed. Every wrapped line (not just the
// first) is registered against `target` in sidebarRows_ so a click anywhere
// on a wrapped entry resolves to it, and every line but the last gets the
// reverse-video treatment together when `cur` is set.
int App::wrapSidebarEntry(int y, int x, int width, const std::string &label,
                          bool cur, SidebarTarget target) {
  auto lines = wrapText(label, std::max(1, width));
  for (auto &ln : lines) {
    if (cur)
      attron(A_REVERSE);
    mvprintw(y, x, "%s", clip(ln, width).c_str());
    if (cur)
      attroff(A_REVERSE);
    sidebarRows_.push_back({y, target});
    ++y;
  }
  return y;
}

void App::drawSidebar(int rows, int width) {
  sidebarRows_.clear();
  attron(A_BOLD | COLOR_PAIR(1));
  mvprintw(1, 2, "◉ STRIDE");
  attroff(A_BOLD | COLOR_PAIR(1));
  mvprintw(3, 2, "FOCUS");

  for (int i = 0; i < (int)views_.size(); ++i) {
    bool cur = scopeKind_ == 'v' && hidden_.empty() && i == view_;
    if (cur)
      attron(A_REVERSE);
    mvprintw(5 + i, 2, "%s", cur ? "▣" : "○");
    if (cur)
      attroff(A_REVERSE);
    wrapSidebarEntry(5 + i, 4, width - 6, views_[i], cur, {'v', i});
  }

  int y = 5 + (int)views_.size() + 1;
  mvprintw(y++, 2, "AREAS");

  for (auto &a : s_.areas()) {
    y++;
    if (y >= rows - 3)
      break;

    // Fixed: Use character literal 'a' instead of the object a
    bool cur = scopeKind_ == 'a' && scope_ == a.id;
    if (cur)
      attron(A_REVERSE);
    mvprintw(y, 2, "◆");
    if (cur)
      attroff(A_REVERSE);

    // Fixed: Use character literal 'a'
    y = wrapSidebarEntry(y, 4, width - 6, a.name, cur, {'a', a.id});

    for (auto &p : s_.projectsInArea(a.id, /*excludeSomeday=*/true)) {
      if (y >= rows - 3)
        break;

      // Fixed: Use character literal 'p' instead of the object p
      bool curp = scopeKind_ == 'p' && scope_ == p.id;
      if (curp)
        attron(A_REVERSE);
      mvprintw(y, 4, "▹");
      if (curp)
        attroff(A_REVERSE);

      // Fixed: Use character literal 'p'
      y = wrapSidebarEntry(y, 6, width - 8, p.name, curp, {'p', p.id});
    }
  }

  if (y < rows - 3)
    y++;

  for (auto &p : s_.projects(/*archived=*/false, /*excludeSomeday=*/true)) {
    if (!p.sub.empty())
      continue;
    if (y >= rows - 3)
      break;

    // Fixed: Use character literal 'p' instead of the object p
    bool curp = scopeKind_ == 'p' && scope_ == p.id;
    if (curp)
      attron(A_REVERSE);
    mvprintw(y, 2, "◇");
    if (curp)
      attroff(A_REVERSE);

    // Fixed: Use character literal 'p'
    y = wrapSidebarEntry(y, 4, width - 6, p.name, curp, {'p', p.id});
  }

  attron(A_DIM);
  mvprintw(rows - 2, 2, "%s", clip("? for shortcuts", width - 4).c_str());
  attroff(A_DIM);
  mvvline(0, width, ACS_VLINE, rows);
}

bool App::rowSelected(int index) const {
  if (!visual_)
    return index == pick_;
  int lo = std::min(visualAnchor_, pick_), hi = std::max(visualAnchor_, pick_);
  return index >= lo && index <= hi;
}

void App::drawIconStrip(int y, int cols, const Item &x) {
  int stripW = 22;
  int cx = cols - stripW;
  if (cx < 0)
    return;
  std::string tdy = today();
  if (x.deadline.size() >= 10) {
    bool overdue = x.deadline <= tdy;
    if (overdue)
      attron(COLOR_PAIR(2));
    mvprintw(y, cx, "\xe2\x9a\x91%s", x.deadline.substr(5, 5).c_str());
    if (overdue)
      attroff(COLOR_PAIR(2));
  }
  if (x.doDate.size() >= 10)
    mvprintw(y, cx + 8, "\xe2\x97\xb7%s", x.doDate.substr(5, 5).c_str());
  if (!x.notes.empty())
    mvprintw(y, cx + 16, "\xe2\x89\xa1");
  if (!parseChecklist(x.checklist).empty())
    mvprintw(y, cx + 18, "\xe2\x98\x91");
  if (!x.tags.empty())
    mvprintw(y, cx + 20, "#");
}

void App::drawMain(int rows, int cols, int off) {
  mainRows_.clear();
  descRowRange_ = {0, 0};
  int innerW = cols - off - 4;
  attron(A_BOLD);
  mvprintw(1, off + 3, "%s", clip(name(), innerW - 20).c_str());
  attroff(A_BOLD);
  if (!tags_.empty()) {
    attron(A_DIM);
    std::string tagLabel = clip("tags: " + tags_, 24);
    mvprintw(1, std::max(off + 3, cols - (int)tagLabel.size() - 2), "%s",
             tagLabel.c_str());
    attroff(A_DIM);
  }
  int headerRow = 2;
  if (scopeKind_ == 'p' && !scopeDescription_.empty()) {
    auto lines = wrapText(scopeDescription_, innerW);
    // No more hard 3-line cap -- grow to fit, bounded only by leaving
    // reasonable room for the list itself on very long descriptions.
    int maxDescLines = std::max(1, (rows - 8) / 2);
    int shown = std::min((int)lines.size(), maxDescLines);
    attron(A_DIM);
    for (int i = 0; i < shown; ++i)
      mvprintw(2 + i, off + 3, "%s", clip(lines[i], innerW).c_str());
    attroff(A_DIM);
    headerRow = 2 + shown;
    descRowRange_ = {2, headerRow};
  }
  mvhline(headerRow, off + 2, ACS_HLINE, std::max(0, cols - off - 4));

  if (list_.empty()) {
    attron(A_DIM);
    mvprintw(
        headerRow + 2, off + 4, "%s",
        clip("Nothing here yet. n captures a next action.", innerW).c_str());
    attroff(A_DIM);
  }

  int contentTop = headerRow + 2;
  int viewH = std::max(0, (rows - 2) - contentTop);
  auto lines = buildMainLines(cols, off);
  updateScroll(lines, viewH);

  for (int ln = scroll_; ln < (int)lines.size() && ln - scroll_ < viewH; ++ln) {
    auto &mline = lines[ln];
    int y = contentTop + (ln - scroll_);
    if (mline.header) {
      if (!mline.text.empty()) {
        bool secondary = mline.indent != off + 3;
        if (secondary)
          attron(COLOR_PAIR(3));
        else
          attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(y, mline.indent, "%s",
                 clip(mline.text, cols - mline.indent - 2).c_str());
        if (secondary)
          attroff(COLOR_PAIR(3));
        else
          attroff(A_BOLD | COLOR_PAIR(3));
      }
      continue;
    }
    auto &x = list_[mline.itemIndex];
    bool selected = rowSelected(mline.itemIndex);
    mainRows_.push_back({y, mline.itemIndex});
    if (selected)
      attron(A_REVERSE);
    if (x.kind == 'h') {
      attron(A_BOLD);
      mvprintw(y, mline.indent, "%s",
               clip(mline.text, cols - mline.indent - 2).c_str());
      attroff(A_BOLD);
    } else {
      bool projSomeday = x.kind == 'p' && x.doDate == "someday";
      bool graySomeday = (x.someday || projSomeday) && x.status == "open";
      bool dim = (x.status != "open") || graySomeday;
      if (graySomeday)
        attron(COLOR_PAIR(4));
      else if (dim)
        attron(A_DIM);
      mvprintw(y, mline.indent, "%s",
               clip(mline.text, std::max(1, cols - mline.indent - 23)).c_str());
      if (graySomeday)
        attroff(COLOR_PAIR(4));
      else if (dim)
        attroff(A_DIM);
      if (mline.firstOfItem)
        drawIconStrip(y, cols, x);
    }
    if (selected)
      attroff(A_REVERSE);
  }
  attron(A_DIM);
  mvprintw(rows - 1, off + 3, "%s",
           clip(visual_
                    ? "VISUAL -- j/k extend \xc2\xb7 x complete \xc2\xb7 m "
                      "move \xc2\xb7 T tag \xc2\xb7 s/S dates \xc2\xb7 Esc exit"
                    : "? for shortcuts",
                std::max(0, cols - off - 5))
               .c_str());
  attroff(A_DIM);
}

// Builds the full virtual layout of the main list -- one MainLine per
// physical screen line the list would occupy if the screen were infinitely
// tall, including wrapped title/heading continuation lines and group/date
// headers (with a blank separator line above each, per requirement #4). The
// draw pass and the scroll-offset calculation both work off this same list,
// so wrapping and scrolling can never disagree about where anything sits.
std::vector<MainLine> App::buildMainLines(int cols, int off) const {
  std::vector<MainLine> out;
  int innerW = cols - off - 4;
  bool twoLevel = group_ && scopeKind_ == 0;
  std::string lastPrimary, lastSecondary;
  bool primarySet = false;
  auto pushHeader = [&](const std::string &text, int indent) {
    if (!out.empty())
      out.push_back({true, -1, false, indent, ""});
    for (auto &ln : wrapText(text, std::max(1, innerW - (indent - off - 3))))
      out.push_back({true, -1, false, indent, ln});
  };
  for (int i = 0; i < (int)list_.size(); ++i) {
    auto &x = list_[i];
    if (twoLevel) {
      GroupKey gk = groupKeyFor(x);
      if (!primarySet || gk.primary != lastPrimary) {
        if (!gk.primary.empty())
          pushHeader(gk.primary, off + 3);
        lastPrimary = gk.primary;
        lastSecondary.clear();
        primarySet = true;
      }
      if (!gk.secondary.empty() && gk.secondary != lastSecondary) {
        for (auto &ln : wrapText(gk.secondary, std::max(1, innerW - 2)))
          out.push_back({true, -1, false, off + 5, ln});
        lastSecondary = gk.secondary;
      }
    } else {
      std::string grp = dateGroupLabel(x);
      if (!grp.empty() && grp != lastPrimary) {
        pushHeader(grp, off + 3);
        lastPrimary = grp;
      }
    }
    int indent = off + 4;
    if (scopeKind_ == 'p' && x.kind == 't' && x.headingId != 0)
      indent += 2;

    // --- ADDED: Push a blank separator line before headings ---
    if (x.kind == 'h' && !out.empty()) {
      // {isHeader = true, index = -1, isFirstLine = false, indent, text = ""}
      out.push_back({true, -1, false, indent, ""});
    }

    std::string full;
    if (x.kind == 'h') {
      full = "\xe2\x80\x94 " + x.title;
    } else {
      std::string icon = x.kind == 'p'   ? "\xe2\x97\x87"
                         : x.kind == 'a' ? "\xe2\x97\x88"
                                         : "\xe2\x97\x8b";
      std::string mark = x.status != "open" ? "\xe2\x9c\x93 " : "";
      full = icon + " " + mark + x.title;
    }
    int wrapWidth = std::max(1, cols - indent - 2 - 23);
    auto wrapped = wrapText(full, wrapWidth);
    for (int li = 0; li < (int)wrapped.size(); ++li)
      out.push_back(
          {false, i, li == 0, li == 0 ? indent : indent + 2, wrapped[li]});
  }
  return out;
}

// Adjusts scroll_ (persistent across frames, so it only resets where the
// interaction actually warrants it -- see the pick_/scroll_ resets in
// handle()/openContainer()/etc.) so the selected row stays visible:
// scrolloff-style margin correction for ordinary single-step movement, and
// a "land roughly a third of the way down, with 3-4 rows of context below"
// recenter when the target is a genuine jump to somewhere off-screen. Both
// are clamped to the list's own bounds, which is what naturally keeps the
// last couple of entries from being centered -- there's nothing below them
// to leave room for.
void App::updateScroll(const std::vector<MainLine> &lines, int viewH) {
  if (list_.empty() || viewH <= 0) {
    scroll_ = 0;
    return;
  }
  int targetStart = -1;
  for (int ln = 0; ln < (int)lines.size(); ++ln)
    if (!lines[ln].header && lines[ln].itemIndex == pick_) {
      targetStart = ln;
      break;
    }
  if (targetStart < 0) {
    scroll_ = 0;
    return;
  }
  int totalLines = (int)lines.size();
  int maxScroll = std::max(0, totalLines - viewH);
  const int margin = 3;
  if (targetStart < scroll_ || targetStart >= scroll_ + viewH) {
    scroll_ = targetStart - std::max(0, viewH - 4);
  } else if (targetStart - scroll_ < margin) {
    scroll_ = targetStart - margin;
  } else if (targetStart - scroll_ > viewH - 1 - margin) {
    scroll_ = targetStart - (viewH - 1 - margin);
  }
  scroll_ = std::clamp(scroll_, 0, maxScroll);
}

// ---------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------

int App::findAreaId(const std::string &name) {
  std::string n = lower(trimmed(name));
  if (n.empty())
    return 0;
  for (auto &a : s_.areas())
    if (lower(a.name) == n)
      return a.id;
  return 0;
}
int App::findProjectId(const std::string &name) {
  std::string n = lower(trimmed(name));
  if (n.empty())
    return 0;
  for (auto &p : s_.projects())
    if (lower(p.name) == n)
      return p.id;
  return 0;
}

// ---------------------------------------------------------------------------
// forms
// ---------------------------------------------------------------------------

void App::taskForm(std::optional<Item> e, int presetHeadingId,
                   int insertAfterSortOrder) {
  Item t = e.value_or(Item{});
  std::string areaDefault = t.areaName, projectDefault = t.projectName;
  if (!t.id) {
    if (scopeKind_ == 'a')
      areaDefault = scopeName_;
    else if (scopeKind_ == 'p') {
      projectDefault = scopeName_;
      areaDefault = scopeAreaName_;
    }
  }
  std::string doDateDefault = t.someday ? "someday" : t.doDate;
  // New task, no explicit date yet, and we're looking at Today/Tomorrow:
  // assume that's the day it's meant for.
  if (!t.id && doDateDefault.empty() && scopeKind_ == 0) {
    if (hidden_.empty() && views_[view_] == "Today")
      doDateDefault = today();
    else if (hidden_ == "Tomorrow")
      doDateDefault = todayPlus(1);
  }
  int w = 70, valueX = 15, valueW = w - valueX - 3;
  // Height is sized from existing content when editing; for a brand-new
  // item there's nothing to measure yet, so start with enough room for a
  // couple of lines rather than 1 -- editArea's own vertical scrolling
  // still takes over gracefully if it's typed past that.
  int titleH = std::clamp((int)wrapText(t.title, valueW).size(), 2, 6);
  int notesH = std::clamp((int)wrapText(t.notes, valueW).size(), 2, 12);
  std::vector<FormField> fields = {
      {"Title", t.title, true, titleH, true},
      {"Description", t.notes, true, notesH},
      {"Tags", t.tags, false, 1},
      {"Do date", doDateDefault, false, 1},
      {"Deadline", t.deadline, false, 1},
      {"Area", areaDefault, false, 1},
      {"Project", projectDefault, false, 1},
  };
  int startY = 3, labelX = 2;
  int footerY = startY;
  for (auto &f : fields)
    footerY += f.multiline ? std::max(1, f.height) : 1;
  int scrRows, scrCols;
  getmaxyx(stdscr, scrRows, scrCols);
  (void)scrCols;
  int h = std::clamp(footerY + 3, 10, std::max(10, scrRows - 2));
  WINDOW *win = openDialog(h, w, t.id ? "EDIT TASK" : "NEW TASK");
  getmaxyx(win, h, w);
  wattron(win, A_DIM);
  mvwprintw(win, footerY + 1, 2, "%s",
            clip("YYYY-MM-DD or 'someday'  \xc2\xb7  Tab/Enter next \xc2\xb7 "
                 "Shift+Enter save \xc2\xb7 Esc cancel",
                 w - 4)
                .c_str());
  wattroff(win, A_DIM);
  wrefresh(win);
  FormResult r = runForm(win, fields, startY, labelX, valueX, valueW);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty())
    return;
  t.title = fields[0].value;
  t.notes = fields[1].value;
  t.tags = fields[2].value;
  t.doDate = fields[3].value;
  t.deadline = fields[4].value;
  int areaId = findAreaId(fields[5].value);
  int projectId = findProjectId(fields[6].value);
  if (projectId && !areaId)
    areaId = s_.projectAreaId(projectId);
  bool wasNew = t.id == 0;
  int newId = s_.saveTask(t, areaId, projectId);
  if (wasNew) {
    if (presetHeadingId)
      s_.setTaskHeading(newId, presetHeadingId);
    if (insertAfterSortOrder >= 0)
      s_.insertTaskAfter(newId, insertAfterSortOrder);
  }
}

void App::projectForm(std::optional<Item> e) {
  Item p = e.value_or(Item{});
  std::string areaDefault = p.areaName;
  if (!p.id && scopeKind_ == 'a')
    areaDefault = scopeName_;
  int w = 70, valueX = 15, valueW = w - valueX - 3;
  int titleH = std::clamp((int)wrapText(p.title, valueW).size(), 2, 6);
  int notesH = std::clamp((int)wrapText(p.notes, valueW).size(), 2, 12);
  std::vector<FormField> fields = {
      {"Name", p.title, true, titleH, true},
      {"Description", p.notes, true, notesH},
      {"Area", areaDefault, false, 1},
      {"Do date", p.doDate, false, 1},
      {"Deadline", p.deadline, false, 1},
  };
  int startY = 3, labelX = 2;
  int footerY = startY;
  for (auto &f : fields)
    footerY += f.multiline ? std::max(1, f.height) : 1;
  int scrRows, scrCols;
  getmaxyx(stdscr, scrRows, scrCols);
  (void)scrCols;
  int h = std::clamp(footerY + 3, 9, std::max(9, scrRows - 2));
  WINDOW *win = openDialog(h, w, p.id ? "EDIT PROJECT" : "NEW PROJECT");
  getmaxyx(win, h, w);
  wattron(win, A_DIM);
  mvwprintw(win, footerY + 1, 2, "%s",
            clip("Do date: YYYY-MM-DD or 'someday'  \xc2\xb7  Tab/Enter next "
                 "\xc2\xb7 Shift+Enter save \xc2\xb7 Esc cancel",
                 w - 4)
                .c_str());
  wattroff(win, A_DIM);
  wrefresh(win);
  FormResult r = runForm(win, fields, startY, labelX, valueX, valueW);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty())
    return;
  p.title = fields[0].value;
  p.notes = fields[1].value;
  p.doDate = fields[3].value;
  p.deadline = fields[4].value;
  s_.saveProject(p, findAreaId(fields[2].value));
}

void App::areaForm(std::optional<Item> e) {
  Item a = e.value_or(Item{});
  int w = 50, valueX = 10, valueW = w - valueX - 3;
  int nameH = std::clamp((int)wrapText(a.title, valueW).size(), 2, 5);
  int h = std::clamp(nameH + 5, 6, 20);
  WINDOW *win = openDialog(h, w, a.id ? "RENAME AREA" : "NEW AREA");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {{"Name", a.title, true, nameH, true}};
  FormResult r = runForm(win, fields, 3, 2, valueX, valueW);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty())
    return;
  if (a.id)
    s_.renameArea(a.id, fields[0].value);
  else
    s_.addArea(fields[0].value);
}

void App::headingForm() {
  int w = 48, valueX = 10, valueW = w - valueX - 3, h = 6;
  WINDOW *win = openDialog(h, w, "NEW HEADING");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {{"Title", "", true, 1, true}};
  FormResult r = runForm(win, fields, 3, 2, valueX, valueW);
  delwin(win);
  if (r == FormResult::Cancelled || trimmed(fields[0].value).empty())
    return;
  s_.addHeading(scope_, fields[0].value);
}

void App::headingLifecycle(const Item &heading) {
  int w = 46, h = 6;
  WINDOW *win = openDialog(h, w, "HEADING");
  getmaxyx(win, h, w);
  mvwprintw(win, 3, 2, "%s",
            clip("r rename   d delete   Esc cancel", w - 4).c_str());
  wrefresh(win);
  KeyEvent k = readKey(win);
  delwin(win);
  if (k.type != Key::Char)
    return;
  if (k.ch == 'r') {
    int w2 = 48, h2 = 6;
    WINDOW *win2 = openDialog(h2, w2, "RENAME HEADING");
    getmaxyx(win2, h2, w2);
    std::vector<FormField> fields = {{"Title", heading.title, false, 1}};
    FormResult r = runForm(win2, fields, 3, 2, 10, w2 - 13);
    delwin(win2);
    if (r == FormResult::Saved && !trimmed(fields[0].value).empty())
      s_.renameHeading(heading.id, fields[0].value);
  } else if (k.ch == 'd') {
    if (confirmDialog("Delete heading \"" + heading.title +
                      "\"? Tasks stay, just ungrouped."))
      s_.deleteHeading(heading.id);
  }
}

void App::tagFilterForm() { tags_ = runTagPicker(s_.allTags(), tags_); }

void App::lifecycle(const Item &i) {
  int w = 46, h = 6;
  WINDOW *win = openDialog(h, w, i.kind == 'p' ? "PROJECT" : "AREA");
  getmaxyx(win, h, w);
  mvwprintw(win, 3, 2, "%s",
            clip("c complete   x cancel   d delete", w - 4).c_str());
  wrefresh(win);
  KeyEvent k = readKey(win);
  delwin(win);
  if (k.type != Key::Char)
    return;
  if (k.ch == 'c')
    s_.complete(i);
  else if (k.ch == 'x')
    s_.cancel(i);
  else if (k.ch == 'd' &&
           confirmDialog("Delete \"" + i.title + "\" permanently?")) {
    s_.erase(i);
    if (scopeKind_ == i.kind && scope_ == i.id) {
      scopeKind_ = 0;
      pick_ = 0;
      scroll_ = 0;
    }
  }
}

void App::checklistEditor(Item t) {
  auto items = parseChecklist(t.checklist);
  int pick = 0;
  bool dirty = false;
  int scrRows, scrCols;
  getmaxyx(stdscr, scrRows, scrCols);
  (void)scrCols;
  while (true) {
    int w = 56;
    int innerW = w - 6;
    std::vector<std::vector<std::string>> wrapped(items.size());
    int totalLines = 0;
    for (size_t i = 0; i < items.size(); ++i) {
      wrapped[i] = wrapText(items[i].text, innerW);
      totalLines += (int)wrapped[i].size();
    }
    int h = std::clamp(totalLines + 6, 8, std::max(8, scrRows - 2));
    WINDOW *win = openDialog(h, w, "CHECKLIST: " + t.title);
    getmaxyx(win, h, w);
    int maxRows = h - 6;
    int y = 3;
    for (int i = 0; i < (int)items.size() && y - 3 < maxRows; ++i) {
      bool sel = i == pick;
      if (sel)
        wattron(win, A_REVERSE);
      for (int li = 0; li < (int)wrapped[i].size() && y - 3 < maxRows; ++li) {
        if (li == 0)
          mvwprintw(win, y, 2, "%s %s",
                    items[i].done ? "\xe2\x98\x91" : "\xe2\x98\x90",
                    wrapped[i][li].c_str());
        else
          mvwprintw(win, y, 4, "%s", wrapped[i][li].c_str());
        ++y;
      }
      if (sel)
        wattroff(win, A_REVERSE);
    }
    if (items.empty()) {
      wattron(win, A_DIM);
      mvwprintw(win, 3, 2, "No items yet.");
      wattroff(win, A_DIM);
    }
    wattron(win, A_DIM);
    mvwprintw(win, h - 2, 2, "%s",
              clip("n add \xc2\xb7 e edit \xc2\xb7 Enter/space toggle \xc2\xb7 "
                   "d delete \xc2\xb7 Esc close",
                   w - 4)
                  .c_str());
    wattroff(win, A_DIM);
    wrefresh(win);
    KeyEvent k = readKey(win);
    bool closing = false;
    if (k.type == Key::Escape)
      closing = true;
    else if (k.type == Key::Down || (k.type == Key::Char && k.ch == 'j'))
      pick = std::min(pick + 1, std::max(0, (int)items.size() - 1));
    else if (k.type == Key::Up || (k.type == Key::Char && k.ch == 'k'))
      pick = std::max(pick - 1, 0);
    else if ((k.type == Key::Enter || (k.type == Key::Char && k.ch == ' ')) &&
             !items.empty()) {
      items[pick].done = !items[pick].done;
      dirty = true;
    } else if (k.type == Key::Char && k.ch == 'd' && !items.empty()) {
      items.erase(items.begin() + pick);
      pick = std::min(pick, std::max(0, (int)items.size() - 1));
      dirty = true;
    } else if (k.type == Key::Char && k.ch == 'e' && !items.empty()) {
      delwin(win);
      int w2 = 52;
      int itemH =
          std::clamp((int)wrapText(items[pick].text, w2 - 11).size(), 1, 6);
      int h2 = itemH + 4;
      WINDOW *win2 = openDialog(h2, w2, "EDIT ITEM");
      getmaxyx(win2, h2, w2);
      std::vector<FormField> fields = {
          {"Item", items[pick].text, true, itemH, true}};
      FormResult r = runForm(win2, fields, 3, 2, 8, w2 - 11);
      delwin(win2);
      if (r == FormResult::Saved && !trimmed(fields[0].value).empty()) {
        items[pick].text = fields[0].value;
        dirty = true;
      }
      continue;
    } else if (k.type == Key::Char && k.ch == 'n') {
      delwin(win);
      int w2 = 52, h2 = 5;
      WINDOW *win2 = openDialog(h2, w2, "NEW ITEM");
      getmaxyx(win2, h2, w2);
      std::vector<FormField> fields = {{"Item", "", true, 1, true}};
      FormResult r = runForm(win2, fields, 3, 2, 8, w2 - 11);
      delwin(win2);
      if (r == FormResult::Saved && !trimmed(fields[0].value).empty()) {
        items.push_back({false, fields[0].value});
        dirty = true;
      }
      continue;
    }
    delwin(win);
    if (closing)
      break;
  }
  if (dirty) {
    t.checklist = serializeChecklist(items);
    s_.saveTask(t, t.areaId, t.projectId);
  }
}

void App::help() {
  std::vector<std::string> lines = {
      "Mouse           click to select/navigate \xc2\xb7 click selected row to "
      "open",
      "                right-click any row for its actions menu \xc2\xb7 "
      "ctrl+click a",
      "                link in a title/description to open it",
      "",
      "j/k, arrows    move selection",
      "h/l, arrows    switch list / go back (also works inside a project/area)",
      "J/K            reorder (crosses into a heading)",
      "Enter          edit task \xc2\xb7 open project \xc2\xb7 rename/delete "
      "heading",
      "n              new... (task / project / area / heading)",
      "c              edit the selected task's checklist (e to edit an item)",
      "e              edit selected item",
      "x              complete task, or lifecycle for project/area",
      "X              lifecycle for the project/area you're inside",
      "m              move task to another project",
      "f              fuzzy find & jump anywhere",
      "T              filter by tags (fuzzy-searches existing tags)",
      "A              group by area / project",
      "v              visual mode: select a range for bulk actions",
      "u              revive (un-complete/un-cancel) in logbook/archives",
      "d              delete the selected task, or (in logbook/archives) a",
      "               project/area, permanently -- always asks to confirm",
      "b              toggle sidebar",
      "q              quit",
  };
  int scrRows, scrCols;
  getmaxyx(stdscr, scrRows, scrCols);
  int w = std::min(58, std::max(24, scrCols - 4));
  std::vector<std::string> wrapped;
  for (auto &l : lines)
    for (auto &wl : wrapText(l, w - 4))
      wrapped.push_back(wl);
  int h = std::clamp((int)wrapped.size() + 4, 6, std::max(6, scrRows - 2));
  WINDOW *win = openDialog(h, w, "SHORTCUTS");
  getmaxyx(win, h, w);
  for (int i = 0; i < (int)wrapped.size() && 3 + i < h - 1; ++i)
    mvwprintw(win, 3 + i, 2, "%s", clip(wrapped[i], w - 4).c_str());
  wrefresh(win);
  readKey(win);
  delwin(win);
}

// ---------------------------------------------------------------------------
// navigation / finder
// ---------------------------------------------------------------------------

void App::openContainer(char kind, int id, const std::string &title,
                        const std::string &sub) {
  scopeKind_ = kind;
  scope_ = id;
  scopeName_ = title;
  scopeAreaName_ = sub;
  hidden_.clear();
  pick_ = 0;
  scroll_ = 0;
}

std::string App::homeViewFor(const Item &t) const {
  std::string tdy = today();
  if ((!t.doDate.empty() && t.doDate <= tdy) ||
      (!t.deadline.empty() && t.deadline <= tdy))
    return "Today";
  if (!t.doDate.empty())
    return "Upcoming";
  if (t.someday)
    return "Someday";
  if (!t.deadline.empty())
    return "Deadlines";
  return "Inbox";
}

void App::jumpToTask(const Item &t) {
  if (t.projectId) {
    openContainer('p', t.projectId, t.projectName, t.areaName);
  } else if (t.areaId) {
    openContainer('a', t.areaId, t.areaName, "");
  } else {
    scopeKind_ = 0;
    scroll_ = 0;
    std::string v = homeViewFor(t);
    if (v == "Tomorrow" || v == "Deadlines" || v == "Logged Projects" ||
        v == "Archived Areas") {
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
  for (auto &v : views_)
    items.push_back({"\xe2\x96\xa3", v, "List", 'v', Item{}});
  for (std::string v :
       {"Tomorrow", "Deadlines", "Logged Projects", "Archived Areas"})
    items.push_back({"\xe2\x96\xa3", v, "List", 'H', Item{}});
  for (auto &idx : s_.searchIndex()) {
    if (idx.kind == 'a')
      items.push_back({"\xe2\x97\x88", idx.title, "Area", 'a', idx});
    else if (idx.kind == 'p')
      items.push_back({"\xe2\x97\x87", idx.title,
                       idx.areaName.empty()
                           ? "Project"
                           : "Project \xc2\xb7 " + idx.areaName,
                       'p', idx});
    else {
      std::string sub = !idx.projectName.empty() ? idx.projectName
                        : !idx.areaName.empty()  ? idx.areaName
                        : idx.someday            ? "Someday"
                                                 : "Task";
      items.push_back({"\xe2\x97\x8b", idx.title, sub, 't', idx});
    }
  }
  int sel = runPicker("FIND", items);
  if (sel < 0)
    return;
  auto &c = items[sel];
  if (c.kind == 'v') {
    scopeKind_ = 0;
    hidden_.clear();
    scroll_ = 0;
    auto it = std::find(views_.begin(), views_.end(), c.label);
    view_ = it != views_.end() ? (int)(it - views_.begin()) : 0;
  } else if (c.kind == 'H') {
    scopeKind_ = 0;
    hidden_ = c.label;
    scroll_ = 0;
  } else if (c.kind == 'a') {
    openContainer('a', c.raw.id, c.raw.title, "");
  } else if (c.kind == 'p') {
    openContainer('p', c.raw.id, c.raw.title, c.raw.areaName);
  } else if (c.kind == 't') {
    jumpToTask(c.raw);
  }
}

int App::pickProject(const std::string &title) {
  std::vector<PickerItem> items;
  Item inbox;
  items.push_back({"\xe2\x97\x87", "Inbox (no project)", "", 'p', inbox});
  for (auto &p : s_.projects()) {
    Item raw;
    raw.id = p.id;
    items.push_back({"\xe2\x97\x87", p.name,
                     p.sub.empty() ? "Project" : "Project \xc2\xb7 " + p.sub,
                     'p', raw});
  }
  int sel = runPicker(title, items);
  return sel < 0 ? -1 : items[sel].raw.id;
}

// ---------------------------------------------------------------------------
// reordering
// ---------------------------------------------------------------------------

void App::moveItem(int delta) {
  int i = pick_, j = pick_ + delta;
  if (j < 0 || j >= (int)list_.size())
    return;
  Item &a = list_[i];
  Item &b = list_[j];
  if (scopeKind_ == 'p' && a.kind == 't' && b.kind == 'h')
    s_.setTaskHeading(a.id, b.id);
  s_.swapOrder(a, b);
  pick_ = j;
}

// ---------------------------------------------------------------------------
// visual mode / bulk actions
// ---------------------------------------------------------------------------

std::vector<int> App::selectedIndices() const {
  std::vector<int> out;
  if (!visual_) {
    if (!list_.empty() && list_[pick_].kind == 't')
      out.push_back(pick_);
    return out;
  }
  int lo = std::min(visualAnchor_, pick_), hi = std::max(visualAnchor_, pick_);
  for (int i = lo; i <= hi && i < (int)list_.size(); ++i)
    if (list_[i].kind == 't')
      out.push_back(i);
  return out;
}

void App::bulkComplete() {
  for (int i : selectedIndices())
    s_.complete(list_[i]);
  visual_ = false;
}

void App::bulkMove() {
  auto idxs = selectedIndices();
  visual_ = false;
  if (idxs.empty())
    return;
  int pid = pickProject("MOVE TO PROJECT");
  if (pid < 0)
    return;
  for (int i : idxs)
    s_.moveTask(list_[i].id, pid ? s_.projectAreaId(pid) : 0, pid);
}

void App::bulkTag() {
  auto idxs = selectedIndices();
  visual_ = false;
  if (idxs.empty())
    return;
  std::string chosen = runTagPicker(s_.allTags(), "");
  if (chosen.empty() || chosen == "none")
    return;
  for (int i : idxs) {
    Item t = list_[i];
    auto tags = splitComma(t.tags);
    for (auto &nt : splitComma(chosen)) {
      std::string nv = trimmed(nt);
      if (nv.empty())
        continue;
      bool exists = false;
      for (auto &e : tags)
        if (lower(trimmed(e)) == lower(nv))
          exists = true;
      if (!exists)
        tags.push_back(nv);
    }
    std::string merged;
    for (auto &tg : tags) {
      if (!merged.empty())
        merged += ",";
      merged += trimmed(tg);
    }
    t.tags = merged;
    s_.saveTask(t, t.areaId, t.projectId);
  }
}

void App::bulkSetDate(bool deadline) {
  auto idxs = selectedIndices();
  visual_ = false;
  if (idxs.empty())
    return;
  int w = 50, h = 6;
  WINDOW *win = openDialog(h, w, deadline ? "SET DEADLINE" : "SET DO DATE");
  getmaxyx(win, h, w);
  std::vector<FormField> fields = {{"Date", "", false, 1}};
  FormResult r = runForm(win, fields, 3, 2, 10, w - 13);
  delwin(win);
  if (r != FormResult::Saved || trimmed(fields[0].value).empty())
    return;
  for (int i : idxs) {
    Item t = list_[i];
    if (deadline)
      t.deadline = fields[0].value;
    else
      t.doDate = fields[0].value;
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
  if (index < 0 || index >= (int)list_.size())
    return;
  pick_ = index;
  showActionsMenuFor(list_[index]);
}

void App::showActionsMenuFor(const Item &x) {
  bool open = x.status == "open";

  std::vector<PickerItem> opts;
  auto add = [&](const char *icon, const std::string &label, char code) {
    opts.push_back({icon, label, "", code, x});
  };

  if (x.kind == 't') {
    add(open ? "\xe2\x9c\x93" : "\xe2\x86\xba", open ? "Complete" : "Reopen",
        'x');
    add("\xe2\x9c\x8e", "Edit...", 'e');
    add("\xe2\x86\x92", "Move to project...", 'm');
    add("\xe2\x98\x91", "Edit checklist...", 'c');
    add("\xf0\x9f\x97\x91", "Delete permanently", 'd');
  } else if (x.kind == 'p' || x.kind == 'a') {
    if (x.kind == 'p')
      add("\xe2\x86\xb5", "Open", '\n');
    add("\xe2\x9c\x8e", "Edit...", 'e');
    if (open)
      add("\xe2\x9a\x99", "Complete / cancel / delete...", 'L');
    else {
      add("\xe2\x86\xba", "Reopen", 'x');
      add("\xf0\x9f\x97\x91", "Delete permanently", 'd');
    }
  } else if (x.kind == 'h') {
    add("\xe2\x9c\x8e", "Rename / delete...", 'H');
  }
  if (opts.empty())
    return;

  int r = runPicker(x.title, opts);
  if (r < 0)
    return;
  switch (opts[r].kind) {
  case 'x':
    if (x.kind == 't')
      s_.complete(x);
    else
      s_.reopen(x);
    break;
  case 'e':
    if (x.kind == 't')
      taskForm(x);
    else if (x.kind == 'p')
      projectForm(x);
    else if (x.kind == 'a')
      areaForm(x);
    break;
  case 'm': {
    int pid = pickProject("MOVE TO PROJECT");
    if (pid >= 0)
      s_.moveTask(x.id, pid ? s_.projectAreaId(pid) : 0, pid);
    break;
  }
  case 'c':
    checklistEditor(x);
    break;
  case 'L':
    lifecycle(x);
    break;
  case 'd':
    if (confirmDialog("Delete \"" + x.title + "\" permanently?"))
      s_.erase(x);
    break;
  case '\n':
    openContainer('p', x.id, x.title, x.areaName);
    break;
  case 'H':
    headingLifecycle(x);
    break;
  }
}

void App::handleMouse() {
  MEVENT ev;
  if (getmouse(&ev) != OK)
    return;
  bool leftClick =
      ev.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED);
  bool rightClick = ev.bstate & (BUTTON3_CLICKED | BUTTON3_PRESSED);
#ifdef BUTTON_CTRL
  bool ctrlClick = leftClick && (ev.bstate & BUTTON_CTRL);
#else
  bool ctrlClick = false;
#endif
  if (!leftClick && !rightClick)
    return;

  if (ctrlClick && ev.y >= descRowRange_.first && ev.y < descRowRange_.second) {
    openUrl(firstUrl(scopeDescription_));
    return;
  }

  for (auto &[y, target] : sidebarRows_) {
    if (ev.y != y)
      continue;
    if (target.kind == 'v') {
      view_ = target.idOrView;
      scopeKind_ = 0;
      hidden_.clear();
      pick_ = 0;
      scroll_ = 0;
    } else if (target.kind == 'a') {
      std::string areaName;
      for (auto &a : s_.areas())
        if (a.id == target.idOrView)
          areaName = a.name;
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
      if (rightClick)
        showActionsMenuFor(p);
      else
        openContainer('p', p.id, p.title, p.areaName);
    }
    return;
  }

  for (auto &[y, idx] : mainRows_) {
    if (ev.y != y)
      continue;
    if (ctrlClick && idx >= 0 && idx < (int)list_.size()) {
      std::string url = firstUrl(list_[idx].title);
      if (url.empty())
        url = firstUrl(list_[idx].notes);
      openUrl(url);
      return;
    }
    if (rightClick) {
      showActionsMenu(idx);
    } else if (ev.bstate & BUTTON1_DOUBLE_CLICKED) {
      pick_ = idx;
      handle({Key::Enter});
    } else if (pick_ == idx) {
      handle({Key::Enter}); // clicking the already-selected row opens/edits it
    } else {
      pick_ = idx;
    }
    return;
  }
}

void App::handle(KeyEvent k) {
  auto isChar = [&](char c) { return k.type == Key::Char && k.ch == c; };
  bool down = k.type == Key::Down || isChar('j');
  bool up = k.type == Key::Up || isChar('k');
  bool left = k.type == Key::Left || isChar('h');
  bool right = k.type == Key::Right || isChar('l');
  if (visual_) {
    if (k.type == Key::Escape || isChar('v')) {
      visual_ = false;
      return;
    }
    if (down) {
      pick_ = std::min(pick_ + 1, std::max(0, (int)list_.size() - 1));
      return;
    }
    if (up) {
      pick_ = std::max(0, pick_ - 1);
      return;
    }
    if (isChar('x')) {
      bulkComplete();
      return;
    }
    if (isChar('m')) {
      bulkMove();
      return;
    }
    if (isChar('T')) {
      bulkTag();
      return;
    }
    if (isChar('s')) {
      bulkSetDate(false);
      return;
    }
    if (isChar('S')) {
      bulkSetDate(true);
      return;
    }
    return;
  }
  if (isChar('q')) {
    on_ = false;
    return;
  }
  if (down) {
    pick_ = std::min(pick_ + 1, std::max(0, (int)list_.size() - 1));
    return;
  }
  if (up) {
    pick_ = std::max(0, pick_ - 1);
    return;
  }
  // Escape is deliberately NOT wired to step back out of a project/area --
  // it's also "cancel" inside dialogs, and having it double as "go back"
  // here too made it too easy to pop out further than intended.
  if (left && scopeKind_ != 0) {
    scopeKind_ = 0;
    pick_ = 0;
    scroll_ = 0;
    return;
  }
  if (left) {
    view_ = (view_ + (int)views_.size() - 1) % views_.size();
    hidden_.clear();
    pick_ = 0;
    scroll_ = 0;
    return;
  }
  if (right) {
    if (scopeKind_ != 0)
      return;
    view_ = (view_ + 1) % views_.size();
    hidden_.clear();
    pick_ = 0;
    scroll_ = 0;
    return;
  }
  if (isChar('J') && !list_.empty() && pick_ + 1 < (int)list_.size()) {
    moveItem(1);
    return;
  }
  if (isChar('K') && !list_.empty() && pick_ > 0) {
    moveItem(-1);
    return;
  }
  if (isChar('b')) {
    sidebar_ = !sidebar_;
    return;
  }
  if (isChar('v') && !list_.empty() && list_[pick_].kind == 't') {
    visual_ = true;
    visualAnchor_ = pick_;
    return;
  }
  if (isChar('n')) {
    std::vector<PickerItem> opts = {
        {"\xe2\x97\x8b", "New Task", "", 't', Item{}},
        {"\xe2\x97\x87", "New Project", "", 'p', Item{}},
        {"\xe2\x97\x88", "New Area", "", 'a', Item{}},
    };
    if (scopeKind_ == 'p')
      opts.push_back({"\xe2\x80\x94", "New Heading", "", 'h', Item{}});
    int r = runPicker("NEW\xe2\x80\xa6", opts);
    if (r < 0)
      return;
    switch (opts[r].kind) {
    case 't': {
      int presetHeading = 0, afterSort = -1;
      if (scopeKind_ == 'p' && !list_.empty()) {
        Item &sel = list_[pick_];
        if (sel.kind == 'h')
          presetHeading = sel.id;
        else if (sel.kind == 't') {
          presetHeading = sel.headingId;
          afterSort = sel.sortOrder;
        }
      }
      taskForm({}, presetHeading, afterSort);
      break;
    }
    case 'p':
      projectForm();
      break;
    case 'a':
      areaForm();
      break;
    case 'h':
      headingForm();
      break;
    }
    return;
  }
  if (isChar('c') && !list_.empty() && list_[pick_].kind == 't') {
    checklistEditor(list_[pick_]);
    return;
  }
  if (isChar('f')) {
    find();
    return;
  }
  if (isChar('m') && !list_.empty() && list_[pick_].kind == 't') {
    int pid = pickProject("MOVE TO PROJECT");
    if (pid >= 0)
      s_.moveTask(list_[pick_].id, pid ? s_.projectAreaId(pid) : 0, pid);
    return;
  }
  if (isChar('T')) {
    tagFilterForm();
    return;
  }
  if (isChar('A')) {
    group_ = !group_;
    return;
  }
  if (isChar('x') && !list_.empty()) {
    if (list_[pick_].kind == 't')
      s_.complete(list_[pick_]);
    else if (list_[pick_].kind != 'h')
      lifecycle(list_[pick_]);
    return;
  }
  if (isChar('X') && scopeKind_ != 0) {
    Item cur;
    cur.id = scope_;
    cur.kind = scopeKind_;
    cur.title = scopeName_;
    lifecycle(cur);
    return;
  }
  if (isChar('u') && !list_.empty() &&
      (active() == "Logbook" || hidden_ == "Logged Projects" ||
       hidden_ == "Archived Areas")) {
    s_.reopen(list_[pick_]);
    return;
  }
  // d now deletes any task anywhere (always with a confirmation prompt);
  // projects/areas keep the narrower archive-only behavior since they also
  // have the fuller complete/cancel/delete flow via x/L.
  if (isChar('d') && !list_.empty()) {
    auto &item = list_[pick_];
    bool inArchiveView = active() == "Logbook" ||
                         hidden_ == "Logged Projects" ||
                         hidden_ == "Archived Areas";
    if (item.kind == 't') {
      if (confirmDialog("Delete \"" + item.title + "\" permanently?"))
        s_.erase(item);
    } else if (inArchiveView) {
      if (confirmDialog("Delete \"" + item.title + "\" permanently?"))
        s_.erase(item);
    }
    return;
  }
  if (isChar('e') && !list_.empty()) {
    if (list_[pick_].kind == 't')
      taskForm(list_[pick_]);
    else if (list_[pick_].kind == 'p')
      projectForm(list_[pick_]);
    else if (list_[pick_].kind == 'a')
      areaForm(list_[pick_]);
    return;
  }
  if (k.type == Key::Enter && !list_.empty()) {
    auto &sel = list_[pick_];
    if (sel.kind == 'p')
      openContainer('p', sel.id, sel.title, sel.areaName);
    else if (sel.kind == 't')
      taskForm(sel);
    else if (sel.kind == 'h' && scopeKind_ == 'p')
      headingLifecycle(sel);
    return;
  }
  if (isChar('?')) {
    help();
    return;
  }
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
  mouseinterval(0); // report clicks immediately rather than trying to pair them
                    // into one down+up event
  // Opt in to the Kitty keyboard protocol / xterm modifyOtherKeys reporting,
  // which is what lets Shift+Enter be told apart from plain Enter. Terminals
  // that don't understand this simply ignore it.
  fputs("\x1b[>1u", stdout);
  fflush(stdout);
  std::signal(SIGCHLD,
              SIG_IGN); // openUrl() forks a detached opener; never wait on it
  while (on_) {
    load();
    draw();
    KeyEvent k = readKey(stdscr);
    if (k.type == Key::Mouse)
      handleMouse();
    else
      handle(k);
  }
  fputs("\x1b[<1u", stdout);
  fflush(stdout);
  endwin();
}
