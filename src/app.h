#pragma once

#include <ncurses.h>

#include <optional>
#include <string>
#include <vector>

#include "store.h"

struct GroupKey {
  int rank;
  std::string primary;
  std::string secondary;
};

class App {
 public:
  explicit App(Store& s) : s_(s) {}
  void run();

 private:
  Store& s_;
  bool on_ = true, sidebar_ = true, group_ = false, visual_ = false;
  int pick_ = 0, view_ = 0, scope_ = 0, visualAnchor_ = 0;
  char scopeKind_ = 0;
  std::string scopeName_, scopeAreaName_, scopeDescription_, tags_, hidden_;
  std::vector<Item> list_;
  std::vector<std::string> areaOrder_;
  std::vector<std::string> views_{"Inbox", "Today", "Upcoming", "Anytime", "Someday", "Logbook"};

  std::string active() const;
  std::string name() const;
  void load();
  GroupKey groupKeyFor(const Item& x) const;
  std::string dateGroupLabel(const Item& x) const;
  void applyGrouping();

  // rendering
  void draw();
  void drawSidebar(int rows, int width);
  void drawMain(int rows, int cols, int off);
  void drawIconStrip(int y, int rightEdge, const Item& x);
  bool rowSelected(int index) const;

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

  void handle(int k);
};
