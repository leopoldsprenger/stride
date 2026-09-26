#include "mirror_import.h"

#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#include "store.h"
#include "util.h"

namespace {
namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

// -- frontmatter -----------------------------------------------------------

struct Frontmatter {
  std::map<std::string, std::string> kv;
  std::string body;
};

std::string unquote(std::string v) {
  if (v.size() < 2 || v.front() != '"' || v.back() != '"') return v;
  std::string out;
  for (size_t i = 1; i + 1 < v.size(); ++i) {
    if (v[i] == '\\' && i + 2 < v.size()) out += v[++i];
    else out += v[i];
  }
  return out;
}

Frontmatter parseFrontmatter(const std::string& text) {
  Frontmatter fm;
  if (text.rfind("---", 0) != 0) {
    fm.body = text;
    return fm;
  }
  std::istringstream in(text);
  std::string line;
  std::getline(in, line);  // opening ---
  while (std::getline(in, line)) {
    if (line == "---") break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    fm.kv[trimmed(line.substr(0, colon))] = unquote(trimmed(line.substr(colon + 1)));
  }
  std::ostringstream rest;
  rest << in.rdbuf();
  fm.body = rest.str();
  return fm;
}

// -- task lines --------------------------------------------------------------

struct ParsedTask {
  std::string uuid, title, status = "open", doDate, deadline, notes, completedAt;
  bool someday = false;
  std::vector<std::string> tags;
  std::vector<ChecklistItem> checklist;
};

struct ParsedBlock {  // one heading's worth of tasks, or (if uuid/title empty) the top-level bucket
  std::string uuid, title;
  std::vector<ParsedTask> tasks;
};

ParsedTask parseTaskLine(char checkbox, std::string rest) {
  ParsedTask t;
  t.status = checkbox == 'x' ? "done" : checkbox == '~' ? "cancelled" : "open";

  static const std::regex uuidRe(R"(\s*<!--\s*uuid:([0-9a-fA-F-]+)\s*-->)");
  std::smatch m;
  if (std::regex_search(rest, m, uuidRe)) {
    t.uuid = m[1].str();
    rest = m.prefix().str() + m.suffix().str();
  }

  static const std::regex tokenRe(R"(\s*`([a-zA-Z]+)(?::([^`]*))?`)");
  auto begin = std::sregex_iterator(rest.begin(), rest.end(), tokenRe);
  std::string cleaned;
  size_t last = 0;
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    cleaned += rest.substr(last, it->position() - last);
    last = it->position() + it->length();
    std::string key = (*it)[1].str(), val = (*it)[2].str();
    if (key == "someday") t.someday = true;
    else if (key == "do") t.doDate = val;
    else if (key == "deadline") t.deadline = val;
    // Preserve the original completion time across devices: without this,
    // every device that reconciles an already-completed task would
    // re-stamp completed_at to its own "now", so no two devices' renders
    // of the same task would ever agree -- an endless ping-pong of
    // settling commits between them, never actually settling.
    else if (key == "completed" || key == "cancelled") t.completedAt = val;
  }
  cleaned += rest.substr(last);
  rest = cleaned;

  static const std::regex tagRe(R"(#([A-Za-z0-9_][A-Za-z0-9_-]*))");
  begin = std::sregex_iterator(rest.begin(), rest.end(), tagRe);
  cleaned.clear();
  last = 0;
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    cleaned += rest.substr(last, it->position() - last);
    last = it->position() + it->length();
    t.tags.push_back((*it)[1].str());
  }
  cleaned += rest.substr(last);

  t.title = trimmed(cleaned);
  return t;
}

// One pass over a file's body (post-frontmatter), splitting on `## Heading`
// lines and accumulating each task's indented `> notes` and `- [ ]`
// checklist sub-lines. Mirrors renderTask/renderTaskSections in mirror.cpp
// exactly -- this file only ever has to parse its own sibling's output.
std::vector<ParsedBlock> parseBody(const std::string& body) {
  std::vector<ParsedBlock> blocks{{"", "", {}}};
  ParsedTask* current = nullptr;
  int taskIndent = -1;

  static const std::regex headingRe(R"(^##\s+(.*?)(?:\s*<!--\s*uuid:([0-9a-fA-F-]+)\s*-->)?\s*$)");
  static const std::regex taskRe(R"(^(\s*)-\s\[([ x~])\]\s(.*)$)");
  static const std::regex noteRe(R"(^(\s*)>\s?(.*)$)");

  std::istringstream in(body);
  std::string line;
  while (std::getline(in, line)) {
    std::smatch m;
    if (line.rfind("##", 0) == 0 && std::regex_match(line, m, headingRe)) {
      blocks.push_back({m[2].matched ? m[2].str() : "", trimmed(m[1].str()), {}});
      current = nullptr;
      taskIndent = -1;
      continue;
    }
    if (std::regex_match(line, m, taskRe)) {
      int indent = (int)m[1].str().size();
      if (current && indent > taskIndent) {
        current->checklist.push_back({m[2].str() == "x", trimmed(m[3].str())});
      } else {
        blocks.back().tasks.push_back(parseTaskLine(m[2].str()[0], m[3].str()));
        current = &blocks.back().tasks.back();
        taskIndent = indent;
      }
      continue;
    }
    if (current && std::regex_match(line, m, noteRe) && (int)m[1].str().size() > taskIndent) {
      if (!current->notes.empty()) current->notes += ' ';
      current->notes += trimmed(m[2].str());
      continue;
    }
    if (trimmed(line).empty()) {
      current = nullptr;
      taskIndent = -1;
    }
  }
  return blocks;
}

void applyStatus(Store& store, char kind, int id, const std::string& status, const std::string& at = "") {
  Item i;
  i.id = id;
  i.kind = kind;
  if (status == "cancelled") store.cancel(i, at);
  else if (status == "open") store.reopen(i);
  else store.complete(i, at);  // "done"/"completed"/anything else archived
}

void applyTask(Store& store, const ParsedTask& pt, int areaId, int projectId, int headingId, ReconcileStats& stats) {
  if (pt.uuid.empty() || pt.title.empty()) return;  // malformed line -- skip rather than guess
  Item it;
  it.id = store.findByStrideUuid('t', pt.uuid);
  it.title = pt.title;
  it.notes = pt.notes;
  for (size_t i = 0; i < pt.tags.size(); ++i) {
    if (i) it.tags += ",";
    it.tags += pt.tags[i];
  }
  it.checklist = serializeChecklist(pt.checklist);
  it.doDate = pt.someday ? "someday" : pt.doDate;
  it.deadline = pt.deadline;
  bool wasNew = it.id == 0;
  int id = store.saveTask(it, areaId, projectId);
  store.setTaskHeading(id, headingId);
  if (wasNew) store.setStrideUuid('t', id, pt.uuid);
  applyStatus(store, 't', id, pt.status, pt.completedAt);
  stats.tasks++;
}

void applyBlocks(Store& store, const std::vector<ParsedBlock>& blocks, int areaId, int projectId,
                  ReconcileStats& stats) {
  for (auto& b : blocks) {
    int headingId = 0;
    if (!b.uuid.empty()) {
      headingId = store.findByStrideUuid('h', b.uuid);
      if (!headingId) {
        headingId = store.addHeading(projectId, b.title);
        store.setStrideUuid('h', headingId, b.uuid);
      } else {
        store.renameHeading(headingId, b.title);
      }
      stats.headings++;
    }
    for (auto& t : b.tasks) applyTask(store, t, areaId, projectId, headingId, stats);
  }
}

}  // namespace

ReconcileStats reconcileFromMirror(Store& store, const fs::path& mirrorDir) {
  ReconcileStats stats;
  auto contentDir = mirrorDir / "content";
  if (!fs::exists(contentDir)) return stats;

  // -- areas first: projects/tasks below need to resolve area names to ids --
  std::map<std::string, int> areaIdByName;
  for (auto& r : store.areas(false)) areaIdByName[r.name] = r.id;
  for (auto& r : store.areas(true)) areaIdByName[r.name] = r.id;

  auto areasDir = contentDir / "areas";
  if (fs::exists(areasDir)) {
    for (auto& entry : fs::directory_iterator(areasDir)) {
      if (entry.path().extension() != ".md") continue;
      auto fm = parseFrontmatter(readFile(entry.path()));
      std::string uuid = fm.kv["uuid"], title = fm.kv["title"], status = fm.kv["status"];
      if (uuid.empty() || title.empty()) continue;
      int id = store.findByStrideUuid('a', uuid);
      if (!id) {
        id = store.addArea(title);
        store.setStrideUuid('a', id, uuid);
      } else {
        store.renameArea(id, title);
      }
      applyStatus(store, 'a', id, status, fm.kv.count("completed_at") ? fm.kv["completed_at"] : "");
      areaIdByName[title] = id;
      stats.areas++;
      applyBlocks(store, parseBody(fm.body), id, 0, stats);  // area's own direct tasks
    }
  }

  // -- projects, then their headings/tasks --
  auto projectsDir = contentDir / "projects";
  if (fs::exists(projectsDir)) {
    for (auto& entry : fs::directory_iterator(projectsDir)) {
      if (entry.path().extension() != ".md") continue;
      auto fm = parseFrontmatter(readFile(entry.path()));
      std::string uuid = fm.kv["uuid"], title = fm.kv["title"], status = fm.kv["status"];
      if (uuid.empty() || title.empty()) continue;
      int areaId = 0;
      auto areaIt = fm.kv.find("area");
      if (areaIt != fm.kv.end() && areaIdByName.count(areaIt->second)) areaId = areaIdByName[areaIt->second];

      Item it;
      it.id = store.findByStrideUuid('p', uuid);
      it.title = title;
      // project body = "\n# Title\n\n> note lines...\n\n### Open\n..." -- pull out the
      // description (consecutive "> " lines right after the title, before the first section).
      std::istringstream in(fm.body);
      std::string line, notes;
      bool pastTitle = false;
      while (std::getline(in, line)) {
        std::string t = trimmed(line);
        if (t.empty()) continue;
        if (!pastTitle) {
          pastTitle = true;
          if (!t.empty() && t[0] == '#') continue;  // the "# Title" line itself
        }
        if (line.rfind("> ", 0) == 0) {
          notes += (notes.empty() ? "" : " ") + trimmed(line.substr(2));
          continue;
        }
        break;  // hit a heading or task line -- description block is over
      }
      it.notes = notes;
      it.doDate = fm.kv.count("do_date") ? fm.kv["do_date"] : "";
      it.deadline = fm.kv.count("deadline") ? fm.kv["deadline"] : "";
      bool wasNew = it.id == 0;
      int id = store.saveProject(it, areaId);
      if (wasNew) store.setStrideUuid('p', id, uuid);
      applyStatus(store, 'p', id, status, fm.kv.count("completed_at") ? fm.kv["completed_at"] : "");
      stats.projects++;

      applyBlocks(store, parseBody(fm.body), areaId, id, stats);
    }
  }

  // -- inbox: tasks with no area/project at all --
  auto inboxFile = contentDir / "inbox.md";
  if (fs::exists(inboxFile)) {
    auto fm = parseFrontmatter(readFile(inboxFile));
    applyBlocks(store, parseBody(fm.body), 0, 0, stats);
  }

  return stats;
}
