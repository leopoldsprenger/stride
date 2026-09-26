#pragma once
// All SQLite access. Every read goes through read(), which always maps the
// same column shape into an Item so one function can serve every query.

#include <sqlite3.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "util.h"

class Store {
 public:
  explicit Store(const std::string& path);
  ~Store();

  void exec(const std::string& q);

  // -- reference lists ------------------------------------------------------
  std::vector<Ref> areas(bool archived = false);
  std::vector<Ref> projects(bool archived = false);
  std::vector<Ref> projectsInArea(int areaId);
  std::vector<std::string> areaOrder();  // area names in sidebar order, for grouped sorting
  std::vector<std::string> allTags();
  bool projectIsOpen(int id);
  int projectAreaId(int projectId);

  // -- everything searchable by the fuzzy finder -----------------------------
  std::vector<Item> searchIndex();

  // -- smart views ------------------------------------------------------------
  std::vector<Item> viewInbox(const std::string& tf);
  std::vector<Item> viewAnytime(const std::string& tf);  // today + upcoming + undated, everything but someday
  std::vector<Item> viewSomeday(const std::string& tf);
  std::vector<Item> viewLogbook();
  std::vector<Item> viewLoggedProjects();
  std::vector<Item> viewArchivedAreas();
  std::vector<Item> viewDay(bool tomorrow, const std::string& tf);
  std::vector<Item> viewUpcoming(const std::string& tf);
  std::vector<Item> viewDeadlines(const std::string& tf);
  std::vector<Item> viewArea(int areaId, const std::string& tf);
  std::vector<Item> viewProject(int projectId, const std::string& tf, bool projectIsOpen);
  Item getProject(int id);

  // -- full raw dumps, for the mirror exporter (every row, any status) --------
  std::vector<Item> allAreas();
  std::vector<Item> allProjects();
  std::vector<Item> allTasks();
  std::vector<Item> allHeadings();  // headingId doubles as id here; projectId set

  // -- Things-import bookkeeping (idempotent re-imports by source uuid) -------
  int findByThingsUuid(char kind, const std::string& uuid);
  void setThingsUuid(char kind, int id, const std::string& uuid);

  // -- cross-device identity, for git-mirror reconciliation --------------------
  int findByStrideUuid(char kind, const std::string& uuid);
  void setStrideUuid(char kind, int id, const std::string& uuid);
  std::map<int, std::string> strideUuids(char kind);  // id -> uuid, whole table in one query

  // -- mutation ---------------------------------------------------------------
  int saveTask(Item t, int areaId, int projectId);  // returns the task's id
  void insertTaskAfter(int taskId, int afterSortOrder);
  int saveProject(Item p, int areaId);  // returns the project's id
  void renameArea(int id, const std::string& name);
  int addArea(const std::string& name);  // returns the area's id
  int addHeading(int projectId, const std::string& title);
  void renameHeading(int id, const std::string& title);
  void deleteHeading(int id);
  void setTaskHeading(int taskId, int headingId);
  void moveTask(int taskId, int areaId, int projectId);
  void complete(const Item& i, const std::string& at = "");  // at: explicit completed_at (reconcile only); empty = now
  void cancel(const Item& i, const std::string& at = "");
  void reopen(const Item& i);
  void erase(const Item& i);
  void swapOrder(const Item& a, const Item& b);
  void migrate();

 private:
  sqlite3* db_{};
  sqlite3_stmt* prep(const std::string& q);
  static std::string tx(sqlite3_stmt* s, int n);
  static void bind(sqlite3_stmt* s, int n, const std::string& v);
  static void num(sqlite3_stmt* s, int n, int v);
  static void nullable(sqlite3_stmt* s, int n, const std::string& v);
  void step(sqlite3_stmt* s);
  void stmt(const std::string& q, const std::vector<std::string>& v);
  std::vector<Ref> refs(const std::string& q);
  std::vector<Item> read(const std::string& q);
  std::vector<Item> taskQuery(const std::string& where, const std::string& order, const std::string& tagFilter);
  // Removes tasks from `tasks` that belong to a project in `projects` when
  // *every* open task of that project matches `matches` (or, if
  // `sameDateOnly`, shares the project's own do date exactly).
  void foldProjectTasks(const std::vector<Item>& projects, std::vector<Item>& tasks,
                         const std::function<bool(const Item&)>& matches, bool sameDateOnly = false);
};
