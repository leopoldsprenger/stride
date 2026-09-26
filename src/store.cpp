#include "store.h"

#include <algorithm>
#include <stdexcept>

Store::Store(const std::string& path) {
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("cannot open database");
  exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;");
  migrate();
}
Store::~Store() { sqlite3_close(db_); }

void Store::exec(const std::string& q) {
  char* err = nullptr;
  if (sqlite3_exec(db_, q.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string e = err ? err : "sqlite error";
    sqlite3_free(err);
    throw std::runtime_error(e);
  }
}

sqlite3_stmt* Store::prep(const std::string& q) {
  sqlite3_stmt* s{};
  sqlite3_prepare_v2(db_, q.c_str(), -1, &s, nullptr);
  return s;
}
std::string Store::tx(sqlite3_stmt* s, int n) {
  auto p = (const char*)sqlite3_column_text(s, n);
  return p ? p : "";
}
void Store::bind(sqlite3_stmt* s, int n, const std::string& v) { sqlite3_bind_text(s, n, v.c_str(), -1, SQLITE_TRANSIENT); }
void Store::num(sqlite3_stmt* s, int n, int v) {
  if (v) sqlite3_bind_int(s, n, v);
  else sqlite3_bind_null(s, n);
}
void Store::nullable(sqlite3_stmt* s, int n, const std::string& v) {
  if (v.empty()) sqlite3_bind_null(s, n);
  else bind(s, n, v);
}
void Store::step(sqlite3_stmt* s) {
  sqlite3_step(s);
  sqlite3_finalize(s);
}
void Store::stmt(const std::string& q, const std::vector<std::string>& v) {
  auto* s = prep(q);
  for (int i = 0; i < (int)v.size(); ++i) bind(s, i + 1, v[i]);
  step(s);
}

std::vector<Ref> Store::refs(const std::string& q) {
  std::vector<Ref> o;
  auto* s = prep(q);
  while (sqlite3_step(s) == SQLITE_ROW) o.push_back({sqlite3_column_int(s, 0), tx(s, 1), tx(s, 2)});
  sqlite3_finalize(s);
  return o;
}

// 17-column contract: id, kind, title, notes, checklist, areaId, areaName,
// projectId, projectName, headingId, doDate, deadline, tags, someday,
// status, completedAt, sortOrder.
std::vector<Item> Store::read(const std::string& q) {
  std::vector<Item> out;
  auto* s = prep(q);
  while (sqlite3_step(s) == SQLITE_ROW) {
    Item it;
    it.id = sqlite3_column_int(s, 0);
    std::string k = tx(s, 1);
    it.kind = k.empty() ? 't' : k[0];
    it.title = tx(s, 2);
    it.notes = tx(s, 3);
    it.checklist = tx(s, 4);
    it.areaId = sqlite3_column_int(s, 5);
    it.areaName = tx(s, 6);
    it.projectId = sqlite3_column_int(s, 7);
    it.projectName = tx(s, 8);
    it.headingId = sqlite3_column_int(s, 9);
    it.doDate = tx(s, 10);
    it.deadline = tx(s, 11);
    it.tags = tx(s, 12);
    it.someday = sqlite3_column_int(s, 13) != 0;
    it.status = tx(s, 14);
    it.completedAt = tx(s, 15);
    it.sortOrder = sqlite3_column_int(s, 16);
    out.push_back(it);
  }
  sqlite3_finalize(s);
  return out;
}

std::vector<Item> Store::taskQuery(const std::string& where, const std::string& order, const std::string& tagFilter) {
  std::string w = where;
  if (tagFilter == "none") {
    w += " AND (t.tags IS NULL OR t.tags='')";
  } else if (!tagFilter.empty()) {
    for (auto& raw : splitComma(tagFilter)) {
      auto tag = sanitizeTag(raw);
      if (tag.empty()) continue;
      w += " AND instr(','||replace(lower(t.tags),' ','')||',',',"+tag+",')>0";
    }
  }
  std::string q =
      "SELECT t.id,'t',t.title,t.notes,t.checklist,COALESCE(t.area_id,0),COALESCE(a.name,''),"
      "COALESCE(t.project_id,0),COALESCE(pr.name,''),COALESCE(t.heading_id,0),COALESCE(t.do_date,''),"
      "COALESCE(t.deadline,''),t.tags,t.someday,t.status,COALESCE(t.completed_at,''),t.sort_order FROM tasks t "
      "LEFT JOIN areas a ON a.id=t.area_id LEFT JOIN projects pr ON pr.id=t.project_id WHERE " +
      w + " ORDER BY " + order;
  return read(q);
}

static std::string projectSelectBase() {
  return "SELECT p.id,'p',p.name,p.description,'',COALESCE(p.area_id,0),COALESCE(a.name,''),0,'',0,"
         "COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',0,p.status,COALESCE(p.completed_at,''),p.sort_order "
         "FROM projects p LEFT JOIN areas a ON a.id=p.area_id";
}

void Store::foldProjectTasks(const std::vector<Item>& projects, std::vector<Item>& tasks,
                              const std::function<bool(const Item&)>& matches, bool sameDateOnly) {
  for (auto& p : projects) {
    auto open = read("SELECT id,'t','','','',0,'',0,'',0,COALESCE(do_date,''),COALESCE(deadline,''),'',0,status,'',0 "
                      "FROM tasks WHERE project_id=" +
                      std::to_string(p.id) + " AND status='open'");
    if (open.empty()) continue;
    bool allMatch = std::all_of(open.begin(), open.end(), [&](const Item& t) {
      if (sameDateOnly) return !t.doDate.empty() && t.doDate == p.doDate;
      return matches(t);
    });
    if (!allMatch) continue;
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [&](const Item& t) { return t.projectId == p.id; }),
                tasks.end());
  }
}

// -- reference lists ----------------------------------------------------------

std::vector<Ref> Store::areas(bool archived) {
  return refs(std::string("SELECT id,name,'' FROM areas WHERE status ") + (archived ? "!='open'" : "='open'") +
              " ORDER BY sort_order,name");
}
std::vector<Ref> Store::projects(bool archived) {
  return refs(std::string("SELECT p.id,p.name,COALESCE(a.name,'') FROM projects p LEFT JOIN areas a ON a.id=p.area_id "
                           "WHERE p.status ") +
              (archived ? "!='open'" : "='open'") + " ORDER BY p.sort_order,p.name");
}
std::vector<Ref> Store::projectsInArea(int areaId) {
  return refs("SELECT id,name,'' FROM projects WHERE status='open' AND area_id=" + std::to_string(areaId) +
              " ORDER BY sort_order,name");
}
std::vector<std::string> Store::areaOrder() {
  std::vector<std::string> out;
  for (auto& a : areas()) out.push_back(a.name);
  return out;
}
std::vector<std::string> Store::allTags() {
  std::vector<std::string> out;
  auto* s = prep("SELECT tags FROM tasks WHERE tags IS NOT NULL AND tags<>''");
  while (sqlite3_step(s) == SQLITE_ROW) {
    for (auto& t : splitComma(tx(s, 0))) {
      std::string v = trimmed(t);
      if (!v.empty()) out.push_back(v);
    }
  }
  sqlite3_finalize(s);
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return lower(a) < lower(b); });
  out.erase(std::unique(out.begin(), out.end(), [](auto& a, auto& b) { return lower(a) == lower(b); }), out.end());
  return out;
}
bool Store::projectIsOpen(int id) {
  auto* s = prep("SELECT status FROM projects WHERE id=?");
  sqlite3_bind_int(s, 1, id);
  bool open = true;
  if (sqlite3_step(s) == SQLITE_ROW) open = tx(s, 0) == "open";
  sqlite3_finalize(s);
  return open;
}
int Store::projectAreaId(int projectId) {
  auto* s = prep("SELECT COALESCE(area_id,0) FROM projects WHERE id=?");
  sqlite3_bind_int(s, 1, projectId);
  int a = 0;
  if (sqlite3_step(s) == SQLITE_ROW) a = sqlite3_column_int(s, 0);
  sqlite3_finalize(s);
  return a;
}

// -- fuzzy finder index --------------------------------------------------------

std::vector<Item> Store::searchIndex() {
  std::vector<Item> out;
  auto as = read("SELECT id,'a',name,'','',0,'',0,'',0,'','','',0,'open','',sort_order FROM areas WHERE status='open' ORDER BY name");
  auto ps = read(projectSelectBase() + " WHERE p.status='open' ORDER BY p.name");
  auto ts = taskQuery("t.status='open'", "t.title", "");
  out.insert(out.end(), as.begin(), as.end());
  out.insert(out.end(), ps.begin(), ps.end());
  out.insert(out.end(), ts.begin(), ts.end());
  return out;
}

// -- smart views ----------------------------------------------------------------

std::vector<Item> Store::viewInbox(const std::string& tf) {
  return taskQuery("t.status='open' AND t.area_id IS NULL AND t.project_id IS NULL AND t.do_date IS NULL AND t.someday=0",
                    "t.sort_order,t.id", tf);
}
std::vector<Item> Store::viewAnytime(const std::string& tf) {
  // Everything open except Someday -- today's and future work included.
  return taskQuery("t.status='open' AND t.someday=0", "t.sort_order,t.id", tf);
}
std::vector<Item> Store::viewSomeday(const std::string& tf) {
  return taskQuery("t.status='open' AND t.someday=1", "t.sort_order,t.id", tf);
}
std::vector<Item> Store::viewLogbook() { return taskQuery("t.status='done'", "t.completed_at DESC", ""); }
std::vector<Item> Store::viewLoggedProjects() {
  return read(projectSelectBase() + " WHERE p.status!='open' ORDER BY p.completed_at DESC");
}
std::vector<Item> Store::viewArchivedAreas() {
  return read(
      "SELECT id,'a',name,'','',0,'',0,'',0,'','','',0,status,COALESCE(completed_at,''),sort_order "
      "FROM areas WHERE status!='open' ORDER BY completed_at DESC");
}

std::vector<Item> Store::viewDay(bool tomorrow, const std::string& tf) {
  std::string target = tomorrow ? "date('now','localtime','+1 day')" : "date('now','localtime')";
  std::string cmp = tomorrow ? "=" : "<=";
  std::string taskWhere = "t.status='open' AND t.someday=0 AND (t.do_date " + cmp + " " + target + " OR t.deadline " +
                           cmp + " " + target + ")";
  std::string projWhere = "p.status='open' AND (p.do_date " + cmp + " " + target + " OR p.deadline " + cmp + " " + target + ")";
  auto tasks = taskQuery(taskWhere, "t.sort_order,t.id", tf);
  auto projects = read(projectSelectBase() + " WHERE " + projWhere + " ORDER BY p.sort_order,p.id");
  std::string tomorrowDate = todayPlus(1);
  foldProjectTasks(projects, tasks, [tomorrow, tomorrowDate](const Item& t) {
    std::string tdy = today();
    return (!t.doDate.empty() && (tomorrow ? t.doDate == tomorrowDate : t.doDate <= tdy)) ||
           (!t.deadline.empty() && (tomorrow ? t.deadline == tomorrowDate : t.deadline <= tdy));
  });
  std::vector<Item> out = projects;
  out.insert(out.end(), tasks.begin(), tasks.end());
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.sortOrder < b.sortOrder; });
  return out;
}

std::vector<Item> Store::viewUpcoming(const std::string& tf) {
  auto tasks = taskQuery("t.status='open' AND t.someday=0 AND t.do_date>date('now','localtime')",
                          "t.do_date,t.sort_order,t.id", tf);
  auto projects = read(projectSelectBase() +
                        " WHERE p.status='open' AND p.do_date>date('now','localtime') ORDER BY p.do_date,p.sort_order,p.id");
  foldProjectTasks(projects, tasks, [](const Item& t) { return !t.doDate.empty(); }, /*sameDateOnly=*/true);
  std::vector<Item> out = projects;
  out.insert(out.end(), tasks.begin(), tasks.end());
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
    std::string da = a.doDate.empty() ? a.deadline : a.doDate;
    std::string db = b.doDate.empty() ? b.deadline : b.doDate;
    if (da != db) return da < db;
    return a.sortOrder < b.sortOrder;
  });
  return out;
}

std::vector<Item> Store::viewDeadlines(const std::string& tf) {
  auto tasks = taskQuery("t.status='open' AND t.deadline IS NOT NULL", "t.deadline,t.sort_order,t.id", tf);
  auto projects =
      read(projectSelectBase() + " WHERE p.status='open' AND p.deadline IS NOT NULL ORDER BY p.deadline,p.sort_order,p.id");
  std::vector<Item> out = projects;
  out.insert(out.end(), tasks.begin(), tasks.end());
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
    if (a.deadline != b.deadline) return a.deadline < b.deadline;
    return a.sortOrder < b.sortOrder;
  });
  return out;
}

std::vector<Item> Store::viewArea(int areaId, const std::string& tf) {
  std::vector<Item> out;
  auto projects = read(
      "SELECT id,'p',name,description,'',0,'',0,'',0,COALESCE(do_date,''),COALESCE(deadline,''),'',0,status,'',sort_order "
      "FROM projects WHERE status='open' AND area_id=" +
      std::to_string(areaId) + " ORDER BY (do_date IS NULL),do_date,sort_order");
  auto undated = taskQuery(
      "t.status='open' AND t.someday=0 AND t.do_date IS NULL AND t.area_id=" + std::to_string(areaId) + " AND t.project_id IS NULL",
      "t.sort_order,t.id", tf);
  auto dated = taskQuery(
      "t.status='open' AND t.someday=0 AND t.do_date IS NOT NULL AND t.area_id=" + std::to_string(areaId) + " AND t.project_id IS NULL",
      "t.sort_order,t.id", tf);
  auto someday = taskQuery(
      "t.status='open' AND t.someday=1 AND t.area_id=" + std::to_string(areaId) + " AND t.project_id IS NULL",
      "t.sort_order,t.id", tf);
  for (auto& p : projects) p.section = "Projects";
  for (auto& t : undated) t.section = "Tasks";
  for (auto& t : dated) t.section = "Scheduled";
  for (auto& t : someday) t.section = "Someday";
  out.insert(out.end(), projects.begin(), projects.end());
  out.insert(out.end(), undated.begin(), undated.end());
  out.insert(out.end(), dated.begin(), dated.end());
  out.insert(out.end(), someday.begin(), someday.end());
  return out;
}

std::vector<Item> Store::viewProject(int projectId, const std::string& tf, bool projectIsOpen) {
  std::string statusClause = projectIsOpen ? "t.status='open' AND " : "";
  auto tasks = taskQuery(statusClause + "t.project_id=" + std::to_string(projectId), "t.id", tf);
  auto headings =
      read("SELECT id,'h',title,'','',0,'',project_id,'',id,'','','',0,'open','',sort_order FROM headings WHERE project_id=" +
           std::to_string(projectId) + " ORDER BY sort_order,id");
  std::vector<std::pair<int, int>> bucketOrder;  // (heading_id or -1, heading sort_order)
  bucketOrder.push_back({-1, -1});
  for (auto& h : headings) bucketOrder.push_back({h.id, h.sortOrder});
  std::sort(tasks.begin(), tasks.end(), [&](const Item& a, const Item& b) {
    int oa = 0, ob = 0;
    for (auto& [hid, ord] : bucketOrder) {
      if (hid == a.headingId) oa = ord;
      if (hid == b.headingId) ob = ord;
    }
    if (oa != ob) return oa < ob;
    std::string da = a.doDate.empty() ? "9999-99-99" : a.doDate;
    std::string db = b.doDate.empty() ? "9999-99-99" : b.doDate;
    if (da != db) return da < db;
    return a.sortOrder < b.sortOrder;
  });
  std::vector<Item> out;
  int idx = 0;
  while (idx < (int)tasks.size() && tasks[idx].headingId == 0) out.push_back(tasks[idx++]);
  for (auto& h : headings) {
    out.push_back(h);
    while (idx < (int)tasks.size() && tasks[idx].headingId == h.id) out.push_back(tasks[idx++]);
  }
  while (idx < (int)tasks.size()) out.push_back(tasks[idx++]);
  return out;
}

Item Store::getProject(int id) {
  auto r = read(projectSelectBase() + " WHERE p.id=" + std::to_string(id));
  return r.empty() ? Item{} : r.front();
}

// -- full raw dumps, for the mirror exporter (every row, any status) ----------

std::vector<Item> Store::allAreas() {
  return read("SELECT id,'a',name,'','',0,'',0,'',0,'','','',0,status,COALESCE(completed_at,''),sort_order "
              "FROM areas ORDER BY sort_order,name");
}
std::vector<Item> Store::allProjects() { return read(projectSelectBase() + " ORDER BY p.sort_order,p.id"); }
std::vector<Item> Store::allTasks() {
  return read(
      "SELECT t.id,'t',t.title,t.notes,t.checklist,COALESCE(t.area_id,0),COALESCE(a.name,''),"
      "COALESCE(t.project_id,0),COALESCE(pr.name,''),COALESCE(t.heading_id,0),COALESCE(t.do_date,''),"
      "COALESCE(t.deadline,''),t.tags,t.someday,t.status,COALESCE(t.completed_at,''),t.sort_order FROM tasks t "
      "LEFT JOIN areas a ON a.id=t.area_id LEFT JOIN projects pr ON pr.id=t.project_id "
      "ORDER BY COALESCE(t.project_id,0),COALESCE(t.heading_id,0),t.sort_order,t.id");
}
std::vector<Item> Store::allHeadings() {
  return read(
      "SELECT id,'h',title,'','',0,'',project_id,'',id,'','','',0,'open','',sort_order "
      "FROM headings ORDER BY project_id,sort_order,id");
}

// -- Things-import bookkeeping -------------------------------------------------

int Store::findByThingsUuid(char kind, const std::string& uuid) {
  auto* s = prep("SELECT id FROM " + std::string(tableFor(kind)) + " WHERE things_uuid=?");
  bind(s, 1, uuid);
  int id = 0;
  if (sqlite3_step(s) == SQLITE_ROW) id = sqlite3_column_int(s, 0);
  sqlite3_finalize(s);
  return id;
}
void Store::setThingsUuid(char kind, int id, const std::string& uuid) {
  auto* s = prep("UPDATE " + std::string(tableFor(kind)) + " SET things_uuid=? WHERE id=?");
  bind(s, 1, uuid);
  sqlite3_bind_int(s, 2, id);
  step(s);
}

// -- cross-device identity ------------------------------------------------

int Store::findByStrideUuid(char kind, const std::string& uuid) {
  auto* s = prep("SELECT id FROM " + std::string(tableFor(kind)) + " WHERE stride_uuid=?");
  bind(s, 1, uuid);
  int id = 0;
  if (sqlite3_step(s) == SQLITE_ROW) id = sqlite3_column_int(s, 0);
  sqlite3_finalize(s);
  return id;
}
void Store::setStrideUuid(char kind, int id, const std::string& uuid) {
  auto* s = prep("UPDATE " + std::string(tableFor(kind)) + " SET stride_uuid=? WHERE id=?");
  bind(s, 1, uuid);
  sqlite3_bind_int(s, 2, id);
  step(s);
}
std::map<int, std::string> Store::strideUuids(char kind) {
  std::map<int, std::string> out;
  auto* s = prep("SELECT id,stride_uuid FROM " + std::string(tableFor(kind)) + " WHERE stride_uuid IS NOT NULL");
  while (sqlite3_step(s) == SQLITE_ROW) out[sqlite3_column_int(s, 0)] = (const char*)sqlite3_column_text(s, 1);
  sqlite3_finalize(s);
  return out;
}

// -- mutation ---------------------------------------------------------------

int Store::saveTask(Item t, int areaId, int projectId) {
  t.someday = t.doDate == "someday";
  if (t.someday) t.doDate.clear();
  sqlite3_stmt* s = prep(t.id ? "UPDATE tasks SET title=?,notes=?,tags=?,checklist=?,area_id=?,project_id=?,do_date=?,"
                               "deadline=?,someday=? WHERE id=?"
                             : "INSERT INTO tasks(title,notes,tags,checklist,area_id,project_id,do_date,deadline,someday,"
                               "sort_order) VALUES(?,?,?,?,?,?,?,?,?,COALESCE((SELECT MAX(sort_order)+1 FROM tasks),0))");
  bind(s, 1, t.title);
  bind(s, 2, t.notes);
  bind(s, 3, t.tags);
  bind(s, 4, t.checklist);
  num(s, 5, areaId);
  num(s, 6, projectId);
  nullable(s, 7, t.doDate);
  nullable(s, 8, t.deadline);
  sqlite3_bind_int(s, 9, t.someday ? 1 : 0);
  if (t.id) sqlite3_bind_int(s, 10, t.id);
  step(s);
  int id = t.id ? t.id : (int)sqlite3_last_insert_rowid(db_);
  if (!t.id) setStrideUuid('t', id, genUuid());
  return id;
}
void Store::insertTaskAfter(int taskId, int afterSortOrder) {
  exec("UPDATE tasks SET sort_order=sort_order+1 WHERE sort_order>" + std::to_string(afterSortOrder) + " AND id<>" +
       std::to_string(taskId));
  exec("UPDATE tasks SET sort_order=" + std::to_string(afterSortOrder + 1) + " WHERE id=" + std::to_string(taskId));
}
int Store::saveProject(Item p, int areaId) {
  sqlite3_stmt* s = prep(p.id ? "UPDATE projects SET name=?,description=?,area_id=?,do_date=?,deadline=? WHERE id=?"
                               : "INSERT INTO projects(name,description,area_id,do_date,deadline,sort_order) "
                                 "VALUES(?,?,?,?,?,COALESCE((SELECT MAX(sort_order)+1 FROM projects),0))");
  bind(s, 1, p.title);
  bind(s, 2, p.notes);
  num(s, 3, areaId);
  nullable(s, 4, p.doDate);
  nullable(s, 5, p.deadline);
  if (p.id) sqlite3_bind_int(s, 6, p.id);
  step(s);
  int id = p.id ? p.id : (int)sqlite3_last_insert_rowid(db_);
  if (!p.id) setStrideUuid('p', id, genUuid());
  return id;
}
void Store::renameArea(int id, const std::string& name) {
  sqlite3_stmt* s = prep("UPDATE areas SET name=? WHERE id=?");
  bind(s, 1, name);
  sqlite3_bind_int(s, 2, id);
  step(s);
}
int Store::addArea(const std::string& name) {
  stmt("INSERT INTO areas(name,sort_order) VALUES(?,COALESCE((SELECT MAX(sort_order)+1 FROM areas),0))", {name});
  int id = (int)sqlite3_last_insert_rowid(db_);
  setStrideUuid('a', id, genUuid());
  return id;
}
int Store::addHeading(int projectId, const std::string& title) {
  sqlite3_stmt* s = prep(
      "INSERT INTO headings(project_id,title,sort_order) VALUES(?,?,COALESCE((SELECT MAX(sort_order)+1 FROM "
      "headings WHERE project_id=?),0))");
  sqlite3_bind_int(s, 1, projectId);
  bind(s, 2, title);
  sqlite3_bind_int(s, 3, projectId);
  step(s);
  int id = (int)sqlite3_last_insert_rowid(db_);
  setStrideUuid('h', id, genUuid());
  return id;
}
void Store::renameHeading(int id, const std::string& title) {
  sqlite3_stmt* s = prep("UPDATE headings SET title=? WHERE id=?");
  bind(s, 1, title);
  sqlite3_bind_int(s, 2, id);
  step(s);
}
void Store::deleteHeading(int id) {
  sqlite3_stmt* s = prep("UPDATE tasks SET heading_id=NULL WHERE heading_id=?");
  sqlite3_bind_int(s, 1, id);
  step(s);
  exec("DELETE FROM headings WHERE id=" + std::to_string(id));
}
void Store::setTaskHeading(int taskId, int headingId) {
  sqlite3_stmt* s = prep("UPDATE tasks SET heading_id=? WHERE id=?");
  num(s, 1, headingId);
  sqlite3_bind_int(s, 2, taskId);
  step(s);
}
void Store::moveTask(int taskId, int areaId, int projectId) {
  sqlite3_stmt* s = prep("UPDATE tasks SET area_id=?,project_id=?,heading_id=NULL WHERE id=?");
  num(s, 1, areaId);
  num(s, 2, projectId);
  sqlite3_bind_int(s, 3, taskId);
  step(s);
}
void Store::complete(const Item& i, const std::string& at) {
  std::string col = i.kind == 'p' ? "completed" : "done";
  auto* s = prep("UPDATE " + std::string(tableFor(i.kind)) + " SET status='" + col +
                 "',completed_at=" + (at.empty() ? "strftime('%Y-%m-%dT%H:%M:%f','now','localtime')" : "?") +
                 " WHERE id=?");
  int idIdx = 1;
  if (!at.empty()) {
    bind(s, 1, at);
    idIdx = 2;
  }
  sqlite3_bind_int(s, idIdx, i.id);
  step(s);
}
void Store::cancel(const Item& i, const std::string& at) {
  auto* s = prep("UPDATE " + std::string(tableFor(i.kind)) +
                 " SET status='cancelled',completed_at=" + (at.empty() ? "strftime('%Y-%m-%dT%H:%M:%f','now','localtime')" : "?") +
                 " WHERE id=?");
  int idIdx = 1;
  if (!at.empty()) {
    bind(s, 1, at);
    idIdx = 2;
  }
  sqlite3_bind_int(s, idIdx, i.id);
  step(s);
}
void Store::reopen(const Item& i) {
  exec("UPDATE " + std::string(tableFor(i.kind)) + " SET status='open',completed_at=NULL WHERE id=" + std::to_string(i.id));
}
void Store::erase(const Item& i) { exec("DELETE FROM " + std::string(tableFor(i.kind)) + " WHERE id=" + std::to_string(i.id)); }
void Store::swapOrder(const Item& a, const Item& b) {
  exec("UPDATE " + std::string(tableFor(a.kind)) + " SET sort_order=" + std::to_string(b.sortOrder) +
       " WHERE id=" + std::to_string(a.id));
  exec("UPDATE " + std::string(tableFor(b.kind)) + " SET sort_order=" + std::to_string(a.sortOrder) +
       " WHERE id=" + std::to_string(b.id));
}

void Store::migrate() {
  exec("CREATE TABLE IF NOT EXISTS areas(id INTEGER PRIMARY KEY,name TEXT UNIQUE NOT NULL,status TEXT NOT NULL "
       "DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT NULL DEFAULT 0);");
  exec("CREATE TABLE IF NOT EXISTS projects(id INTEGER PRIMARY KEY,name TEXT NOT NULL UNIQUE,description TEXT NOT "
       "NULL DEFAULT '',area_id INTEGER REFERENCES areas(id),do_date TEXT,deadline TEXT,status TEXT NOT NULL "
       "DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT NULL DEFAULT 0);");
  exec("CREATE TABLE IF NOT EXISTS tasks(id INTEGER PRIMARY KEY,title TEXT NOT NULL,notes TEXT NOT NULL DEFAULT "
       "'',tags TEXT NOT NULL DEFAULT '',project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,area_id "
       "INTEGER REFERENCES areas(id) ON DELETE SET NULL,heading_id INTEGER,do_date TEXT,deadline TEXT,someday "
       "INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT "
       "NULL DEFAULT 0,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
  exec("CREATE TABLE IF NOT EXISTS headings(id INTEGER PRIMARY KEY,project_id INTEGER NOT NULL REFERENCES "
       "projects(id) ON DELETE CASCADE,title TEXT NOT NULL,sort_order INTEGER NOT NULL DEFAULT 0);");
  for (auto q : {"ALTER TABLE tasks ADD COLUMN heading_id INTEGER", "ALTER TABLE tasks ADD COLUMN checklist TEXT NOT NULL DEFAULT ''",
                 "ALTER TABLE areas ADD COLUMN things_uuid TEXT", "ALTER TABLE projects ADD COLUMN things_uuid TEXT",
                 "ALTER TABLE tasks ADD COLUMN things_uuid TEXT", "ALTER TABLE headings ADD COLUMN things_uuid TEXT",
                 "ALTER TABLE areas ADD COLUMN stride_uuid TEXT", "ALTER TABLE projects ADD COLUMN stride_uuid TEXT",
                 "ALTER TABLE tasks ADD COLUMN stride_uuid TEXT", "ALTER TABLE headings ADD COLUMN stride_uuid TEXT"}) {
    try {
      exec(q);
    } catch (...) {
    }
  }
  // Partial unique indexes: only rows actually imported from Things carry a
  // uuid, so re-running the importer can look one up and update in place
  // instead of creating duplicates.
  for (auto t : {"areas", "projects", "tasks", "headings"}) {
    try {
      exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_" + std::string(t) + "_things_uuid ON " + t +
           "(things_uuid) WHERE things_uuid IS NOT NULL");
      exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_" + std::string(t) + "_stride_uuid ON " + t +
           "(stride_uuid) WHERE stride_uuid IS NOT NULL");
    } catch (...) {
    }
  }
  // stride_uuid is every row's cross-device identity for git-mirror
  // reconciliation (see sync.cpp/mirror_import.cpp) -- every row needs one,
  // including ones that pre-date this column, so backfill once here rather
  // than only assigning it on new inserts.
  for (auto t : {"areas", "projects", "tasks", "headings"}) {
    auto* sel = prep("SELECT id FROM " + std::string(t) + " WHERE stride_uuid IS NULL");
    std::vector<int> ids;
    while (sqlite3_step(sel) == SQLITE_ROW) ids.push_back(sqlite3_column_int(sel, 0));
    sqlite3_finalize(sel);
    for (int id : ids) {
      auto* upd = prep("UPDATE " + std::string(t) + " SET stride_uuid=? WHERE id=?");
      bind(upd, 1, genUuid());
      sqlite3_bind_int(upd, 2, id);
      step(upd);
    }
  }
}
