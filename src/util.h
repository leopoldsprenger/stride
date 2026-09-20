#pragma once
// Data model + small pure-function helpers with no ncurses/sqlite dependency,
// so they're easy to reuse from every other file.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

struct Item {
  int id = 0;
  char kind = 't';  // 't' task, 'p' project, 'a' area, 'h' heading
  std::string title;
  std::string notes;      // task notes, or project description (unrestricted length)
  std::string checklist;  // task-only; lines of "0|text" / "1|text", never shown in list views
  int areaId = 0;
  std::string areaName;
  int projectId = 0;
  std::string projectName;
  int headingId = 0;
  std::string doDate;
  std::string deadline;
  std::string tags;
  bool someday = false;
  std::string status = "open";  // open/done/completed/cancelled
  std::string completedAt;
  int sortOrder = 0;
  std::string section;  // UI-only grouping hint (area view sections); not persisted
};

struct Ref {
  int id = 0;
  std::string name;
  std::string sub;  // secondary context, e.g. a project's area name
};

struct ChecklistItem {
  bool done = false;
  std::string text;
};

inline const char* tableFor(char kind) {
  switch (kind) {
    case 'p': return "projects";
    case 'a': return "areas";
    case 'h': return "headings";
    default: return "tasks";
  }
}

inline std::string today() {
  std::time_t t = std::time(nullptr);
  char buf[11]{};
  std::strftime(buf, sizeof(buf), "%F", std::localtime(&t));
  return buf;
}

inline std::string todayPlus(int days) {
  std::time_t t = std::time(nullptr) + days * 86400;
  char buf[11]{};
  std::strftime(buf, sizeof(buf), "%F", std::localtime(&t));
  return buf;
}

// Clip a string to fit `cols` terminal columns so we never write past a
// window edge (ncurses doesn't wrap or clip mvprintw for us). Counts UTF-8
// codepoints rather than bytes, since every symbol this app uses (icons,
// punctuation) occupies exactly one terminal column.
inline std::string clip(const std::string& s, int cols) {
  if (cols <= 0) return "";
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size();) {
    starts.push_back(i);
    unsigned char c = s[i];
    size_t len = (c < 0x80) ? 1 : ((c >> 5) == 0x6) ? 2 : ((c >> 4) == 0xE) ? 3 : ((c >> 3) == 0x1E) ? 4 : 1;
    i += len;
  }
  if ((int)starts.size() <= cols) return s;
  if (cols == 1) return s.substr(starts[0], (starts.size() > 1 ? starts[1] : s.size()) - starts[0]);
  return s.substr(0, starts[cols - 1]) + "\xe2\x80\xa6";  // UTF-8 ellipsis
}

// Number of UTF-8 codepoints (== terminal columns for the text we render).
inline int displayWidth(const std::string& s) {
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

inline std::vector<std::string> splitComma(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0, n;
  while ((n = s.find(',', p)) != std::string::npos) {
    if (n > p) out.push_back(s.substr(p, n - p));
    p = n + 1;
  }
  if (p < s.size()) out.push_back(s.substr(p));
  return out;
}

// Keep only characters we ever legitimately need in a tag, so tag filters
// can't be used to smuggle SQL into the query we build by hand in Store.
inline std::string sanitizeTag(std::string s) {
  std::string out;
  for (char c : s)
    if (isalnum((unsigned char)c) || c == '-' || c == '_') out += (char)tolower((unsigned char)c);
  return out;
}

inline std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(' ');
  return s.substr(a, b - a + 1);
}

inline std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

inline std::vector<ChecklistItem> parseChecklist(const std::string& raw) {
  std::vector<ChecklistItem> out;
  size_t p = 0;
  while (p < raw.size()) {
    size_t nl = raw.find('\n', p);
    std::string line = raw.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
    if (line.size() >= 2 && line[1] == '|') out.push_back({line[0] == '1', line.substr(2)});
    p = nl == std::string::npos ? raw.size() : nl + 1;
  }
  return out;
}

inline std::string serializeChecklist(const std::vector<ChecklistItem>& items) {
  std::string out;
  for (auto& c : items) {
    out += (c.done ? '1' : '0');
    out += '|';
    out += c.text;
    out += '\n';
  }
  return out;
}

// Word-wraps `text` (which may already contain '\n' line breaks) to fit
// `width` columns, used for both editing and read-only display of
// descriptions that shouldn't be cut off.
inline std::vector<std::string> wrapText(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width < 1) width = 1;
  size_t start = 0;
  while (true) {
    size_t nl = text.find('\n', start);
    std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (para.empty()) {
      lines.push_back("");
    } else {
      size_t i = 0;
      while (i < para.size()) {
        size_t remaining = para.size() - i;
        if ((int)remaining <= width) {
          lines.push_back(para.substr(i));
          break;
        }
        size_t brk = para.rfind(' ', i + width);
        size_t cut = (brk == std::string::npos || brk < i) ? i + width : brk;
        lines.push_back(para.substr(i, cut - i));
        i = cut;
        while (i < para.size() && para[i] == ' ') ++i;
      }
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  if (lines.empty()) lines.push_back("");
  return lines;
}

// OS-aware data directory: honours STRIDE_DATA_DIR and XDG_DATA_HOME first,
// otherwise follows each platform's own convention.
inline std::filesystem::path dataDir() {
  if (const char* d = std::getenv("STRIDE_DATA_DIR")) return d;
  const char* home = std::getenv("HOME");
  std::filesystem::path base = home ? home : ".";
#if defined(__APPLE__)
  return base / "Library/Application Support/stride";
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) return std::filesystem::path(xdg) / "stride";
  return base / ".local/share/stride";
#endif
}

// Random UUID v4 (hex, RFC-4122 variant bits set). Used as the stable,
// cross-device identity for every area/project/task/heading -- local
// autoincrement ids only mean something on the machine that assigned them,
// but two Stride installs need to recognize "this is the same item" when
// reconciling the git mirror. Not cryptographically sensitive, just needs
// to not collide in practice.
inline std::string genUuid() {
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<int> hexd(0, 15);
  const char* hex = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      s += '-';
    } else if (i == 14) {
      s += '4';
    } else if (i == 19) {
      s += hex[8 + (hexd(gen) & 3)];  // variant bits 10xx
    } else {
      s += hex[hexd(gen)];
    }
  }
  return s;
}

// Lowercase, filesystem- and git-diff-friendly slug: keeps alphanumerics,
// collapses everything else to single hyphens, trims them from the ends.
// Used only for mirror file names, never for identity (ids own that).
inline std::string slugify(const std::string& s) {
  std::string out;
  bool lastDash = false;
  for (unsigned char c : s) {
    if (isalnum(c)) {
      out += (char)tolower(c);
      lastDash = false;
    } else if (!lastDash && !out.empty()) {
      out += '-';
      lastDash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.empty()) out = "untitled";
  return out;
}
