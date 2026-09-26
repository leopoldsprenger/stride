#include "mirror.h"

#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <algorithm>
#include <sstream>

#include "store.h"
#include "util.h"

namespace {

namespace fs = std::filesystem;

// Minimal YAML scalar quoting: always double-quote strings so a colon,
// hash, or leading/trailing space in a title can never be misread as YAML
// syntax by anything that parses this mirror later.
std::string yq(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

void writeFile(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << content;
}

// Filenames are keyed by stride_uuid (short prefix), not the local
// autoincrement id: the id is only meaningful on the device that assigned
// it, so using it here would make two devices' otherwise-identical bundles
// hash differently after every reconcile (a reconciled item gets a new,
// unrelated local id on the receiving device), producing a spurious commit
// on every single multi-device sync. The uuid is the same everywhere.
std::string slugName(const std::string& uuid, const std::string& title) {
  std::ostringstream o;
  o << (uuid.empty() ? "00000000" : uuid.substr(0, 8)) << '-' << slugify(title);
  return o.str();
}

std::string uuidOf(const std::map<int, std::string>& uuids, int id) {
  auto it = uuids.find(id);
  return it == uuids.end() ? "" : it->second;
}

// One `- [ ] Title  `do:…` `deadline:…` #tag <!-- uuid:… -->` line, plus
// indented checklist sub-items and `> `-prefixed notes. `indent` lets
// project-file tasks nest one level under a heading. The trailing HTML
// comment is invisible in any Markdown renderer but is exactly what
// mirror_import.cpp matches on to recognize "this is the same task" when
// reconciling a pulled mirror back into another device's database --
// without it, a re-synced task would look like a brand new one every time.
std::string renderTask(const Item& t, const std::string& indent, const std::string& uuid) {
  std::ostringstream o;
  bool done = t.status == "done";
  bool cancelled = t.status == "cancelled";
  o << indent << "- [" << (done ? 'x' : (cancelled ? '~' : ' ')) << "] " << t.title;
  if (t.someday) o << "  `someday`";
  if (!t.doDate.empty()) o << "  `do:" << t.doDate << "`";
  if (!t.deadline.empty()) o << "  `deadline:" << t.deadline << "`";
  if (!t.completedAt.empty()) o << "  `" << (cancelled ? "cancelled" : "completed") << ":" << t.completedAt << "`";
  if (!t.tags.empty()) {
    for (auto& tag : splitComma(t.tags)) {
      auto v = trimmed(tag);
      if (!v.empty()) o << " #" << v;
    }
  }
  if (!uuid.empty()) o << "  <!-- uuid:" << uuid << " -->";
  o << '\n';
  if (!t.notes.empty()) {
    for (auto& line : wrapText(t.notes, 100)) o << indent << "    > " << line << '\n';
  }
  for (auto& c : parseChecklist(t.checklist)) {
    o << indent << "  - [" << (c.done ? 'x' : ' ') << "] " << c.text << '\n';
  }
  return o.str();
}

// Groups already-filtered tasks into Open/Someday/Completed/Cancelled
// sections, only emitting the sections that actually have items.
std::string renderTaskSections(const std::vector<Item>& tasks, const std::string& indent,
                                const std::map<int, std::string>& taskUuids) {
  std::ostringstream o;
  auto section = [&](const char* title, const std::function<bool(const Item&)>& pred) {
    std::vector<const Item*> matched;
    for (auto& t : tasks)
      if (pred(t)) matched.push_back(&t);
    if (matched.empty()) return;
    // Sorted by (do date, title, uuid) rather than the incoming sort_order:
    // sort_order is a per-device counter (each device assigns its own when
    // a task is first created there, including via reconcile), so it never
    // agrees between two devices for the same task set -- rendering in
    // that order would make every device's bundle hash differently forever
    // even when the actual data converged, causing an endless ping-pong of
    // settling commits between devices. This key is built entirely from
    // synced fields, so it's identical everywhere.
    std::sort(matched.begin(), matched.end(), [&](const Item* a, const Item* b) {
      auto key = [&](const Item* t) {
        return std::make_tuple(t->doDate.empty() ? std::string("9999-99-99") : t->doDate, t->title, uuidOf(taskUuids, t->id));
      };
      return key(a) < key(b);
    });
    o << indent << "### " << title << '\n';
    for (auto* t : matched) o << renderTask(*t, indent, uuidOf(taskUuids, t->id));
    o << '\n';
  };
  section("Open", [](const Item& t) { return t.status == "open" && !t.someday; });
  section("Someday", [](const Item& t) { return t.status == "open" && t.someday; });
  section("Completed", [](const Item& t) { return t.status == "done"; });
  section("Cancelled", [](const Item& t) { return t.status == "cancelled"; });
  return o.str();
}

}  // namespace

void MirrorExporter::exportTo(const fs::path& mirrorDir) {
  fs::create_directories(mirrorDir);
  writeFile(mirrorDir / kMarkerFile,
            "This file marks the directory as a Stride mirror. Do not remove it -- Stride checks\n"
            "for it before pushing to a remote, to avoid overwriting an unrelated repository.\n");

  auto contentDir = mirrorDir / "content";
  std::error_code ec;
  fs::remove_all(contentDir, ec);  // fully regenerated below; a derived view, never hand-edited
  fs::create_directories(contentDir);

  auto areas = store_.allAreas();
  auto projects = store_.allProjects();
  auto tasks = store_.allTasks();
  auto headings = store_.allHeadings();
  auto areaUuids = store_.strideUuids('a');
  auto projectUuids = store_.strideUuids('p');
  auto taskUuids = store_.strideUuids('t');
  auto headingUuids = store_.strideUuids('h');

  std::map<int, std::vector<Item>> tasksByArea, tasksByProject;
  std::vector<Item> inboxTasks;
  for (auto& t : tasks) {
    if (t.projectId) tasksByProject[t.projectId].push_back(t);
    else if (t.areaId) tasksByArea[t.areaId].push_back(t);
    else inboxTasks.push_back(t);
  }
  std::map<int, std::vector<Item>> headingsByProject;
  for (auto& h : headings) headingsByProject[h.projectId].push_back(h);
  std::map<int, std::vector<Item>> projectsByArea;
  for (auto& p : projects) projectsByArea[p.areaId].push_back(p);
  // Same reasoning as the task sort in renderTaskSections: sort_order is
  // per-device, so ordering these by it would make two devices' bundles
  // diverge forever even once their actual data agreed. (title, uuid) is
  // built from synced fields, so it's identical on every device.
  auto byTitleThenUuid = [&](const std::map<int, std::string>& uuids) {
    return [&](const Item& a, const Item& b) {
      return std::make_pair(a.title, uuidOf(uuids, a.id)) < std::make_pair(b.title, uuidOf(uuids, b.id));
    };
  };
  for (auto& [id, hs] : headingsByProject) std::sort(hs.begin(), hs.end(), byTitleThenUuid(headingUuids));
  for (auto& [id, ps] : projectsByArea) std::sort(ps.begin(), ps.end(), byTitleThenUuid(projectUuids));

  // -- inbox.md --
  {
    std::ostringstream o;
    o << "---\ntitle: \"Inbox\"\n---\n\n# Inbox\n\n" << renderTaskSections(inboxTasks, "", taskUuids);
    writeFile(contentDir / "inbox.md", o.str());
  }

  // -- one file per area --
  for (auto& a : areas) {
    std::ostringstream o;
    o << "---\nuuid: " << uuidOf(areaUuids, a.id) << "\ntitle: " << yq(a.title)
      << "\nstatus: " << a.status;
    if (!a.completedAt.empty()) o << "\ncompleted_at: " << a.completedAt;
    o << "\n---\n\n# " << a.title << "\n\n";
    auto& ps = projectsByArea[a.id];
    if (!ps.empty()) {
      o << "## Projects\n\n";
      for (auto& p : ps) {
        o << "- [" << (p.status == "open" ? "open" : p.status) << "] [" << p.title << "](../projects/"
          << slugName(uuidOf(projectUuids, p.id), p.title) << ".md)\n";
      }
      o << '\n';
    }
    o << renderTaskSections(tasksByArea[a.id], "", taskUuids);
    writeFile(contentDir / "areas" / (slugName(uuidOf(areaUuids, a.id), a.title) + ".md"), o.str());
  }

  // -- one file per project --
  for (auto& p : projects) {
    std::ostringstream o;
    o << "---\nuuid: " << uuidOf(projectUuids, p.id) << "\ntitle: " << yq(p.title)
      << "\nstatus: " << p.status;
    if (p.areaId) o << "\narea: " << yq(p.areaName);
    if (!p.doDate.empty()) o << "\ndo_date: " << p.doDate;
    if (!p.deadline.empty()) o << "\ndeadline: " << p.deadline;
    if (!p.completedAt.empty()) o << "\ncompleted_at: " << p.completedAt;
    o << "\n---\n\n# " << p.title << "\n\n";
    if (!p.notes.empty()) {
      for (auto& line : wrapText(p.notes, 100)) o << "> " << line << '\n';
      o << '\n';
    }
    auto& pTasks = tasksByProject[p.id];
    std::vector<Item> noHeading;
    for (auto& t : pTasks)
      if (t.headingId == 0) noHeading.push_back(t);
    o << renderTaskSections(noHeading, "", taskUuids);
    for (auto& h : headingsByProject[p.id]) {
      std::vector<Item> hTasks;
      for (auto& t : pTasks)
        if (t.headingId == h.id) hTasks.push_back(t);
      o << "## " << h.title << "  <!-- uuid:" << uuidOf(headingUuids, h.id) << " -->\n\n"
        << renderTaskSections(hTasks, "", taskUuids);
    }
    writeFile(contentDir / "projects" / (slugName(uuidOf(projectUuids, p.id), p.title) + ".md"), o.str());
  }

  // -- top-level index --
  {
    std::ostringstream o;
    o << "# Stride mirror\n\n"
         "Generated by Stride, and read back on sync to bring in changes made on other devices -- "
         "but only from what's actually committed on the remote. Local edits made directly to these "
         "files are discarded (not merged) the next time Stride syncs, so treat this as a view, not "
         "an editor: change things in Stride itself.\n\n"
      << "- " << areas.size() << " area(s)\n"
      << "- " << projects.size() << " project(s)\n"
      << "- " << tasks.size() << " task(s)\n\n"
      << "See `content/` -- `inbox.md`, `areas/`, `projects/`.\n";
    writeFile(mirrorDir / "README.md", o.str());
  }
}
