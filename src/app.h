#pragma once

#include <ncurses.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "input.h"
#include "store.h"

struct GroupKey {
  int rank;
  std::string primary;
  std::string secondary;
};

// Where a sidebar row leads: a FOCUS view (kind 'v', idOrView = index into
// views_), an area (kind 'a'), or a project (kind 'p').
struct SidebarTarget {
  char kind;
  int idOrView;
};

// One physical screen line within the main list, produced by
// buildMainLines() -- either a (possibly wrapped) group/date heading line,
// a blank separator above one, or one (possibly wrapped) line of an item's
// icon+title. Kept as data so the scroll offset can be computed against the
// full virtual layout before anything is actually drawn.
struct MainLine {
  bool header = false;
  int itemIndex = -1;    // valid when !header
  bool firstOfItem = false;
  int indent = 0;
  std::string text;
};

class App {
 public:
  explicit App(Store& s) : s_(s) {}
  void run();

 private:
  Store& s_;
  bool on_ = true, sidebar_ = true, group_ = false, visual_ = false;
  int pick_ = 0, view_ = 0, scope_ = 0, visualAnchor_ = 0;
  int scroll_ = 0;  // main-list scroll offset, in virtual lines (see buildMainLines)
  char scopeKind_ = 0;
  std::string scopeName_, scopeAreaName_, scopeDescription_, tags_, hidden_;
  std::vector<Item> list_;
  std::vector<std::string> areaOrder_;
  std::vector<std::string> views_{"Inbox", "Today", "Upcoming", "Anytime", "Someday", "Logbook"};

  // Screen-row -> target maps, rebuilt every draw() so mouse clicks (which
  // only know a row number) can be resolved back to "what's there". Mouse
  // handling never assumes these are stable between frames.
  std::vector<std::pair<int, SidebarTarget>> sidebarRows_;
  std::vector<std::pair<int, int>> mainRows_;  // screen row -> index into list_
  // Screen rows occupied by the project-description header (ctrl+click link
  // support only -- see handleMouse); {0,0} when there's no description shown.
  std::pair<int, int> descRowRange_{0, 0};

  std::string active() const;
  std::string name() const;
  void load();
  GroupKey groupKeyFor(const Item& x) const;
  std::string dateGroupLabel(const Item& x) const;
  void applyGrouping();

  // rendering
  void draw();
  void drawSidebar(int rows, int width);
  int wrapSidebarEntry(int y, int x, int width, const std::string& label, bool cur, SidebarTarget target);
  void drawMain(int rows, int cols, int off);
  void drawIconStrip(int y, int rightEdge, const Item& x);
  bool rowSelected(int index) const;
  std::vector<MainLine> buildMainLines(int cols, int off) const;
  void updateScroll(const std::vector<MainLine>& lines, int viewH);

  // lookups
  int findAreaId(const std::string& name);
  int findProjectId(const std::string& name);

  // forms
  void taskForm(std::optional<Item> e = {}, int presetHeadingId = 0, int insertAfterSortOrder = -1);
  void projectForm(std::optional<Item> e = {});
  void areaForm(std::optional<Item> e = {});
  void headingForm();
  void headingLifecycle(const Item& heading);
  void tagFilterForm();
  void lifecycle(const Item& i);
  void checklistEditor(Item t);
  void help();

  // navigation / finder
  void openContainer(char kind, int id, const std::string& title, const std::string& sub);
  std::string homeViewFor(const Item& t) const;
  void jumpToTask(const Item& t);
  void find();
  int pickProject(const std::string& title);

  // reordering
  void moveItem(int delta);

  // visual mode / bulk actions
  std::vector<int> selectedIndices() const;
  void bulkComplete();
  void bulkMove();
  void bulkTag();
  void bulkSetDate(bool deadline);

  // mouse / contextual actions menu -- see input.md notes in app.cpp for why
  void handleMouse();
  void showActionsMenu(int index);       // for a row in the main list_
  void showActionsMenuFor(const Item& x);  // the shared menu itself; also used for sidebar rows

  void handle(KeyEvent k);
};
