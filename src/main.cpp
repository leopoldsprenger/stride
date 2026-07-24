// Stride — a GTD-inspired TUI. See README.md for the feature/keymap overview.
//
// Layout of this file:
//   1. Item / Ref data structs
//   2. Store          — all SQLite access; every read goes through read(),
//   which
//                        always maps the same 16-column row shape into an Item.
//   3. Fuzzy matching  — small subsequence scorer used by the 'f' finder.
//   4. App             — ncurses rendering + input handling.
//   5. main()

#include <ncurses.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

struct Item {
  int id = 0;
  char kind = 't'; // 't' task, 'p' project, 'a' area, 'h' heading
  std::string title;
  std::string notes; // task notes, or project description
  int areaId = 0;
  std::string areaName;
  int projectId = 0;
  std::string projectName;
  int headingId = 0;
  std::string doDate;
  std::string deadline;
  std::string tags;
  bool someday = false;
  std::string status = "open"; // open/done/completed/cancelled
  std::string completedAt;
  int sortOrder = 0;
  std::string section; // UI-only grouping hint (e.g. "Projects" in an area
                       // view); not persisted
};

struct Ref {
  int id = 0;
  std::string name;
  std::string sub; // secondary context, e.g. a project's area name
};

static const char *tableFor(char kind) {
  switch (kind) {
  case 'p':
    return "projects";
  case 'a':
    return "areas";
  case 'h':
    return "headings";
  default:
    return "tasks";
  }
}

static std::string today() {
  std::time_t t = std::time(nullptr);
  char buf[11]{};
  std::strftime(buf, sizeof(buf), "%F", std::localtime(&t));
  return buf;
}

// Clip a string to fit `cols` terminal columns so we never write past a
// window edge (ncurses doesn't wrap or clip mvprintw for us). Counts UTF-8
// codepoints rather than bytes, since every symbol this app uses (icons,
// arrows, punctuation) occupies exactly one terminal column.
static std::string clip(const std::string &s, int cols) {
  if (cols <= 0)
    return "";
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size();) {
    starts.push_back(i);
    unsigned char c = s[i];
    size_t len = (c < 0x80)           ? 1
                 : ((c >> 5) == 0x6)  ? 2
                 : ((c >> 4) == 0xE)  ? 3
                 : ((c >> 3) == 0x1E) ? 4
                                      : 1;
    i += len;
  }
  if ((int)starts.size() <= cols)
    return s;
  if (cols == 1)
    return s.substr(starts[0],
                    (starts.size() > 1 ? starts[1] : s.size()) - starts[0]);
  return s.substr(0, starts[cols - 1]) + "\xe2\x80\xa6"; // UTF-8 ellipsis
}

static std::vector<std::string> splitComma(const std::string &s) {
  std::vector<std::string> out;
  size_t p = 0, n;
  while ((n = s.find(',', p)) != std::string::npos) {
    if (n > p)
      out.push_back(s.substr(p, n - p));
    p = n + 1;
  }
  if (p < s.size())
    out.push_back(s.substr(p));
  return out;
}

// Keep only characters we ever legitimately need in a tag, so tag filters
// can't be used to smuggle SQL into the query we build by hand below.
static std::string sanitizeTag(std::string s) {
  std::string out;
  for (char c : s)
    if (isalnum((unsigned char)c) || c == '-' || c == '_')
      out += (char)tolower((unsigned char)c);
  return out;
}

static std::string trimmed(const std::string &s) {
  size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(' ');
  return s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
  for (auto &c : s)
    c = (char)tolower((unsigned char)c);
  return s;
}

// ---------------------------------------------------------------------------
// Store — SQLite access
// ---------------------------------------------------------------------------

class Store {
public:
  explicit Store(const std::string &path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
      throw std::runtime_error("cannot open database");
    exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;");
    migrate();
  }
  ~Store() { sqlite3_close(db_); }

  void exec(const std::string &q) {
    char *err = nullptr;
    if (sqlite3_exec(db_, q.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string e = err ? err : "sqlite error";
      sqlite3_free(err);
      throw std::runtime_error(e);
    }
  }

  // -- reference lists --------------------------------------------------
  std::vector<Ref> areas(bool archived = false) {
    return refs(std::string("SELECT id,name,'' FROM areas WHERE status ") +
                (archived ? "!='open'" : "='open'") +
                " ORDER BY sort_order,name");
  }
  std::vector<Ref> projects(bool archived = false) {
    return refs(std::string("SELECT p.id,p.name,COALESCE(a.name,'') FROM "
                            "projects p LEFT JOIN areas a ON a.id=p.area_id "
                            "WHERE p.status ") +
                (archived ? "!='open'" : "='open'") +
                " ORDER BY p.sort_order,p.name");
  }
  std::vector<Ref> projectsInArea(int areaId) {
    return refs(
        "SELECT id,name,'' FROM projects WHERE status='open' AND area_id=" +
        std::to_string(areaId) + " ORDER BY sort_order,name");
  }
  int projectAreaId(int projectId) {
    auto *s = prep("SELECT COALESCE(area_id,0) FROM projects WHERE id=?");
    sqlite3_bind_int(s, 1, projectId);
    int a = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
      a = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return a;
  }
  bool projectIsOpen(int id) {
    auto *s = prep("SELECT status FROM projects WHERE id=?");
    sqlite3_bind_int(s, 1, id);
    bool open = true;
    if (sqlite3_step(s) == SQLITE_ROW)
      open = tx(s, 0) == "open";
    sqlite3_finalize(s);
    return open;
  }

  // -- everything searchable by the fuzzy finder -------------------------
  std::vector<Item> searchIndex() {
    std::vector<Item> out;
    auto as = read(
        "SELECT id,'a',name,'',0,'',0,'',0,'','','',0,'open','',sort_order "
        "FROM areas WHERE status='open' ORDER BY name");
    auto ps = read("SELECT "
                   "p.id,'p',p.name,p.description,COALESCE(p.area_id,0),"
                   "COALESCE(a.name,''),0,'',0,"
                   "COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',0,p."
                   "status,'',p.sort_order "
                   "FROM projects p LEFT JOIN areas a ON a.id=p.area_id WHERE "
                   "p.status='open' ORDER BY p.name");
    auto ts = read("SELECT "
                   "t.id,'t',t.title,t.notes,COALESCE(t.area_id,0),COALESCE(a."
                   "name,''),COALESCE(t.project_id,0),"
                   "COALESCE(pr.name,''),0,COALESCE(t.do_date,''),COALESCE(t."
                   "deadline,''),t.tags,t.someday,t.status,'',t.sort_order "
                   "FROM tasks t LEFT JOIN areas a ON a.id=t.area_id LEFT JOIN "
                   "projects pr ON pr.id=t.project_id "
                   "WHERE t.status='open' ORDER BY t.title");
    out.insert(out.end(), as.begin(), as.end());
    out.insert(out.end(), ps.begin(), ps.end());
    out.insert(out.end(), ts.begin(), ts.end());
    return out;
  }

  // -- smart views --------------------------------------------------------
  std::vector<Item> viewInbox(const std::string &tf) {
    return taskQuery("t.status='open' AND t.area_id IS NULL AND t.project_id "
                     "IS NULL AND t.do_date IS NULL AND t.someday=0",
                     "t.sort_order,t.id", tf);
  }
  std::vector<Item> viewAnytime(const std::string &tf) {
    return taskQuery("t.status='open' AND t.someday=0 AND t.do_date IS NULL",
                     "t.sort_order,t.id", tf);
  }
  std::vector<Item> viewSomeday(const std::string &tf) {
    return taskQuery("t.status='open' AND t.someday=1", "t.sort_order,t.id",
                     tf);
  }
  std::vector<Item> viewLogbook() {
    return taskQuery("t.status='done'", "t.completed_at DESC", "");
  }
  std::vector<Item> viewLoggedProjects() {
    return read("SELECT "
                "p.id,'p',p.name,p.description,COALESCE(p.area_id,0),COALESCE("
                "a.name,''),0,'',0,"
                "COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',0,p.status,"
                "COALESCE(p.completed_at,''),p.sort_order "
                "FROM projects p LEFT JOIN areas a ON a.id=p.area_id WHERE "
                "p.status!='open' ORDER BY p.completed_at DESC");
  }
  std::vector<Item> viewArchivedAreas() {
    return read("SELECT "
                "id,'a',name,'',0,'',0,'',0,'','','',0,status,COALESCE("
                "completed_at,''),sort_order "
                "FROM areas WHERE status!='open' ORDER BY completed_at DESC");
  }

  // Today/Tomorrow: tasks and projects due on/for the target date, merged
  // into one manually-orderable list. If *every* open task in a project is
  // also due that date, the tasks are folded away (the project row already
  // implies them); otherwise the matching tasks stay visible alongside it.
  std::vector<Item> viewDay(bool tomorrow, const std::string &tf) {
    std::string target = tomorrow ? "date('now','localtime','+1 day')"
                                  : "date('now','localtime')";
    std::string cmp = tomorrow ? "=" : "<=";
    std::string taskWhere = "t.status='open' AND t.someday=0 AND (t.do_date " +
                            cmp + " " + target + " OR t.deadline " + cmp + " " +
                            target + ")";
    std::string projWhere = "p.status='open' AND (p.do_date " + cmp + " " +
                            target + " OR p.deadline " + cmp + " " + target +
                            ")";
    auto tasks = taskQuery(taskWhere, "t.sort_order,t.id", tf);
    auto projects =
        read("SELECT "
             "p.id,'p',p.name,p.description,COALESCE(p.area_id,0),COALESCE(a."
             "name,''),0,'',0,"
             "COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',0,p.status,'',"
             "p.sort_order "
             "FROM projects p LEFT JOIN areas a ON a.id=p.area_id WHERE " +
             projWhere + " ORDER BY p.sort_order,p.id");
    foldProjectTasks(projects, tasks, [&](const Item &t) {
      return (!t.doDate.empty() &&
              (tomorrow ? t.doDate == today_plus(1) : t.doDate <= today())) ||
             (!t.deadline.empty() &&
              (tomorrow ? t.deadline == today_plus(1) : t.deadline <= today()));
    });
    std::vector<Item> out = projects;
    out.insert(out.end(), tasks.begin(), tasks.end());
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) {
      return a.sortOrder < b.sortOrder;
    });
    return out;
  }

  std::vector<Item> viewUpcoming(const std::string &tf) {
    auto tasks = taskQuery("t.status='open' AND t.someday=0 AND t.do_date>" +
                               std::string("date('now','localtime')"),
                           "t.do_date,t.sort_order,t.id", tf);
    auto projects =
        read("SELECT "
             "p.id,'p',p.name,p.description,COALESCE(p.area_id,0),COALESCE(a."
             "name,''),0,'',0,"
             "COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',0,p.status,'',"
             "p.sort_order "
             "FROM projects p LEFT JOIN areas a ON a.id=p.area_id WHERE "
             "p.status='open' AND p.do_date>date('now','localtime') "
             "ORDER BY p.do_date,p.sort_order,p.id");
    foldProjectTasks(
        projects, tasks, [&](const Item &t) { return !t.doDate.empty(); },
        /*sameDateAsProjectOnly=*/true);
    std::vector<Item> out = projects;
    out.insert(out.end(), tasks.begin(), tasks.end());
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) {
      std::string da = a.doDate.empty() ? a.deadline : a.doDate;
      std::string db = b.doDate.empty() ? b.deadline : b.doDate;
      if (da != db)
        return da < db;
      return a.sortOrder < b.sortOrder;
    });
    return out;
  }

  std::vector<Item> viewDeadlines(const std::string &tf) {
    auto tasks = taskQuery("t.status='open' AND t.deadline IS NOT NULL",
                           "t.deadline,t.sort_order,t.id", tf);
    auto projects = read("SELECT "
                         "p.id,'p',p.name,p.description,COALESCE(p.area_id,0),"
                         "COALESCE(a.name,''),0,'',0,"
                         "COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',0,"
                         "p.status,'',p.sort_order "
                         "FROM projects p LEFT JOIN areas a ON a.id=p.area_id "
                         "WHERE p.status='open' AND p.deadline IS NOT NULL "
                         "ORDER BY p.deadline,p.sort_order,p.id");
    std::vector<Item> out = projects;
    out.insert(out.end(), tasks.begin(), tasks.end());
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) {
      if (a.deadline != b.deadline)
        return a.deadline < b.deadline;
      return a.sortOrder < b.sortOrder;
    });
    return out;
  }

  // Area detail: open projects (sorted by do date), then standalone tasks
  // split into undated / dated, then someday tasks — each its own section.
  std::vector<Item> viewArea(int areaId, const std::string &tf) {
    std::vector<Item> out;
    auto projects = read("SELECT "
                         "id,'p',name,description,0,'',0,'',0,COALESCE(do_date,"
                         "''),COALESCE(deadline,''),'',0,status,'',sort_order "
                         "FROM projects WHERE status='open' AND area_id=" +
                         std::to_string(areaId) +
                         " ORDER BY (do_date IS NULL),do_date,sort_order");
    auto undated = taskQuery(
        "t.status='open' AND t.someday=0 AND t.do_date IS NULL AND t.area_id=" +
            std::to_string(areaId) + " AND t.project_id IS NULL",
        "t.sort_order,t.id", tf);
    auto dated =
        taskQuery("t.status='open' AND t.someday=0 AND t.do_date IS NOT NULL "
                  "AND t.area_id=" +
                      std::to_string(areaId) + " AND t.project_id IS NULL",
                  "t.sort_order,t.id", tf);
    auto someday =
        taskQuery("t.status='open' AND t.someday=1 AND t.area_id=" +
                      std::to_string(areaId) + " AND t.project_id IS NULL",
                  "t.sort_order,t.id", tf);
    for (auto &p : projects)
      p.section = "Projects";
    for (auto &t : undated)
      t.section = "Tasks";
    for (auto &t : dated)
      t.section = "Scheduled";
    for (auto &t : someday)
      t.section = "Someday";
    out.insert(out.end(), projects.begin(), projects.end());
    out.insert(out.end(), undated.begin(), undated.end());
    out.insert(out.end(), dated.begin(), dated.end());
    out.insert(out.end(), someday.begin(), someday.end());
    return out;
  }

  // Project detail: tasks are bucketed by heading (ungrouped first, then
  // headings in their own order), and sorted by do date inside each bucket.
  // If the project itself isn't open, show every task regardless of status
  // so a finished project can still be reviewed.
  std::vector<Item> viewProject(int projectId, const std::string &tf,
                                bool projectIsOpen) {
    std::string statusClause = projectIsOpen ? "t.status='open' AND " : "";
    auto tasks = taskQuery(
        statusClause + "t.project_id=" + std::to_string(projectId), "t.id", tf);
    auto headings = read("SELECT "
                         "id,'h',title,'',0,'',project_id,'',id,'','','',0,'"
                         "open','',sort_order FROM headings "
                         "WHERE project_id=" +
                         std::to_string(projectId) + " ORDER BY sort_order,id");
    std::vector<std::pair<int, int>>
        bucketOrder; // (heading_id or -1, heading sort_order)
    bucketOrder.push_back({-1, -1});
    for (auto &h : headings)
      bucketOrder.push_back({h.id, h.sortOrder});
    std::sort(tasks.begin(), tasks.end(), [&](const Item &a, const Item &b) {
      int ba = a.headingId, bb = b.headingId;
      int oa = 0, ob = 0;
      for (auto &[hid, ord] : bucketOrder) {
        if (hid == ba)
          oa = ord;
        if (hid == bb)
          ob = ord;
      }
      if (oa != ob)
        return oa < ob;
      std::string da = a.doDate.empty() ? "9999-99-99" : a.doDate;
      std::string db = b.doDate.empty() ? "9999-99-99" : b.doDate;
      if (da != db)
        return da < db;
      return a.sortOrder < b.sortOrder;
    });
    std::vector<Item> out;
    int idx = 0;
    for (auto &h : headings) {
      while (idx < (int)tasks.size() && tasks[idx].headingId != h.id &&
             tasks[idx].headingId == 0)
        out.push_back(tasks[idx++]);
      out.push_back(h);
      while (idx < (int)tasks.size() && tasks[idx].headingId == h.id)
        out.push_back(tasks[idx++]);
    }
    while (idx < (int)tasks.size())
      out.push_back(tasks[idx++]);
    return out;
  }

  // -- mutation -------------------------------------------------------------
  void saveTask(Item t, int areaId, int projectId) {
    t.someday = t.doDate == "someday";
    if (t.someday)
      t.doDate.clear();
    sqlite3_stmt *s = prep(t.id ? "UPDATE tasks SET "
                                  "title=?,notes=?,tags=?,area_id=?,project_id="
                                  "?,do_date=?,deadline=?,someday=? WHERE id=?"
                                : "INSERT INTO "
                                  "tasks(title,notes,tags,area_id,project_id,"
                                  "do_date,deadline,someday,sort_order) "
                                  "VALUES(?,?,?,?,?,?,?,?,COALESCE((SELECT "
                                  "MAX(sort_order)+1 FROM tasks),0))");
    bind(s, 1, t.title);
    bind(s, 2, t.notes);
    bind(s, 3, t.tags);
    num(s, 4, areaId);
    num(s, 5, projectId);
    nullable(s, 6, t.doDate);
    nullable(s, 7, t.deadline);
    sqlite3_bind_int(s, 8, t.someday ? 1 : 0);
    if (t.id)
      sqlite3_bind_int(s, 9, t.id);
    step(s);
  }
  void saveProject(Item p, int areaId) {
    sqlite3_stmt *s = prep(
        p.id ? "UPDATE projects SET "
               "name=?,description=?,area_id=?,do_date=?,deadline=? WHERE id=?"
             : "INSERT INTO "
               "projects(name,description,area_id,do_date,deadline,sort_order) "
               "VALUES(?,?,?,?,?,COALESCE((SELECT MAX(sort_order)+1 FROM "
               "projects),0))");
    bind(s, 1, p.title);
    bind(s, 2, p.notes);
    num(s, 3, areaId);
    nullable(s, 4, p.doDate);
    nullable(s, 5, p.deadline);
    if (p.id)
      sqlite3_bind_int(s, 6, p.id);
    step(s);
  }
  void renameArea(int id, const std::string &name) {
    sqlite3_stmt *s = prep("UPDATE areas SET name=? WHERE id=?");
    bind(s, 1, name);
    sqlite3_bind_int(s, 2, id);
    step(s);
  }
  void addArea(const std::string &name) {
    stmt("INSERT INTO areas(name,sort_order) VALUES(?,COALESCE((SELECT "
         "MAX(sort_order)+1 FROM areas),0))",
         {name});
  }
  int addHeading(int projectId, const std::string &title) {
    sqlite3_stmt *s =
        prep("INSERT INTO headings(project_id,title,sort_order) VALUES(?,?,"
             "COALESCE((SELECT MAX(sort_order)+1 FROM headings WHERE "
             "project_id=?),0))");
    sqlite3_bind_int(s, 1, projectId);
    bind(s, 2, title);
    sqlite3_bind_int(s, 3, projectId);
    step(s);
    return (int)sqlite3_last_insert_rowid(db_);
  }
  void setTaskHeading(int taskId, int headingId) {
    sqlite3_stmt *s = prep("UPDATE tasks SET heading_id=? WHERE id=?");
    num(s, 1, headingId);
    sqlite3_bind_int(s, 2, taskId);
    step(s);
  }
  void moveTask(int taskId, int areaId, int projectId) {
    sqlite3_stmt *s = prep(
        "UPDATE tasks SET area_id=?,project_id=?,heading_id=NULL WHERE id=?");
    num(s, 1, areaId);
    num(s, 2, projectId);
    sqlite3_bind_int(s, 3, taskId);
    step(s);
  }
  void complete(const Item &i) {
    std::string col = i.kind == 'p' ? "completed" : "done";
    exec("UPDATE " + std::string(tableFor(i.kind)) + " SET status='" + col +
         "',completed_at=strftime('%Y-%m-%dT%H:%M:%f','now','localtime') WHERE "
         "id=" +
         std::to_string(i.id));
  }
  void cancel(const Item &i) {
    exec("UPDATE " + std::string(tableFor(i.kind)) +
         " SET "
         "status='cancelled',completed_at=strftime('%Y-%m-%dT%H:%M:%f','now','"
         "localtime') WHERE id=" +
         std::to_string(i.id));
  }
  void erase(const Item &i) {
    exec("DELETE FROM " + std::string(tableFor(i.kind)) +
         " WHERE id=" + std::to_string(i.id));
  }
  // Swaps position between any two rows (even across the tasks/projects/
  // headings tables), which is what lets J/K reorder a single merged list.
  void swapOrder(const Item &a, const Item &b) {
    exec("UPDATE " + std::string(tableFor(a.kind)) + " SET sort_order=" +
         std::to_string(b.sortOrder) + " WHERE id=" + std::to_string(a.id));
    exec("UPDATE " + std::string(tableFor(b.kind)) + " SET sort_order=" +
         std::to_string(a.sortOrder) + " WHERE id=" + std::to_string(b.id));
  }

  void migrate() {
    exec("CREATE TABLE IF NOT EXISTS areas(id INTEGER PRIMARY KEY,name TEXT "
         "UNIQUE NOT NULL,status TEXT NOT NULL "
         "DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT NULL DEFAULT "
         "0);");
    exec("CREATE TABLE IF NOT EXISTS projects(id INTEGER PRIMARY KEY,name TEXT "
         "NOT NULL UNIQUE,description TEXT NOT "
         "NULL DEFAULT '',area_id INTEGER REFERENCES areas(id),do_date "
         "TEXT,deadline TEXT,status TEXT NOT NULL "
         "DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT NULL DEFAULT "
         "0);");
    exec("CREATE TABLE IF NOT EXISTS tasks(id INTEGER PRIMARY KEY,title TEXT "
         "NOT NULL,notes TEXT NOT NULL DEFAULT "
         "'',tags TEXT NOT NULL DEFAULT '',project_id INTEGER REFERENCES "
         "projects(id) ON DELETE SET NULL,area_id "
         "INTEGER REFERENCES areas(id) ON DELETE SET NULL,heading_id "
         "INTEGER,do_date TEXT,deadline TEXT,someday "
         "INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT "
         "'open',completed_at TEXT,sort_order INTEGER NOT "
         "NULL DEFAULT 0,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
    exec("CREATE TABLE IF NOT EXISTS headings(id INTEGER PRIMARY "
         "KEY,project_id INTEGER NOT NULL REFERENCES "
         "projects(id) ON DELETE CASCADE,title TEXT NOT NULL,sort_order "
         "INTEGER NOT NULL DEFAULT 0);");
    for (auto q : {"ALTER TABLE tasks ADD COLUMN heading_id INTEGER"})
      try {
        exec(q);
      } catch (...) {
      }
  }

private:
  sqlite3 *db_{};
  sqlite3_stmt *prep(const std::string &q) {
    sqlite3_stmt *s{};
    sqlite3_prepare_v2(db_, q.c_str(), -1, &s, nullptr);
    return s;
  }
  static std::string tx(sqlite3_stmt *s, int n) {
    auto p = (const char *)sqlite3_column_text(s, n);
    return p ? p : "";
  }
  static void bind(sqlite3_stmt *s, int n, const std::string &v) {
    sqlite3_bind_text(s, n, v.c_str(), -1, SQLITE_TRANSIENT);
  }
  static void num(sqlite3_stmt *s, int n, int v) {
    if (v)
      sqlite3_bind_int(s, n, v);
    else
      sqlite3_bind_null(s, n);
  }
  static void nullable(sqlite3_stmt *s, int n, const std::string &v) {
    if (v.empty())
      sqlite3_bind_null(s, n);
    else
      bind(s, n, v);
  }
  void step(sqlite3_stmt *s) {
    sqlite3_step(s);
    sqlite3_finalize(s);
  }
  void stmt(const std::string &q, const std::vector<std::string> &v) {
    auto *s = prep(q);
    for (int i = 0; i < (int)v.size(); ++i)
      bind(s, i + 1, v[i]);
    step(s);
  }
  std::vector<Ref> refs(const std::string &q) {
    std::vector<Ref> o;
    auto *s = prep(q);
    while (sqlite3_step(s) == SQLITE_ROW)
      o.push_back({sqlite3_column_int(s, 0), tx(s, 1), tx(s, 2)});
    sqlite3_finalize(s);
    return o;
  }
  // Every SELECT in this file, no matter the table, produces these 16
  // columns in this order so one mapping function can serve all of them.
  std::vector<Item> read(const std::string &q) {
    std::vector<Item> out;
    auto *s = prep(q);
    while (sqlite3_step(s) == SQLITE_ROW) {
      Item it;
      it.id = sqlite3_column_int(s, 0);
      std::string k = tx(s, 1);
      it.kind = k.empty() ? 't' : k[0];
      it.title = tx(s, 2);
      it.notes = tx(s, 3);
      it.areaId = sqlite3_column_int(s, 4);
      it.areaName = tx(s, 5);
      it.projectId = sqlite3_column_int(s, 6);
      it.projectName = tx(s, 7);
      it.headingId = sqlite3_column_int(s, 8);
      it.doDate = tx(s, 9);
      it.deadline = tx(s, 10);
      it.tags = tx(s, 11);
      it.someday = sqlite3_column_int(s, 12) != 0;
      it.status = tx(s, 13);
      it.completedAt = tx(s, 14);
      it.sortOrder = sqlite3_column_int(s, 15);
      out.push_back(it);
    }
    sqlite3_finalize(s);
    return out;
  }
  std::vector<Item> taskQuery(const std::string &where,
                              const std::string &order,
                              const std::string &tagFilter) {
    std::string w = where;
    if (tagFilter == "none") {
      w += " AND (t.tags IS NULL OR t.tags='')";
    } else if (!tagFilter.empty()) {
      for (auto &raw : splitComma(tagFilter)) {
        auto tag = sanitizeTag(raw);
        if (tag.empty())
          continue;
        w += " AND instr(','||replace(lower(t.tags),' ','')||',','," + tag +
             ",')>0";
      }
    }
    std::string q = "SELECT "
                    "t.id,'t',t.title,t.notes,COALESCE(t.area_id,0),COALESCE(a."
                    "name,''),COALESCE(t.project_id,0),"
                    "COALESCE(pr.name,''),COALESCE(t.heading_id,0),COALESCE(t."
                    "do_date,''),COALESCE(t.deadline,''),t.tags,"
                    "t.someday,t.status,COALESCE(t.completed_at,''),t.sort_"
                    "order FROM tasks t "
                    "LEFT JOIN areas a ON a.id=t.area_id LEFT JOIN projects pr "
                    "ON pr.id=t.project_id WHERE " +
                    w + " ORDER BY " + order;
    return read(q);
  }
  static std::string today_plus(int days) {
    std::time_t t = std::time(nullptr) + days * 86400;
    char buf[11]{};
    std::strftime(buf, sizeof(buf), "%F", std::localtime(&t));
    return buf;
  }
  // Removes tasks from `tasks` that belong to a project in `projects` when
  // *every* open task of that project matches `matches`. `sameDateOnly`
  // additionally requires the task's do date to equal the project's.
  template <typename Pred>
  void foldProjectTasks(const std::vector<Item> &projects,
                        std::vector<Item> &tasks, Pred matches,
                        bool sameDateOnly = false) {
    for (auto &p : projects) {
      auto open = read("SELECT "
                       "id,'t','','',0,'',0,'',0,COALESCE(do_date,''),COALESCE("
                       "deadline,''),'',0,status,'',0 "
                       "FROM tasks WHERE project_id=" +
                       std::to_string(p.id) + " AND status='open'");
      if (open.empty())
        continue;
      bool allMatch = std::all_of(open.begin(), open.end(), [&](const Item &t) {
        if (sameDateOnly)
          return !t.doDate.empty() && t.doDate == p.doDate;
        return matches(t);
      });
      if (!allMatch)
        continue;
      tasks.erase(
          std::remove_if(tasks.begin(), tasks.end(),
                         [&](const Item &t) { return t.projectId == p.id; }),
          tasks.end());
    }
  }
};

// ---------------------------------------------------------------------------
// Fuzzy matching for the 'f' finder
// ---------------------------------------------------------------------------

// Ordered-subsequence match: -1 if `needle` isn't a subsequence of
// `haystack`, otherwise a score rewarding early, contiguous hits so tighter
// matches float to the top as you type.
static int fuzzyScore(const std::string &needleRaw,
                      const std::string &haystackRaw) {
  if (needleRaw.empty())
    return 0;
  std::string needle = lower(needleRaw), hay = lower(haystackRaw);
  int score = 0, ni = 0, streak = 0;
  for (int hi = 0; hi < (int)hay.size() && ni < (int)needle.size(); ++hi) {
    if (hay[hi] == needle[ni]) {
      score += 10 + streak * 4 - hi / 4;
      streak++;
      ni++;
    } else {
      streak = 0;
    }
  }
  return ni == (int)needle.size() ? score : -1;
}

static std::string friendlyDate(const std::string &iso) {
  if (iso.empty())
    return "No date";
  std::tm tmv{};
  strptime(iso.c_str(), "%Y-%m-%d", &tmv);
  std::mktime(&tmv);
  char buf[24];
  std::strftime(buf, sizeof(buf), "%a, %b %d", &tmv);
  return buf;
}

// Buckets a completed_at timestamp into Today / Yesterday / "Month Year"
// (current year) / "Year" (past years), for the Logbook-style views.
static std::string bucketLabel(const std::string &completedAtIso) {
  if (completedAtIso.size() < 10)
    return "Earlier";
  std::string date = completedAtIso.substr(0, 10);
  std::string tdy = today();
  if (date == tdy)
    return "Today";
  std::tm tmv{};
  strptime(tdy.c_str(), "%Y-%m-%d", &tmv);
  std::time_t t = std::mktime(&tmv) - 86400;
  char yb[11];
  std::strftime(yb, sizeof(yb), "%F", std::localtime(&t));
  if (date == yb)
    return "Yesterday";
  int y = std::stoi(date.substr(0, 4)), m = std::stoi(date.substr(5, 2));
  int cy = std::stoi(tdy.substr(0, 4));
  static const char *months[] = {
      "",     "January", "February",  "March",   "April",    "May",     "June",
      "July", "August",  "September", "October", "November", "December"};
  if (y == cy)
    return std::string(months[m]) + " " + std::to_string(y);
  return std::to_string(y);
}

struct Candidate {
  char kind; // 'v' view, 'H' hidden view, 'a' area, 'p' project, 't' task
  std::string icon;
  std::string label;
  std::string sub;
  std::string viewName; // only for 'v'/'H'
  Item raw;             // only for 'a'/'p'/'t'
};

// ---------------------------------------------------------------------------
// App — ncurses rendering and input handling
// ---------------------------------------------------------------------------

class App {
public:
  explicit App(Store &s) : s_(s) {}

  void run() {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_RED, -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLORS >= 16 ? 8 : COLOR_WHITE, -1);
    while (on_) {
      load();
      draw();
      handle(getch());
    }
    endwin();
  }

private:
  Store &s_;
  bool on_ = true, sidebar_ = true, group_ = false;
  int pick_ = 0, view_ = 0, scope_ = 0;
  char scopeKind_ = 0;
  std::string scopeName_, scopeAreaName_, tags_, hidden_;
  std::vector<Item> list_;
  std::vector<std::string> views_{"Inbox",   "Today",   "Upcoming",
                                  "Anytime", "Someday", "Logbook"};

  std::string active() const {
    return hidden_.empty() ? views_[view_] : hidden_;
  }

  std::string name() const {
    if (scopeKind_ == 'p')
      return scopeAreaName_.empty() ? scopeName_
                                    : scopeAreaName_ + " / " + scopeName_;
    if (scopeKind_ == 'a')
      return scopeName_;
    return active();
  }

  void load() {
    if (scopeKind_ == 'p') {
      list_ = s_.viewProject(scope_, tags_, s_.projectIsOpen(scope_));
    } else if (scopeKind_ == 'a') {
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
    pick_ = std::clamp(pick_, 0, std::max(0, (int)list_.size() - 1));
  }

  std::string groupLabel(const Item &x) const {
    if (scopeKind_ == 'a')
      return x.section;
    if (scopeKind_ == 'p')
      return "";
    if (group_) {
      if (!x.projectName.empty())
        return x.areaName.empty() ? x.projectName
                                  : x.areaName + " / " + x.projectName;
      if (!x.areaName.empty())
        return x.areaName;
      return "Inbox / Anytime";
    }
    std::string v = active();
    if (v == "Upcoming")
      return friendlyDate(x.doDate.empty() ? x.deadline : x.doDate);
    if (v == "Deadlines")
      return friendlyDate(x.deadline);
    if (v == "Logbook" || v == "Logged Projects" || v == "Archived Areas")
      return bucketLabel(x.completedAt);
    return "";
  }

  // ---- rendering ---------------------------------------------------------

  void draw() {
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

  void drawSidebar(int rows, int width) {
    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(1, 2, "\xe2\x97\x89 STRIDE");
    attroff(A_BOLD | COLOR_PAIR(1));
    mvprintw(3, 2, "FOCUS");
    for (int i = 0; i < (int)views_.size(); ++i) {
      bool cur = scopeKind_ == 0 && hidden_.empty() && i == view_;
      if (cur)
        attron(A_REVERSE);
      mvprintw(5 + i, 2, "%s %s", cur ? "\xe2\x96\xa3" : "\xe2\x97\x8b",
               clip(views_[i], width - 6).c_str());
      if (cur)
        attroff(A_REVERSE);
    }
    int y = 5 + (int)views_.size() + 1;
    mvprintw(y++, 2, "AREAS");
    for (auto &a : s_.areas()) {
      if (y >= rows - 3)
        break;
      bool cur = scopeKind_ == 'a' && scope_ == a.id;
      if (cur)
        attron(A_REVERSE);
      mvprintw(y++, 2, "\xe2\x97\x88 %s", clip(a.name, width - 6).c_str());
      if (cur)
        attroff(A_REVERSE);
      for (auto &p : s_.projectsInArea(a.id)) {
        if (y >= rows - 3)
          break;
        bool curp = scopeKind_ == 'p' && scope_ == p.id;
        if (curp)
          attron(A_REVERSE);
        mvprintw(y++, 4, "\xe2\x96\xb9 %s", clip(p.name, width - 8).c_str());
        if (curp)
          attroff(A_REVERSE);
      }
    }
    for (auto &p : s_.projects()) {
      if (!p.sub.empty())
        continue; // has an area, already listed above
      if (y >= rows - 3)
        break;
      bool curp = scopeKind_ == 'p' && scope_ == p.id;
      if (curp)
        attron(A_REVERSE);
      mvprintw(y++, 2, "\xe2\x97\x87 %s", clip(p.name, width - 6).c_str());
      if (curp)
        attroff(A_REVERSE);
    }
    attron(A_DIM);
    mvprintw(rows - 2, 2, "%s", clip("f find  b sidebar", width - 4).c_str());
    attroff(A_DIM);
    mvvline(0, width, ACS_VLINE, rows);
  }

  void drawMain(int rows, int cols, int off) {
    int innerW = cols - off - 4;
    attron(A_BOLD);
    std::string title = name();
    if (group_ && scopeKind_ == 0)
      title += " \xc2\xb7 grouped";
    mvprintw(1, off + 3, "%s", clip(title, innerW - 20).c_str());
    attroff(A_BOLD);
    if (!tags_.empty()) {
      attron(A_DIM);
      std::string tagLabel = clip("tags: " + tags_, 24);
      mvprintw(1, std::max(off + 3, cols - (int)tagLabel.size() - 2), "%s",
               tagLabel.c_str());
      attroff(A_DIM);
    }
    mvhline(2, off + 2, ACS_HLINE, std::max(0, cols - off - 4));

    if (list_.empty()) {
      attron(A_DIM);
      mvprintw(
          4, off + 4, "%s",
          clip("Nothing here yet. n captures a next action.", innerW).c_str());
      attroff(A_DIM);
    }

    std::string last;
    int y = 4;
    for (int i = 0; i < (int)list_.size() && y < rows - 3; ++i) {
      auto &x = list_[i];
      std::string grp = groupLabel(x);
      if (!grp.empty() && grp != last) {
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(y++, off + 3, "%s", clip(grp, innerW).c_str());
        attroff(A_BOLD | COLOR_PAIR(3));
        last = grp;
        if (y >= rows - 3)
          break;
      }
      bool selected = i == pick_;
      if (selected)
        attron(A_REVERSE);
      if (x.kind == 'h') {
        attron(A_BOLD);
        mvprintw(y, off + 4, "%s",
                 clip("\xe2\x80\x94 " + x.title, innerW - 2).c_str());
        attroff(A_BOLD);
      } else {
        bool graySomeday = x.someday && x.status == "open";
        bool dim = (x.status != "open") || graySomeday;
        bool overdue = !dim && !x.deadline.empty() && x.deadline <= today();
        if (graySomeday)
          attron(COLOR_PAIR(4));
        else if (dim)
          attron(A_DIM);
        if (overdue)
          attron(COLOR_PAIR(2));
        std::string icon = x.kind == 'p'   ? "\xe2\x97\x87"
                           : x.kind == 'a' ? "\xe2\x97\x88"
                                           : "\xe2\x97\x8b";
        std::string mark = x.status != "open" ? "\xe2\x9c\x93 " : "";
        int tagCol = 22;
        mvprintw(y, off + 4, "%s",
                 clip(icon + " " + mark + x.title, innerW - tagCol).c_str());
        if (overdue)
          attroff(COLOR_PAIR(2));
        if (graySomeday)
          attroff(COLOR_PAIR(4));
        else if (dim)
          attroff(A_DIM);
        std::string ctx;
        if (!group_ && scopeKind_ == 0) {
          if (!x.projectName.empty())
            ctx = !x.areaName.empty() ? x.areaName + "/" + x.projectName
                                      : x.projectName;
          else
            ctx = x.areaName;
        }
        if (!ctx.empty()) {
          attron(A_DIM);
          mvprintw(y, cols - tagCol, "%s", clip(ctx, tagCol - 2).c_str());
          attroff(A_DIM);
        }
      }
      if (selected)
        attroff(A_REVERSE);
      ++y;
    }
    attron(A_DIM);
    std::string hints = "j/k move  J/K reorder  n new  N heading  p project  a "
                        "area  f find  T tags  A group  x done  X list  e "
                        "edit  m move  ? help";
    mvprintw(rows - 2, off + 3, "%s",
             clip(hints, std::max(0, cols - off - 5)).c_str());
    attroff(A_DIM);
  }

  // ---- small form helpers -------------------------------------------------

  WINDOW *openForm(int h, int w, const std::string &title) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    h = std::min(h, rows - 2);
    w = std::min(w, cols - 2);
    WINDOW *win =
        newwin(h, w, std::max(0, (rows - h) / 2), std::max(0, (cols - w) / 2));
    box(win, 0, 0);
    if (!title.empty()) {
      wattron(win, A_BOLD | COLOR_PAIR(1));
      mvwprintw(win, 1, 2, "%s", clip(title, w - 4).c_str());
      wattroff(win, A_BOLD | COLOR_PAIR(1));
    }
    return win;
  }

  static void clearField(WINDOW *w, int y, int x, int width) {
    if (width <= 0)
      return;
    mvwprintw(w, y, x, "%s", std::string(width, ' ').c_str());
  }

  // Reads one field's value; Enter with no input keeps `keepIfBlank`.
  static std::string readAt(WINDOW *w, int y, int x, int maxlen,
                            const std::string &keepIfBlank) {
    maxlen = std::max(maxlen, 1);
    echo();
    curs_set(1);
    wmove(w, y, x);
    wrefresh(w);
    std::vector<char> buf(maxlen + 1, 0);
    wgetnstr(w, buf.data(), maxlen);
    noecho();
    curs_set(0);
    std::string v(buf.data());
    return v.empty() ? keepIfBlank : v;
  }

  // Like readAt, but returns exactly what was typed (used where an empty
  // answer is meaningful, e.g. clearing the tag filter).
  static std::string readLine(WINDOW *w, int y, int x, int maxlen) {
    maxlen = std::max(maxlen, 1);
    echo();
    curs_set(1);
    wmove(w, y, x);
    wrefresh(w);
    std::vector<char> buf(maxlen + 1, 0);
    wgetnstr(w, buf.data(), maxlen);
    noecho();
    curs_set(0);
    return std::string(buf.data());
  }

  // Draws every label + current value up front and refreshes once, so the
  // whole form is visible immediately instead of revealing one field at a
  // time as the user tabs through it.
  static void
  drawFormLabels(WINDOW *w,
                 const std::vector<std::pair<std::string, std::string>> &fields,
                 int startY, int valueX) {
    int wh, ww;
    getmaxyx(w, wh, ww);
    (void)wh;
    for (int i = 0; i < (int)fields.size(); ++i) {
      mvwprintw(w, startY + i, 2, "%-*s", valueX - 3, fields[i].first.c_str());
      wattron(w, A_DIM);
      mvwprintw(w, startY + i, valueX, "%s",
                clip(fields[i].second, ww - valueX - 2).c_str());
      wattroff(w, A_DIM);
    }
  }

  int findAreaId(const std::string &name) {
    std::string n = lower(trimmed(name));
    if (n.empty())
      return 0;
    for (auto &a : s_.areas())
      if (lower(a.name) == n)
        return a.id;
    return 0;
  }
  int findProjectId(const std::string &name) {
    std::string n = lower(trimmed(name));
    if (n.empty())
      return 0;
    for (auto &p : s_.projects())
      if (lower(p.name) == n)
        return p.id;
    return 0;
  }

  // ---- forms ---------------------------------------------------------------

  void taskForm(std::optional<Item> e = {}) {
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
    int w = 68, h = 15;
    WINDOW *win = openForm(h, w, t.id ? "EDIT TASK" : "NEW TASK");
    getmaxyx(win, h, w);
    std::vector<std::pair<std::string, std::string>> fields = {
        {"Title", t.title},
        {"Description", t.notes},
        {"Tags", t.tags},
        {"Do date", t.someday ? "someday" : t.doDate},
        {"Deadline", t.deadline},
        {"Area", areaDefault},
        {"Project", projectDefault},
    };
    int startY = 3, valueX = 15;
    drawFormLabels(win, fields, startY, valueX);
    mvwprintw(
        win, startY + (int)fields.size() + 1, 2, "%s",
        clip("YYYY-MM-DD or 'someday'  \xc2\xb7  Enter saves, Esc cancels",
             w - 4)
            .c_str());
    wrefresh(win);
    std::vector<std::string> values;
    for (int i = 0; i < (int)fields.size(); ++i) {
      clearField(win, startY + i, valueX, w - valueX - 3);
      values.push_back(
          readAt(win, startY + i, valueX, w - valueX - 4, fields[i].second));
    }
    int ch = wgetch(win);
    delwin(win);
    if (ch == 27 || values[0].empty())
      return;
    t.title = values[0];
    t.notes = values[1];
    t.tags = values[2];
    t.doDate = values[3];
    t.deadline = values[4];
    int areaId = findAreaId(values[5]);
    int projectId = findProjectId(values[6]);
    if (projectId && !areaId)
      areaId = s_.projectAreaId(projectId);
    s_.saveTask(t, areaId, projectId);
  }

  void projectForm(std::optional<Item> e = {}) {
    Item p = e.value_or(Item{});
    std::string areaDefault = p.areaName;
    if (!p.id && scopeKind_ == 'a')
      areaDefault = scopeName_;
    int w = 64, h = 12;
    WINDOW *win = openForm(h, w, p.id ? "EDIT PROJECT" : "NEW PROJECT");
    getmaxyx(win, h, w);
    std::vector<std::pair<std::string, std::string>> fields = {
        {"Name", p.title},     {"Description", p.notes}, {"Area", areaDefault},
        {"Do date", p.doDate}, {"Deadline", p.deadline},
    };
    int startY = 3, valueX = 15;
    drawFormLabels(win, fields, startY, valueX);
    mvwprintw(win, startY + (int)fields.size() + 1, 2, "%s",
              clip("Enter saves, Esc cancels", w - 4).c_str());
    wrefresh(win);
    std::vector<std::string> values;
    for (int i = 0; i < (int)fields.size(); ++i) {
      clearField(win, startY + i, valueX, w - valueX - 3);
      values.push_back(
          readAt(win, startY + i, valueX, w - valueX - 4, fields[i].second));
    }
    int ch = wgetch(win);
    delwin(win);
    if (ch == 27 || values[0].empty())
      return;
    p.title = values[0];
    p.notes = values[1];
    p.doDate = values[3];
    p.deadline = values[4];
    s_.saveProject(p, findAreaId(values[2]));
  }

  void areaForm(std::optional<Item> e = {}) {
    Item a = e.value_or(Item{});
    int h = 6, w = 50;
    WINDOW *win = openForm(h, w, a.id ? "RENAME AREA" : "NEW AREA");
    getmaxyx(win, h, w);
    int fw = std::max(4, w - 14);
    mvwprintw(win, 3, 2, "Name");
    wattron(win, A_DIM);
    mvwprintw(win, 3, 12, "%s", clip(a.title, fw).c_str());
    wattroff(win, A_DIM);
    wrefresh(win);
    clearField(win, 3, 12, fw);
    std::string name = readAt(win, 3, 12, fw - 1, a.title);
    delwin(win);
    if (trimmed(name).empty())
      return;
    if (a.id)
      s_.renameArea(a.id, name);
    else
      s_.addArea(name);
  }

  void headingForm() {
    int h = 6, w = 48;
    WINDOW *win = openForm(h, w, "NEW HEADING");
    getmaxyx(win, h, w);
    int fw = std::max(4, w - 14);
    mvwprintw(win, 3, 2, "Title");
    wrefresh(win);
    std::string title = readAt(win, 3, 12, fw - 1, "");
    delwin(win);
    if (!trimmed(title).empty())
      s_.addHeading(scope_, title);
  }

  void tagFilterForm() {
    int h = 7, w = 56;
    WINDOW *win = openForm(h, w, "FILTER BY TAGS");
    getmaxyx(win, h, w);
    mvwprintw(
        win, 3, 2, "%s",
        clip("tag,tag  or 'none' for untagged  \xc2\xb7  blank clears", w - 4)
            .c_str());
    wattron(win, A_DIM);
    mvwprintw(win, 5, 2, "%s", clip(tags_, w - 4).c_str());
    wattroff(win, A_DIM);
    wrefresh(win);
    clearField(win, 5, 2, w - 4);
    tags_ = readLine(win, 5, 2, w - 5);
  }

  bool confirm(const std::string &message) {
    int w = std::clamp((int)message.size() + 10, 30, 70), h = 5;
    WINDOW *win = openForm(h, w, "");
    getmaxyx(win, h, w);
    mvwprintw(win, 2, 2, "%s", clip(message + " (y/N)", w - 4).c_str());
    wrefresh(win);
    int ch = wgetch(win);
    delwin(win);
    return ch == 'y' || ch == 'Y';
  }

  void lifecycle(const Item &i) {
    int h = 6, w = 46;
    WINDOW *win = openForm(h, w, i.kind == 'p' ? "PROJECT" : "AREA");
    getmaxyx(win, h, w);
    mvwprintw(win, 3, 2, "%s",
              clip("c complete   x cancel   d delete", w - 4).c_str());
    wrefresh(win);
    int k = wgetch(win);
    delwin(win);
    if (k == 'c')
      s_.complete(i);
    else if (k == 'x')
      s_.cancel(i);
    else if (k == 'd' && confirm("Delete \"" + i.title + "\" permanently?")) {
      s_.erase(i);
      if (scopeKind_ == i.kind && scope_ == i.id) {
        scopeKind_ = 0;
        pick_ = 0;
      }
    }
  }

  void help() {
    std::vector<std::string> lines = {
        "j/k            move selection",
        "h/l            switch list / go back",
        "J/K           reorder (crosses into a heading)",
        "Enter         open project",
        "n / p / a     new task / project / area",
        "N             new heading (inside a project)",
        "e             edit selected item",
        "x             complete task, or lifecycle for project/area",
        "X             lifecycle for the project/area you're inside",
        "m             move task to another project",
        "f             fuzzy find & jump anywhere",
        "T             filter by tags (tag,tag or 'none')",
        "A             group by area / project",
        "d             delete permanently (logbook & archives)",
        "b             toggle sidebar",
        "q             quit",
    };
    int h = (int)lines.size() + 4, w = 54;
    WINDOW *win = openForm(h, w, "SHORTCUTS");
    getmaxyx(win, h, w);
    for (int i = 0; i < (int)lines.size() && 3 + i < h - 1; ++i)
      mvwprintw(win, 3 + i, 2, "%s", clip(lines[i], w - 4).c_str());
    wrefresh(win);
    wgetch(win);
    delwin(win);
  }

  // ---- fuzzy finder --------------------------------------------------------

  std::vector<Candidate> buildCandidates() {
    std::vector<Candidate> c;
    for (auto &v : views_)
      c.push_back({'v', "\xe2\x96\xa3", v, "List", v, {}});
    for (std::string v :
         {"Tomorrow", "Deadlines", "Logged Projects", "Archived Areas"})
      c.push_back({'H', "\xe2\x96\xa3", v, "List", v, {}});
    for (auto &idx : s_.searchIndex()) {
      if (idx.kind == 'a')
        c.push_back({'a', "\xe2\x97\x88", idx.title, "Area", "", idx});
      else if (idx.kind == 'p')
        c.push_back({'p', "\xe2\x97\x87", idx.title,
                     idx.areaName.empty() ? "Project"
                                          : "Project \xc2\xb7 " + idx.areaName,
                     "", idx});
      else {
        std::string sub = !idx.projectName.empty() ? idx.projectName
                          : !idx.areaName.empty()  ? idx.areaName
                          : idx.someday            ? "Someday"
                                                   : "Task";
        c.push_back({'t', "\xe2\x97\x8b", idx.title, sub, "", idx});
      }
    }
    return c;
  }

  void openContainer(char kind, int id, const std::string &title,
                     const std::string &sub) {
    scopeKind_ = kind;
    scope_ = id;
    scopeName_ = title;
    scopeAreaName_ = sub;
    hidden_.clear();
    pick_ = 0;
  }

  std::string homeViewFor(const Item &t) const {
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

  void jumpToTask(const Item &t) {
    if (t.projectId) {
      openContainer('p', t.projectId, t.projectName, t.areaName);
    } else if (t.areaId) {
      openContainer('a', t.areaId, t.areaName, "");
    } else {
      scopeKind_ = 0;
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

  void find() {
    auto all = buildCandidates();
    std::string query;
    int sel = 0;
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int h = std::min(rows - 4, 20), w = std::min(cols - 4, 70);
    WINDOW *win =
        newwin(h, w, std::max(0, (rows - h) / 2), std::max(0, (cols - w) / 2));
    keypad(win, TRUE);
    while (true) {
      std::vector<std::pair<int, const Candidate *>> scored;
      for (auto &c : all) {
        int sc = fuzzyScore(query, c.label + " " + c.sub);
        if (sc >= 0)
          scored.push_back({sc, &c});
      }
      std::stable_sort(scored.begin(), scored.end(),
                       [](auto &a, auto &b) { return a.first > b.first; });
      int shown = std::min((int)scored.size(), h - 5);
      sel = std::clamp(sel, 0, std::max(0, shown - 1));
      werase(win);
      box(win, 0, 0);
      wattron(win, A_BOLD | COLOR_PAIR(1));
      mvwprintw(win, 1, 2, "FIND");
      wattroff(win, A_BOLD | COLOR_PAIR(1));
      mvwprintw(win, 2, 2, "> %s", clip(query, w - 6).c_str());
      mvwhline(win, 3, 1, ACS_HLINE, w - 2);
      for (int i = 0; i < shown; ++i) {
        auto *c = scored[i].second;
        if (i == sel)
          wattron(win, A_REVERSE);
        mvwprintw(win, 4 + i, 2, "%s",
                  clip(c->icon + " " + c->label, w - 26).c_str());
        wattron(win, A_DIM);
        mvwprintw(win, 4 + i, w - 22, "%s", clip(c->sub, 20).c_str());
        wattroff(win, A_DIM);
        if (i == sel)
          wattroff(win, A_REVERSE);
      }
      if (scored.empty()) {
        wattron(win, A_DIM);
        mvwprintw(win, 4, 2, "No matches");
        wattroff(win, A_DIM);
      }
      curs_set(1);
      wmove(win, 2, 4 + (int)query.size());
      wrefresh(win);
      int ch = wgetch(win);
      if (ch == 27)
        break;
      if (ch == '\n' || ch == KEY_ENTER) {
        if (!scored.empty()) {
          auto *c = scored[sel].second;
          if (c->kind == 'v') {
            scopeKind_ = 0;
            hidden_.clear();
            auto it = std::find(views_.begin(), views_.end(), c->viewName);
            view_ = it != views_.end() ? (int)(it - views_.begin()) : 0;
          } else if (c->kind == 'H') {
            scopeKind_ = 0;
            hidden_ = c->viewName;
          } else if (c->kind == 'a') {
            openContainer('a', c->raw.id, c->raw.title, "");
          } else if (c->kind == 'p') {
            openContainer('p', c->raw.id, c->raw.title, c->raw.areaName);
          } else if (c->kind == 't') {
            jumpToTask(c->raw);
          }
        }
        break;
      }
      if (ch == KEY_UP)
        sel = std::max(0, sel - 1);
      else if (ch == KEY_DOWN)
        sel++;
      else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (!query.empty())
          query.pop_back();
      } else if (ch >= 32 && ch < 127)
        query.push_back((char)ch);
    }
    curs_set(0);
    delwin(win);
  }

  // Returns the chosen project id (0 = Inbox / no project), or -1 if cancelled.
  int pickProject(const std::string &title) {
    std::vector<Ref> options;
    options.push_back({0, "Inbox (no project)", ""});
    for (auto &p : s_.projects())
      options.push_back(p);
    std::string query;
    int sel = 0;
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int h = std::min(rows - 4, 16), w = std::min(cols - 4, 60);
    WINDOW *win =
        newwin(h, w, std::max(0, (rows - h) / 2), std::max(0, (cols - w) / 2));
    keypad(win, TRUE);
    int result = -1;
    while (true) {
      std::vector<const Ref *> matches;
      for (auto &o : options)
        if (fuzzyScore(query, o.name) >= 0)
          matches.push_back(&o);
      int shown = std::min((int)matches.size(), h - 5);
      sel = std::clamp(sel, 0, std::max(0, shown - 1));
      werase(win);
      box(win, 0, 0);
      wattron(win, A_BOLD | COLOR_PAIR(1));
      mvwprintw(win, 1, 2, "%s", clip(title, w - 4).c_str());
      wattroff(win, A_BOLD | COLOR_PAIR(1));
      mvwprintw(win, 2, 2, "> %s", clip(query, w - 6).c_str());
      mvwhline(win, 3, 1, ACS_HLINE, w - 2);
      for (int i = 0; i < shown; ++i) {
        if (i == sel)
          wattron(win, A_REVERSE);
        std::string line =
            matches[i]->name +
            (matches[i]->sub.empty() ? "" : "  \xc2\xb7 " + matches[i]->sub);
        mvwprintw(win, 4 + i, 2, "%s", clip(line, w - 4).c_str());
        if (i == sel)
          wattroff(win, A_REVERSE);
      }
      curs_set(1);
      wmove(win, 2, 4 + (int)query.size());
      wrefresh(win);
      int ch = wgetch(win);
      if (ch == 27)
        break;
      if (ch == '\n' || ch == KEY_ENTER) {
        if (!matches.empty())
          result = matches[sel]->id;
        break;
      }
      if (ch == KEY_UP)
        sel = std::max(0, sel - 1);
      else if (ch == KEY_DOWN)
        sel++;
      else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (!query.empty())
          query.pop_back();
      } else if (ch >= 32 && ch < 127)
        query.push_back((char)ch);
    }
    curs_set(0);
    delwin(win);
    return result;
  }

  // ---- reordering ------------------------------------------------------

  void moveItem(int delta) {
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

  // ---- input dispatch ----------------------------------------------------

  void handle(int k) {
    if (k == 'q') {
      on_ = false;
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
    if ((k == 'h' || k == KEY_LEFT || k == 27) && scopeKind_ != 0) {
      scopeKind_ = 0;
      pick_ = 0;
      return;
    }
    if (k == 'h' || k == KEY_LEFT) {
      view_ = (view_ + (int)views_.size() - 1) % views_.size();
      hidden_.clear();
      pick_ = 0;
      return;
    }
    if (k == 'l' || k == KEY_RIGHT) {
      if (scopeKind_ != 0)
        return;
      view_ = (view_ + 1) % views_.size();
      hidden_.clear();
      pick_ = 0;
      return;
    }
    if (k == 'J' && !list_.empty() && pick_ + 1 < (int)list_.size()) {
      moveItem(1);
      return;
    }
    if (k == 'K' && !list_.empty() && pick_ > 0) {
      moveItem(-1);
      return;
    }
    if (k == 'b') {
      sidebar_ = !sidebar_;
      return;
    }
    if (k == 'n') {
      taskForm();
      return;
    }
    if (k == 'p') {
      projectForm();
      return;
    }
    if (k == 'a') {
      areaForm();
      return;
    }
    if (k == 'N' && scopeKind_ == 'p') {
      headingForm();
      return;
    }
    if (k == 'f') {
      find();
      return;
    }
    if (k == 'm' && !list_.empty() && list_[pick_].kind == 't') {
      int pid = pickProject("MOVE TO PROJECT");
      if (pid >= 0)
        s_.moveTask(list_[pick_].id, pid ? s_.projectAreaId(pid) : 0, pid);
      return;
    }
    if (k == 'T') {
      tagFilterForm();
      return;
    }
    if (k == 'A') {
      group_ = !group_;
      return;
    }
    if (k == 'x' && !list_.empty()) {
      if (list_[pick_].kind == 't')
        s_.complete(list_[pick_]);
      else
        lifecycle(list_[pick_]);
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
    if (k == 'd' && !list_.empty() &&
        (active() == "Logbook" || hidden_ == "Logged Projects" ||
         hidden_ == "Archived Areas")) {
      auto &item = list_[pick_];
      if (item.kind == 't')
        s_.erase(item);
      else if (confirm("Delete \"" + item.title + "\" permanently?"))
        s_.erase(item);
      return;
    }
    if (k == 'e' && !list_.empty()) {
      if (list_[pick_].kind == 't')
        taskForm(list_[pick_]);
      else if (list_[pick_].kind == 'p')
        projectForm(list_[pick_]);
      else if (list_[pick_].kind == 'a')
        areaForm(list_[pick_]);
      return;
    }
    if ((k == '\n' || k == KEY_ENTER) && !list_.empty() &&
        list_[pick_].kind == 'p') {
      openContainer('p', list_[pick_].id, list_[pick_].title,
                    list_[pick_].areaName);
      return;
    }
    if (k == '?') {
      help();
      return;
    }
  }
};

int main() {
  try {
    const char *data = std::getenv("STRIDE_DATA_DIR");
    const char *home = std::getenv("HOME");
    auto dir =
        data ? std::filesystem::path(data)
             : std::filesystem::path(home ? home : ".") / ".local/share/stride";
    std::filesystem::create_directories(dir);
    Store store((dir / "stride.db").string());
    App(store).run();
  } catch (const std::exception &e) {
    std::cerr << "stride: " << e.what() << '\n';
    return 1;
  }
}
