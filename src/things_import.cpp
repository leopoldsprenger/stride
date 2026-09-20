#include "things_import.h"

#include <sqlite3.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <unistd.h>

#include "store.h"
#include "util.h"

namespace {
namespace fs = std::filesystem;

// Things (like Core Data generally) stores dates as seconds since
// 2001-01-01 00:00:00 UTC. 978307200 is that moment's Unix timestamp.
constexpr time_t kThingsEpochOffset = 978307200;

std::string decodeDate(sqlite3_stmt* s, int col) {
  if (sqlite3_column_type(s, col) == SQLITE_NULL) return "";
  time_t t = (time_t)sqlite3_column_int64(s, col) + kThingsEpochOffset;
  char buf[11]{};
  std::strftime(buf, sizeof(buf), "%F", std::gmtime(&t));
  return buf;
}
std::string txt(sqlite3_stmt* s, int col) {
  auto* p = (const char*)sqlite3_column_text(s, col);
  return p ? p : "";
}

struct SourceDb {
  sqlite3* db = nullptr;
  ~SourceDb() {
    if (db) sqlite3_close(db);
  }
  sqlite3_stmt* prep(const std::string& q) const {
    sqlite3_stmt* s{};
    if (sqlite3_prepare_v2(db, q.c_str(), -1, &s, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("bad query against Things db: ") + sqlite3_errmsg(db));
    return s;
  }
};

std::vector<std::string> tagsFor(const SourceDb& src, const std::string& taskUuid) {
  std::vector<std::string> out;
  auto* s = src.prep("SELECT t.title FROM TMTaskTag tt JOIN TMTag t ON t.uuid=tt.tags WHERE tt.tasks=?");
  sqlite3_bind_text(s, 1, taskUuid.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(s) == SQLITE_ROW) out.push_back(txt(s, 0));
  sqlite3_finalize(s);
  return out;
}

std::vector<ChecklistItem> checklistFor(const SourceDb& src, const std::string& taskUuid) {
  std::vector<ChecklistItem> out;
  auto* s = src.prep("SELECT title,status FROM TMChecklistItem WHERE task=? ORDER BY \"index\"");
  sqlite3_bind_text(s, 1, taskUuid.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(s) == SQLITE_ROW) out.push_back({sqlite3_column_int(s, 1) != 0, txt(s, 0)});
  sqlite3_finalize(s);
  return out;
}

// Stride's areas.name and projects.name columns are UNIQUE, but Things
// allows duplicate titles (this file has nine projects all named "Weekly
// Review"). `Store::step()` silently swallows the resulting constraint
// violation rather than throwing, so a naive import would drop every
// duplicate past the first -- and worse, `last_insert_rowid()` would still
// return the *previous* successful insert, mis-tagging its things_uuid.
// `registry` maps title -> the local id currently holding it (seeded from
// what's already in Stride); a collision with a *different* id gets
// " (2)", " (3)"... suffixed until free. A row reclaiming its own
// previously-disambiguated title (on a re-import) matches itself and is
// left alone, so this is stable across repeated runs.
std::string dedupeTitle(std::map<std::string, int>& registry, const std::string& desired, int ownId) {
  std::string candidate = desired;
  int n = 2;
  while (registry.count(candidate) && registry[candidate] != ownId) candidate = desired + " (" + std::to_string(n++) + ")";
  registry[candidate] = ownId;
  return candidate;
}

// A project/task/area's Things status (0 open, 2 cancelled, 3 completed)
// applied to whatever Stride already stored for it.
void applyStatus(Store& store, char kind, int id, int thingsStatus) {
  Item i;
  i.id = id;
  i.kind = kind;
  if (thingsStatus == 3) store.complete(i);
  else if (thingsStatus == 2) store.cancel(i);
  else store.reopen(i);
}

}  // namespace

ImportStats importThings(Store& store, const std::string& sqlitePath) {
  if (!fs::exists(sqlitePath)) throw std::runtime_error("no such file: " + sqlitePath);

  // Copy the db (and any sibling -wal/-shm, where Things keeps very recent
  // changes until its next checkpoint) to a scratch dir so we can open it
  // read-write -- needed for SQLite to replay the WAL -- without ever
  // touching the original export.
  auto tmp = fs::temp_directory_path() / ("stride-things-import-" + std::to_string(::getpid()));
  fs::create_directories(tmp);
  auto tmpMain = tmp / "main.sqlite";
  std::error_code ec;
  fs::copy_file(sqlitePath, tmpMain, fs::copy_options::overwrite_existing, ec);
  for (auto suffix : {"-wal", "-shm"}) {
    auto sibling = fs::path(sqlitePath + suffix);
    if (fs::exists(sibling)) fs::copy_file(sibling, fs::path(tmpMain.string() + suffix), fs::copy_options::overwrite_existing, ec);
  }

  SourceDb src;
  auto cleanup = [&] { fs::remove_all(tmp, ec); };
  if (sqlite3_open_v2(tmpMain.string().c_str(), &src.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    cleanup();
    throw std::runtime_error("could not open Things database");
  }
  // Touching any table forces SQLite to replay the WAL into the main file,
  // so everything below sees fully merged, consistent data.
  if (sqlite3_exec(src.db, "SELECT count(*) FROM TMTask", nullptr, nullptr, nullptr) != SQLITE_OK) {
    cleanup();
    throw std::runtime_error("doesn't look like a Things database (no TMTask table)");
  }

  ImportStats stats;
  std::map<std::string, int> areaMap, projectMap, headingMap;
  std::map<std::string, int> headingProjectId;  // heading uuid -> its project's local id (tasks under
                                                 // a heading have NULL project/area in Things -- only
                                                 // the heading itself carries the project link)
  int placeholderSeq = 0;  // distinct negative "not yet inserted" id per row, so two brand-new
                            // same-named rows aren't mistaken for the same owner by dedupeTitle

  // Seed the dedup registries with everything already in Stride (prior
  // manual entries or an earlier import run), keyed by name -> id.
  std::map<std::string, int> areaTitles, projectTitles;
  for (bool archived : {false, true})
    for (auto& r : store.areas(archived)) areaTitles[r.name] = r.id;
  for (bool archived : {false, true})
    for (auto& r : store.projects(archived)) projectTitles[r.name] = r.id;

  // -- areas --------------------------------------------------------------
  {
    auto* s = src.prep("SELECT uuid,title FROM TMArea ORDER BY \"index\"");
    while (sqlite3_step(s) == SQLITE_ROW) {
      std::string uuid = txt(s, 0), title = txt(s, 1);
      try {
        int id = store.findByThingsUuid('a', uuid);
        title = dedupeTitle(areaTitles, title, id ? id : --placeholderSeq);
        if (id) store.renameArea(id, title);
        else id = store.addArea(title);
        store.setThingsUuid('a', id, uuid);
        areaMap[uuid] = id;
        stats.areas++;
      } catch (const std::exception& e) {
        stats.warnings.push_back("area \"" + title + "\": " + e.what());
      }
    }
    sqlite3_finalize(s);
  }

  // -- projects (TMTask type=1) --------------------------------------------
  {
    auto* s = src.prep(
        "SELECT uuid,title,notes,status,area,startDate,deadline,start FROM TMTask "
        "WHERE type=1 AND trashed=0 ORDER BY \"index\"");
    while (sqlite3_step(s) == SQLITE_ROW) {
      std::string uuid = txt(s, 0), title = txt(s, 1);
      try {
        Item it;
        it.id = store.findByThingsUuid('p', uuid);
        it.title = dedupeTitle(projectTitles, title, it.id ? it.id : --placeholderSeq);
        it.notes = txt(s, 2);
        int status = sqlite3_column_int(s, 3);
        std::string areaUuid = txt(s, 4);
        bool someday = sqlite3_column_int(s, 7) == 2;
        it.doDate = someday ? "" : decodeDate(s, 5);  // saveProject has no someday concept for projects
        it.deadline = decodeDate(s, 6);
        int areaId = areaUuid.empty() ? 0 : areaMap.count(areaUuid) ? areaMap[areaUuid] : 0;
        int id = store.saveProject(it, areaId);
        applyStatus(store, 'p', id, status);
        store.setThingsUuid('p', id, uuid);
        projectMap[uuid] = id;
        stats.projects++;
      } catch (const std::exception& e) {
        stats.warnings.push_back("project \"" + title + "\": " + e.what());
      }
    }
    sqlite3_finalize(s);
  }

  // -- headings (TMTask type=2) ---------------------------------------------
  {
    auto* s = src.prep("SELECT uuid,title,project FROM TMTask WHERE type=2 AND trashed=0 ORDER BY \"index\"");
    while (sqlite3_step(s) == SQLITE_ROW) {
      std::string uuid = txt(s, 0), title = txt(s, 1), projectUuid = txt(s, 2);
      if (!projectMap.count(projectUuid)) {
        stats.warnings.push_back("heading \"" + title + "\": its project wasn't imported, skipped");
        continue;
      }
      try {
        int id = store.findByThingsUuid('h', uuid);
        if (id) store.renameHeading(id, title);
        else id = store.addHeading(projectMap[projectUuid], title);
        store.setThingsUuid('h', id, uuid);
        headingMap[uuid] = id;
        headingProjectId[uuid] = projectMap[projectUuid];
        stats.headings++;
      } catch (const std::exception& e) {
        stats.warnings.push_back("heading \"" + title + "\": " + e.what());
      }
    }
    sqlite3_finalize(s);
  }

  // -- tasks (TMTask type=0) ------------------------------------------------
  {
    auto* s = src.prep(
        "SELECT uuid,title,notes,status,area,project,heading,start,startDate,deadline FROM TMTask "
        "WHERE type=0 AND trashed=0 ORDER BY \"index\"");
    while (sqlite3_step(s) == SQLITE_ROW) {
      std::string uuid = txt(s, 0), title = txt(s, 1);
      try {
        std::string areaUuid = txt(s, 4), projectUuid = txt(s, 5), headingUuid = txt(s, 6);
        int status = sqlite3_column_int(s, 3);
        bool someday = sqlite3_column_int(s, 7) == 2;

        Item it;
        it.id = store.findByThingsUuid('t', uuid);
        it.title = title;
        it.notes = txt(s, 2);
        it.doDate = someday ? "someday" : decodeDate(s, 8);
        it.deadline = decodeDate(s, 9);
        auto tags = tagsFor(src, uuid);
        for (size_t i = 0; i < tags.size(); ++i) {
          if (i) it.tags += ",";
          it.tags += tags[i];
        }
        it.checklist = serializeChecklist(checklistFor(src, uuid));

        int projectId = projectUuid.empty() ? 0 : projectMap.count(projectUuid) ? projectMap[projectUuid] : 0;
        if (!projectId && !headingUuid.empty() && headingProjectId.count(headingUuid)) projectId = headingProjectId[headingUuid];
        int areaId = 0;
        if (projectId) areaId = store.projectAreaId(projectId);
        else if (!areaUuid.empty() && areaMap.count(areaUuid)) areaId = areaMap[areaUuid];

        int id = store.saveTask(it, areaId, projectId);
        if (!headingUuid.empty() && headingMap.count(headingUuid)) store.setTaskHeading(id, headingMap[headingUuid]);
        applyStatus(store, 't', id, status);
        store.setThingsUuid('t', id, uuid);
        stats.tasks++;
      } catch (const std::exception& e) {
        stats.warnings.push_back("task \"" + title + "\": " + e.what());
      }
    }
    sqlite3_finalize(s);
  }

  cleanup();
  return stats;
}
