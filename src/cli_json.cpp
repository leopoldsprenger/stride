#include "cli_json.h"

#include <cctype>
#include <iostream>

#include "util.h"

namespace {

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

std::string jsonStr(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string jsonTags(const std::string& tags) {
  std::string out = "[";
  auto parts = splitComma(tags);
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += ",";
    out += jsonStr(trimmed(parts[i]));
  }
  return out + "]";
}

// One task/project row, in the shape every `--json` view shares. `kind` is
// 't' or 'p' (folded project rows show up in Today/Upcoming just like they
// do in the TUI); a caller passes id+kind straight back to --complete.
std::string jsonItem(const Item& x) {
  std::string o = "{";
  o += "\"id\":" + std::to_string(x.id) + ",";
  o += "\"kind\":" + jsonStr(std::string(1, x.kind)) + ",";
  o += "\"title\":" + jsonStr(x.title) + ",";
  o += "\"doDate\":" + jsonStr(x.doDate) + ",";
  o += "\"deadline\":" + jsonStr(x.deadline) + ",";
  o += "\"someday\":" + std::string(x.someday ? "true" : "false") + ",";
  o += "\"status\":" + jsonStr(x.status) + ",";
  o += "\"completedAt\":" + jsonStr(x.completedAt) + ",";
  o += "\"areaName\":" + jsonStr(x.areaName) + ",";
  o += "\"projectName\":" + jsonStr(x.projectName) + ",";
  o += "\"tags\":" + jsonTags(x.tags) + ",";
  o += "\"hasNotes\":" + std::string(x.notes.empty() ? "false" : "true") + ",";
  o += "\"hasChecklist\":" + std::string(x.checklist.empty() ? "false" : "true");
  return o + "}";
}

std::string jsonItems(const std::vector<Item>& xs) {
  std::string o = "[";
  for (size_t i = 0; i < xs.size(); ++i) {
    if (i) o += ",";
    o += jsonItem(xs[i]);
  }
  return o + "]";
}

std::string jsonProjectRef(const Ref& p) {
  std::string o = "{";
  o += "\"id\":" + std::to_string(p.id) + ",";
  o += "\"name\":" + jsonStr(p.name) + ",";
  o += "\"areaName\":" + jsonStr(p.sub);
  return o + "}";
}

int findAreaIdByName(Store& s, const std::string& name) {
  std::string n = lower(trimmed(name));
  if (n.empty()) return 0;
  for (auto& a : s.areas())
    if (lower(a.name) == n) return a.id;
  return 0;
}
int findProjectIdByName(Store& s, const std::string& name) {
  std::string n = lower(trimmed(name));
  if (n.empty()) return 0;
  for (auto& p : s.projects())
    if (lower(p.name) == n) return p.id;
  return 0;
}

// Accepts either a numeric project id or a project name (what a widget's
// "click into a project" naturally has on hand either way).
int resolveProjectArg(Store& s, const std::string& arg) {
  std::string t = trimmed(arg);
  if (!t.empty() && t.find_first_not_of("0123456789") == std::string::npos) return std::stoi(t);
  return findProjectIdByName(s, t);
}

}  // namespace

int runJson(Store& s, const std::string& view, const std::string& arg) {
  if (view == "today") {
    std::cout << jsonItems(s.viewDay(false, "")) << '\n';
  } else if (view == "upcoming") {
    std::cout << jsonItems(s.viewUpcoming("")) << '\n';
  } else if (view == "inbox") {
    std::cout << jsonItems(s.viewInbox("")) << '\n';
  } else if (view == "logbook") {
    std::cout << jsonItems(s.viewLogbook()) << '\n';
  } else if (view == "projects") {
    std::string o = "[";
    auto ps = s.projects();
    for (size_t i = 0; i < ps.size(); ++i) {
      if (i) o += ",";
      o += jsonProjectRef(ps[i]);
    }
    std::cout << o << "]\n";
  } else if (view == "project") {
    int id = resolveProjectArg(s, arg);
    if (!id) {
      std::cerr << "stride --json project: no project matches \"" << arg << "\"\n";
      return 1;
    }
    Item p = s.getProject(id);
    std::string o = "{\"project\":" + jsonProjectRef({id, p.title, p.areaName}) + ",";
    o += "\"tasks\":" + jsonItems(s.viewProject(id, "", s.projectIsOpen(id)));
    std::cout << o << "}\n";
  } else {
    std::cerr << "stride --json: unknown view \"" << view << "\" (want: today, upcoming, inbox, logbook, projects, "
              << "project)\n";
    return 1;
  }
  return 0;
}

int runComplete(Store& s, int id, char kind) {
  if (kind != 't' && kind != 'p') {
    std::cerr << "stride --complete: kind must be t or p\n";
    return 1;
  }
  Item i;
  i.id = id;
  i.kind = kind;
  s.complete(i);
  return 0;
}

int runQuickAdd(Store& s, const std::string& title, const std::string& list, const std::string& date) {
  if (trimmed(title).empty()) {
    std::cerr << "stride --quick-add: title cannot be blank\n";
    return 1;
  }
  Item t;
  t.title = title;
  t.doDate = lower(trimmed(date)) == "today" ? today() : date;
  int projectId = findProjectIdByName(s, list);
  int areaId = projectId ? s.projectAreaId(projectId) : findAreaIdByName(s, list);
  int id = s.saveTask(t, areaId, projectId);
  std::cout << "{\"id\":" << id << "}\n";
  return 0;
}
