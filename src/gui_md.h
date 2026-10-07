#pragma once
// Markdown for notes and descriptions in the GTK4 front end.
//
// Notes are stored as plain markdown (so the TUI, the mirror and `--dump` all keep working), but they are shown and
// edited as a "live preview": the markers (`**`, backticks, `#`, `[..](..)`) are hidden everywhere except on the line
// the cursor is on, and the text between them is styled -- bold, italic, strike-through, `code`, headings, quotes,
// code fences and links. Typing `* ` or `- ` at the start of a line turns into a real bullet that behaves like one in
// a modern editor (Enter continues it, Enter on an empty one ends the list, Tab / Shift+Tab nest it, Backspace turns it
// back into text). URLs, e-mail addresses and phone numbers are links whether or not they are written as markdown, and
// a click opens them.
//
// Bullets are a display detail: the buffer holds "• " but toMarkdown() writes "- " back, so what reaches the
// database is ordinary markdown.
#include <gtk/gtk.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "gui_theme.h"

namespace md {

// Called when a link is clicked. The GUI sets this to something that launches the URL (and copes with failure).
inline std::function<void(const std::string&)> gOpenLink;

// ---- markdown <-> buffer -------------------------------------------------------------------------------------------

inline std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (true) {
    size_t nl = s.find('\n', p);
    out.push_back(s.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
    if (nl == std::string::npos) break;
    p = nl + 1;
  }
  return out;
}
inline std::string joinLines(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? "\n" : "") + v[i];
  return out;
}

inline std::string toBuffer(const std::string& markdown) {
  auto lines = splitLines(markdown);
  for (auto& l : lines) {
    size_t i = l.find_first_not_of(' ');
    if (i == std::string::npos || l.size() < i + 2) continue;
    if ((l[i] == '-' || l[i] == '*' || l[i] == '+') && l[i + 1] == ' ') l = l.substr(0, i) + "\xe2\x80\xa2 " + l.substr(i + 2);
  }
  return joinLines(lines);
}

inline std::string toMarkdown(const std::string& buffer) {
  auto lines = splitLines(buffer);
  for (auto& l : lines) {
    size_t i = l.find_first_not_of(' ');
    if (i == std::string::npos) continue;
    if (l.compare(i, 3, "\xe2\x80\xa2") == 0) {
      std::string rest = l.size() > i + 3 ? l.substr(i + 3) : "";
      if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
      l = l.substr(0, i) + "- " + rest;
    }
  }
  return joinLines(lines);
}

// ---- recognising links in plain text -------------------------------------------------------------------------------

struct Hit {
  size_t a = 0, b = 0;  // byte range in the line
  std::string url;
};

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline std::string digitsOf(const std::string& s) {
  std::string d;
  for (char c : s)
    if (isDigit(c)) d += c;
  return d;
}

// A phone number is deliberately conservative -- dates, versions and IP addresses are not phone numbers.
inline bool looksLikePhone(const std::string& cand) {
  std::string digits = digitsOf(cand);
  if (digits.size() < 7 || digits.size() > 15) return false;
  static const std::regex date1(R"(^\d{4}[-./ ]\d{1,2}[-./ ]\d{1,2}$)"), date2(R"(^\d{1,2}[-./ ]\d{1,2}[-./ ]\d{2,4}$)"),
      ip(R"(^\d{1,3}(\.\d{1,3}){3}$)");
  if (std::regex_match(cand, date1) || std::regex_match(cand, date2) || std::regex_match(cand, ip)) return false;
  bool plus = cand[0] == '+', zero = cand[0] == '0', paren = cand[0] == '(';
  bool sep = cand.find_first_of(" -./()") != std::string::npos;
  if (plus) return digits.size() >= 8;
  if (zero || paren) return digits.size() >= 8;
  if (!sep) return false;
  // formatted like 555 123 4567 / 555-1234: needs a group of three or more digits, and enough of them
  size_t run = 0, best = 0;
  for (char c : cand) {
    run = isDigit(c) ? run + 1 : 0;
    best = std::max(best, run);
  }
  return digits.size() >= 9 && best >= 3;
}

inline std::string telUrl(const std::string& cand) {
  std::string d = digitsOf(cand);
  return std::string("tel:") + (cand[0] == '+' ? "+" : "") + d;
}

inline void findPhones(const std::string& s, const std::vector<char>& mask, std::vector<Hit>& out) {
  size_t n = s.size();
  for (size_t i = 0; i < n;) {
    char c = s[i];
    bool start = (c == '+' || isDigit(c) || c == '(') && !mask[i];
    if (!start || (i > 0 && (isAlnum(s[i - 1]) || s[i - 1] == '@' || s[i - 1] == '/' || s[i - 1] == '_'))) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < n && !mask[j]) {
      char d = s[j];
      if (isDigit(d) || d == '+' || d == '(' || d == ')' || d == '-' || d == '.' || d == '/') ++j;
      else if (d == ' ' && j + 1 < n && (isDigit(s[j + 1]) || s[j + 1] == '(')) ++j;
      else break;
    }
    size_t e = j;
    while (e > i && !(isDigit(s[e - 1]) || s[e - 1] == ')')) --e;
    if (e > i && !(j < n && isAlnum(s[j]) && j == e)) {
      std::string cand = s.substr(i, e - i);
      if (looksLikePhone(cand)) out.push_back({i, e, telUrl(cand)});
    }
    i = std::max(j, i + 1);
  }
}

inline std::string normaliseUrl(std::string u) {
  if (u.rfind("www.", 0) == 0) return "https://" + u;
  return u;
}

inline void findAutoLinks(const std::string& s, const std::vector<char>& mask, std::vector<Hit>& out) {
  static const std::regex url(R"((?:https?://|www\.)[^\s<>"'`]+)", std::regex::icase);
  static const std::regex mail(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*\.[A-Za-z]{2,})");
  auto free = [&](size_t a, size_t b) {
    for (size_t k = a; k < b; ++k)
      if (mask[k]) return false;
    return true;
  };
  for (auto it = std::sregex_iterator(s.begin(), s.end(), url); it != std::sregex_iterator(); ++it) {
    size_t a = it->position(), b = a + it->length();
    while (b > a) {  // trailing punctuation belongs to the sentence, not the link
      char t = s[b - 1];
      if (std::strchr(".,;:!?'\"]}", t)) { --b; continue; }
      if (t == ')') {
        auto seg = s.substr(a, b - a);
        if (std::count(seg.begin(), seg.end(), '(') < std::count(seg.begin(), seg.end(), ')')) { --b; continue; }
      }
      break;
    }
    if (b - a > 4 && free(a, b)) out.push_back({a, b, normaliseUrl(s.substr(a, b - a))});
  }
  for (auto it = std::sregex_iterator(s.begin(), s.end(), mail); it != std::sregex_iterator(); ++it) {
    size_t a = it->position(), b = a + it->length();
    if (free(a, b)) out.push_back({a, b, "mailto:" + s.substr(a, b - a)});
  }
}

// ---- the view ------------------------------------------------------------------------------------------------------

struct Link {
  int start = 0, end = 0;  // character offsets in the buffer
  std::string url;
};

struct View;
inline std::vector<View*>& registry() {
  static std::vector<View*> v;
  return v;
}

struct View {
  GtkWidget* view = nullptr;
  GtkTextBuffer* buf = nullptr;
  bool readOnly = false;
  bool busy = false;
  bool focused = false;  // markers are only shown (on the cursor's line) while the notes have focus
  int lastCursorLine = -2;
  std::vector<Link> links;
  GtkGesture* click = nullptr;

  ~View() {
    if (!view) return;
    auto& r = registry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
  }

  GtkTextTag* tag(const char* n) { return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), n); }

  void paintTags() {  // colours that follow the palette; called on creation and on every theme change
    auto rgba = [](Rgba c, double a = -1) {
      GdkRGBA g{(float)c.r, (float)c.g, (float)c.b, (float)(a < 0 ? c.a : a)};
      return g;
    };
    GdkRGBA codeBg = rgba(gPal.fg, 0.085), codeFg = rgba(gPal.fg), link = rgba(gPal.accent), dim = rgba(gPal.fg3),
            fg2 = rgba(gPal.fg2), bullet = rgba(gPal.accent), blockBg = rgba(gPal.fg, 0.06);
    g_object_set(tag("code"), "background-rgba", &codeBg, "foreground-rgba", &codeFg, nullptr);
    g_object_set(tag("link"), "foreground-rgba", &link, nullptr);
    g_object_set(tag("mark"), "foreground-rgba", &dim, nullptr);
    g_object_set(tag("quote"), "foreground-rgba", &fg2, nullptr);
    g_object_set(tag("bullet"), "foreground-rgba", &bullet, nullptr);
    g_object_set(tag("num"), "foreground-rgba", &fg2, nullptr);
    g_object_set(tag("codeblock"), "paragraph-background-rgba", &blockBg, nullptr);
  }

  void makeTags() {
    GtkTextBuffer* b = buf;
    gtk_text_buffer_create_tag(b, "h1", "scale", 1.38, "weight", 700, "pixels-above-lines", 6, nullptr);
    gtk_text_buffer_create_tag(b, "h2", "scale", 1.2, "weight", 700, "pixels-above-lines", 4, nullptr);
    gtk_text_buffer_create_tag(b, "h3", "scale", 1.06, "weight", 700, "pixels-above-lines", 2, nullptr);
    gtk_text_buffer_create_tag(b, "bold", "weight", 700, nullptr);
    gtk_text_buffer_create_tag(b, "italic", "style", PANGO_STYLE_ITALIC, nullptr);
    gtk_text_buffer_create_tag(b, "strike", "strikethrough", TRUE, nullptr);
    gtk_text_buffer_create_tag(b, "code", "family", "monospace", "scale", 0.92, nullptr);
    gtk_text_buffer_create_tag(b, "codeblock", "family", "monospace", "scale", 0.92, "left-margin", 8, nullptr);
    gtk_text_buffer_create_tag(b, "quote", "left-margin", 14, nullptr);
    gtk_text_buffer_create_tag(b, "bullet", "weight", 700, nullptr);
    gtk_text_buffer_create_tag(b, "num", "weight", 600, nullptr);
    for (int l = 0; l < 6; ++l) {
      std::string n = "ind" + std::to_string(l);
      gtk_text_buffer_create_tag(b, n.c_str(), "left-margin", 22 + 18 * l, "indent", -14, "pixels-below-lines", 2, nullptr);
    }
    gtk_text_buffer_create_tag(b, "link", "underline", PANGO_UNDERLINE_SINGLE, nullptr);
    gtk_text_buffer_create_tag(b, "mark", "weight", 400, nullptr);  // the dim colour is set in paintTags()
    gtk_text_buffer_create_tag(b, "hide", "invisible", TRUE, nullptr);
    paintTags();
  }

  // -- applying tags by (line, byte range) --
  void applyTag(const char* name, int line, size_t a, size_t b) {
    if (b <= a) return;
    GtkTextIter x, y;
    gtk_text_buffer_get_iter_at_line_index(buf, &x, line, (int)a);
    gtk_text_buffer_get_iter_at_line_index(buf, &y, line, (int)b);
    gtk_text_buffer_apply_tag_by_name(buf, name, &x, &y);
  }

  void restyle() {
    if (busy) return;
    busy = true;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    gtk_text_buffer_remove_all_tags(buf, &s, &e);
    char* raw = gtk_text_buffer_get_text(buf, &s, &e, TRUE);
    std::string text = raw ? raw : "";
    g_free(raw);
    links.clear();

    GtkTextIter ci;
    gtk_text_buffer_get_iter_at_mark(buf, &ci, gtk_text_buffer_get_insert(buf));
    int curLine = (readOnly || !focused) ? -1 : gtk_text_iter_get_line(&ci);
    lastCursorLine = curLine;

    auto lines = splitLines(text);
    bool inFence = false;
    for (int li = 0; li < (int)lines.size(); ++li) {
      const std::string& ln = lines[li];
      auto marker = [&](size_t a, size_t b) { applyTag(li == curLine ? "mark" : "hide", li, a, b); };
      size_t indent = 0;
      while (indent < ln.size() && ln[indent] == ' ') ++indent;

      if (ln.compare(indent, 3, "```") == 0) {
        inFence = !inFence;
        applyTag("codeblock", li, 0, ln.size());
        marker(indent, ln.size());
        continue;
      }
      if (inFence) {
        applyTag("codeblock", li, 0, ln.size());
        continue;
      }

      size_t from = 0;  // where inline content starts
      if (ln.compare(indent, 3, "\xe2\x80\xa2") == 0 && ln.size() >= indent + 4 && ln[indent + 3] == ' ') {
        if (indent) applyTag("hide", li, 0, indent);
        applyTag("bullet", li, indent, indent + 3);
        applyTag(("ind" + std::to_string(std::min<size_t>(5, indent / 2))).c_str(), li, 0, ln.size());
        from = indent + 4;
      } else if (indent < 4 && indent < ln.size() && ln[indent] == '#') {
        size_t k = indent;
        while (k < ln.size() && ln[k] == '#') ++k;
        size_t level = k - indent;
        if (level <= 3 && k < ln.size() && ln[k] == ' ') {
          marker(indent, k + 1);
          applyTag(level == 1 ? "h1" : level == 2 ? "h2" : "h3", li, k + 1, ln.size());
          from = k + 1;
        }
      } else if (indent < 4 && indent < ln.size() && ln[indent] == '>') {
        size_t k = indent + 1;
        if (k < ln.size() && ln[k] == ' ') ++k;
        marker(indent, k);
        applyTag("quote", li, 0, ln.size());
        from = k;
      } else {
        size_t k = indent;
        while (k < ln.size() && isDigit(ln[k])) ++k;
        if (k > indent && k + 1 < ln.size() && (ln[k] == '.' || ln[k] == ')') && ln[k + 1] == ' ') {
          if (indent) applyTag("hide", li, 0, indent);
          applyTag("num", li, indent, k + 1);
          applyTag(("ind" + std::to_string(std::min<size_t>(5, indent / 2))).c_str(), li, 0, ln.size());
          from = k + 2;
        }
      }
      inline_(ln, li, from, curLine);
    }
    busy = false;
  }

  // -- inline spans on one line --
  void inline_(const std::string& ln, int li, size_t from, int curLine) {
    size_t n = ln.size();
    if (from >= n) return;
    auto marker = [&](size_t a, size_t b) { applyTag(li == curLine ? "mark" : "hide", li, a, b); };
    std::vector<char> mask(n, 0);
    for (size_t i = 0; i < from; ++i) mask[i] = 1;
    std::vector<Hit> hits;  // everything that becomes a link

    // 1. code spans: a backtick run closes at the next run of the same length
    for (size_t i = from; i < n;) {
      if (ln[i] != '`' || mask[i]) { ++i; continue; }
      size_t k = i;
      while (k < n && ln[k] == '`') ++k;
      size_t run = k - i, j = k;
      bool found = false;
      while (j < n) {
        if (ln[j] != '`') { ++j; continue; }
        size_t m = j;
        while (m < n && ln[m] == '`') ++m;
        if (m - j == run) { found = true; break; }
        j = m;
      }
      if (!found) { i = k; continue; }
      if (j > k) {
        applyTag("code", li, k, j);
        marker(i, k);
        marker(j, j + run);
        for (size_t x = i; x < j + run; ++x) mask[x] = 1;
      }
      i = j + run;
    }

    // 2. [text](url)
    for (size_t i = from; i < n;) {
      if (ln[i] != '[' || mask[i]) { ++i; continue; }
      size_t close = ln.find("](", i + 1);
      size_t end = close == std::string::npos ? std::string::npos : ln.find(')', close + 2);
      bool inCode = false;
      for (size_t x = i; close != std::string::npos && x < close; ++x) inCode = inCode || mask[x];
      if (close == std::string::npos || end == std::string::npos || close == i + 1 || inCode) { ++i; continue; }
      std::string url = trimmed(ln.substr(close + 2, end - close - 2));
      if (url.empty()) { ++i; continue; }
      if (url.find(' ') != std::string::npos) url = url.substr(0, url.find(' '));
      hits.push_back({i + 1, close, normaliseUrl(url)});
      marker(i, i + 1);
      marker(close, end + 1);
      for (size_t x = close; x <= end; ++x) mask[x] = 1;
      i = end + 1;
    }
    std::vector<Hit> explicitHits = hits;
    std::vector<char> linkText(n, 0);
    for (auto& h : hits)
      for (size_t x = h.a; x < h.b; ++x) linkText[x] = 1;

    // 3. bare URLs, e-mail addresses, phone numbers (never inside code, a link's text or its address)
    std::vector<char> blocked = mask;
    for (size_t x = 0; x < n; ++x) blocked[x] = blocked[x] || linkText[x];
    std::vector<Hit> autos;
    findAutoLinks(ln, blocked, autos);
    std::sort(autos.begin(), autos.end(), [](const Hit& a, const Hit& b) { return a.a < b.a; });
    std::vector<Hit> kept;
    for (auto& h : autos) {
      bool clash = false;
      for (auto& k : kept) clash = clash || (h.a < k.b && k.a < h.b);
      if (!clash) kept.push_back(h);
    }
    for (auto& h : kept)
      for (size_t x = h.a; x < h.b; ++x) blocked[x] = 1;
    std::vector<Hit> phones;
    findPhones(ln, blocked, phones);
    for (auto& h : phones) kept.push_back(h);
    for (auto& h : kept) {
      hits.push_back(h);
      for (size_t x = h.a; x < h.b; ++x) mask[x] = 1;  // emphasis markers inside an address are part of it
    }

    // 4. emphasis: **bold** / __bold__, ~~strike~~, *italic* / _italic_
    auto isMasked = [&](size_t x) { return mask[x] != 0; };
    auto scan = [&](const std::string& delim, const char* tagName, bool wordy) {
      size_t d = delim.size();
      for (size_t i = from; i + d < n;) {
        if (ln.compare(i, d, delim) != 0 || isMasked(i)) { ++i; continue; }
        bool openOk = !isMasked(i + d - 1) && i + d < n && ln[i + d] != ' ' && ln[i + d] != '\t';
        if (wordy && i > from && isAlnum(ln[i - 1])) openOk = false;  // snake_case_names stay as they are
        if (d == 1 && i + 1 < n && ln[i + 1] == delim[0]) openOk = false;
        if (d == 1 && i > from && ln[i - 1] == delim[0]) openOk = false;
        if (!openOk) { ++i; continue; }
        size_t j = i + d;
        bool found = false;
        for (; j + d <= n; ++j) {
          if (ln.compare(j, d, delim) != 0 || isMasked(j)) continue;
          if (ln[j - 1] == ' ' || ln[j - 1] == '\t') continue;
          if (d == 1 && ((j + 1 < n && ln[j + 1] == delim[0]) || ln[j - 1] == delim[0])) continue;
          if (wordy && j + d < n && isAlnum(ln[j + d])) continue;
          found = true;
          break;
        }
        if (!found || j == i + d) { ++i; continue; }
        applyTag(tagName, li, i + d, j);
        marker(i, i + d);
        marker(j, j + d);
        for (size_t x = i; x < i + d; ++x) mask[x] = 1;  // so a `*` marker is never reused as part of another
        for (size_t x = j; x < j + d; ++x) mask[x] = 1;
        i = j + d;
      }
    };
    scan("**", "bold", false);
    scan("__", "bold", true);
    scan("~~", "strike", false);
    scan("*", "italic", false);
    scan("_", "italic", true);

    // 5. record the links (with buffer offsets, for hit-testing clicks) and style them
    for (auto& h : hits) {
      applyTag("link", li, h.a, h.b);
      GtkTextIter x, y;
      gtk_text_buffer_get_iter_at_line_index(buf, &x, li, (int)h.a);
      gtk_text_buffer_get_iter_at_line_index(buf, &y, li, (int)h.b);
      links.push_back({gtk_text_iter_get_offset(&x), gtk_text_iter_get_offset(&y), h.url});
    }
  }

  const Link* linkAt(const GtkTextIter& it) const {
    int off = gtk_text_iter_get_offset(&it);
    for (auto& l : links)
      if (off >= l.start && off < l.end) return &l;
    return nullptr;
  }
};

// ---- editing behaviour -----------------------------------------------------------------------------------------------

struct LineCtx {
  int line = 0;
  std::string text;  // the line
  int col = 0;       // cursor, as a byte index into the line
  GtkTextIter start;
};

inline LineCtx lineCtx(GtkTextBuffer* buf) {
  LineCtx c;
  GtkTextIter cur, end;
  gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
  c.line = gtk_text_iter_get_line(&cur);
  c.col = gtk_text_iter_get_line_index(&cur);
  gtk_text_buffer_get_iter_at_line(buf, &c.start, c.line);
  end = c.start;
  if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
  char* t = gtk_text_buffer_get_text(buf, &c.start, &end, TRUE);
  c.text = t ? t : "";
  g_free(t);
  return c;
}

inline size_t indentOf(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  return i;
}
inline bool isBulletLine(const std::string& s, size_t& indent) {
  indent = indentOf(s);
  return s.compare(indent, 3, "\xe2\x80\xa2") == 0 && (s.size() == indent + 3 || s[indent + 3] == ' ');
}

inline void deleteRange(GtkTextBuffer* buf, int line, int a, int b) {
  GtkTextIter x, y;
  gtk_text_buffer_get_iter_at_line_index(buf, &x, line, a);
  gtk_text_buffer_get_iter_at_line_index(buf, &y, line, b);
  gtk_text_buffer_delete(buf, &x, &y);
}

inline void wrapSelection(GtkTextBuffer* buf, const char* mark) {
  GtkTextIter s, e;
  size_t m = std::strlen(mark);
  if (gtk_text_buffer_get_selection_bounds(buf, &s, &e)) {
    int so = gtk_text_iter_get_offset(&s), eo = gtk_text_iter_get_offset(&e);
    gtk_text_buffer_begin_user_action(buf);
    gtk_text_buffer_insert(buf, &e, mark, (int)m);
    gtk_text_buffer_get_iter_at_offset(buf, &s, so);
    gtk_text_buffer_insert(buf, &s, mark, (int)m);
    gtk_text_buffer_end_user_action(buf);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buf, &a, so + (int)m);
    gtk_text_buffer_get_iter_at_offset(buf, &b, eo + (int)m);
    gtk_text_buffer_select_range(buf, &a, &b);
  } else {
    std::string pair = std::string(mark) + mark;
    gtk_text_buffer_insert_at_cursor(buf, pair.c_str(), (int)pair.size());
    GtkTextIter c;
    gtk_text_buffer_get_iter_at_mark(buf, &c, gtk_text_buffer_get_insert(buf));
    gtk_text_iter_backward_chars(&c, (int)m);
    gtk_text_buffer_place_cursor(buf, &c);
  }
}

// True when Tab / Shift+Tab should nest a bullet instead of walking to the next field.
inline bool wantsTab(GtkWidget* tv) {
  GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  LineCtx c = lineCtx(buf);
  size_t ind;
  return isBulletLine(c.text, ind);
}

inline gboolean onKey(GtkEventControllerKey*, guint kv, guint, GdkModifierType st, gpointer ud) {
  auto* v = static_cast<View*>(ud);
  GtkTextBuffer* buf = v->buf;
  bool ctrl = st & GDK_CONTROL_MASK, shift = st & GDK_SHIFT_MASK, alt = st & GDK_ALT_MASK;
  if (ctrl && !alt && (kv == GDK_KEY_b || kv == GDK_KEY_B)) { wrapSelection(buf, "**"); return GDK_EVENT_STOP; }
  if (ctrl && !alt && (kv == GDK_KEY_i || kv == GDK_KEY_I)) { wrapSelection(buf, "*"); return GDK_EVENT_STOP; }
  if (ctrl || alt) return GDK_EVENT_PROPAGATE;
  if (gtk_text_buffer_get_has_selection(buf) && kv != GDK_KEY_Tab) return GDK_EVENT_PROPAGATE;

  LineCtx c = lineCtx(buf);
  size_t ind = indentOf(c.text);
  bool bullet = false;
  {
    size_t i2;
    bullet = isBulletLine(c.text, i2);
  }

  if (kv == GDK_KEY_space) {  // "* " / "- " / "+ " at the start of a line becomes a bullet
    std::string before = c.text.substr(0, std::min<size_t>(c.col, c.text.size()));
    if (before.size() == ind + 1 && (before[ind] == '*' || before[ind] == '-' || before[ind] == '+') &&
        (size_t)c.col == c.text.size()) {
      gtk_text_buffer_begin_user_action(buf);
      deleteRange(buf, c.line, (int)ind, (int)ind + 1);
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_line_index(buf, &it, c.line, (int)ind);
      gtk_text_buffer_insert(buf, &it, "\xe2\x80\xa2 ", -1);
      gtk_text_buffer_end_user_action(buf);
      return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
  }

  if ((kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) && !shift) {
    std::string pad(ind, ' ');
    if (bullet) {
      std::string rest = c.text.size() > ind + 4 ? c.text.substr(ind + 4) : "";
      if (trimmed(rest).empty()) {  // an empty bullet ends the list (or steps one level out)
        gtk_text_buffer_begin_user_action(buf);
        if (ind >= 2) deleteRange(buf, c.line, 0, 2);
        else deleteRange(buf, c.line, 0, (int)c.text.size());
        gtk_text_buffer_end_user_action(buf);
      } else {
        std::string ins = "\n" + pad + "\xe2\x80\xa2 ";
        gtk_text_buffer_insert_at_cursor(buf, ins.c_str(), (int)ins.size());
      }
      return GDK_EVENT_STOP;
    }
    size_t k = ind;
    while (k < c.text.size() && isDigit(c.text[k])) ++k;
    if (k > ind && k + 1 < c.text.size() + 1 && k < c.text.size() && (c.text[k] == '.' || c.text[k] == ')') &&
        k + 1 < c.text.size() + 1 && (k + 1 == c.text.size() || c.text[k + 1] == ' ')) {
      std::string rest = c.text.size() > k + 2 ? c.text.substr(k + 2) : "";
      if (trimmed(rest).empty()) {
        deleteRange(buf, c.line, 0, (int)c.text.size());
      } else {
        int next = std::atoi(c.text.substr(ind, k - ind).c_str()) + 1;
        std::string ins = "\n" + pad + std::to_string(next) + std::string(1, c.text[k]) + " ";
        gtk_text_buffer_insert_at_cursor(buf, ins.c_str(), (int)ins.size());
      }
      return GDK_EVENT_STOP;
    }
    if (ind < 4 && ind < c.text.size() && c.text[ind] == '>') {
      std::string rest = c.text.size() > ind + 2 ? c.text.substr(ind + 2) : "";
      if (trimmed(rest).empty()) {
        deleteRange(buf, c.line, 0, (int)c.text.size());
      } else {
        gtk_text_buffer_insert_at_cursor(buf, "\n> ", -1);
      }
      return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
  }

  if (kv == GDK_KEY_BackSpace && bullet && (size_t)c.col == ind + 4) {  // backspace right after the glyph: back to text
    gtk_text_buffer_begin_user_action(buf);
    if (ind >= 2) deleteRange(buf, c.line, 0, 2);
    else deleteRange(buf, c.line, 0, 4);
    gtk_text_buffer_end_user_action(buf);
    return GDK_EVENT_STOP;
  }

  if ((kv == GDK_KEY_Tab || kv == GDK_KEY_ISO_Left_Tab) && bullet) {
    bool out = kv == GDK_KEY_ISO_Left_Tab || shift;
    if (out) {
      if (ind >= 2) deleteRange(buf, c.line, 0, 2);
    } else if (ind < 10) {
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_line(buf, &it, c.line);
      gtk_text_buffer_insert(buf, &it, "  ", 2);
    }
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

// ---- construction ----------------------------------------------------------------------------------------------------

inline void onChanged(GtkTextBuffer*, gpointer p) { static_cast<View*>(p)->restyle(); }
inline void onCursor(GObject*, GParamSpec*, gpointer p) {
  auto* vv = static_cast<View*>(p);
  GtkTextIter ci;
  gtk_text_buffer_get_iter_at_mark(vv->buf, &ci, gtk_text_buffer_get_insert(vv->buf));
  if (gtk_text_iter_get_line(&ci) != vv->lastCursorLine) vv->restyle();
}

inline const Link* linkUnder(View* vv, double x, double y) {
  int bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(vv->view), GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  GtkTextIter it;
  if (!gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(vv->view), &it, nullptr, bx, by)) return nullptr;
  return vv->linkAt(it);
}

inline void onLinkClick(GtkGestureClick* gc, int, double x, double y, gpointer p) {
  auto* vv = static_cast<View*>(p);
  if (gtk_text_buffer_get_has_selection(vv->buf)) return;
  if (const Link* l = linkUnder(vv, x, y)) {
    std::string url = l->url;
    gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
    if (gOpenLink) gOpenLink(url);
  }
}

inline void onLinkHover(GtkEventControllerMotion*, double x, double y, gpointer p) {
  auto* vv = static_cast<View*>(p);
  gtk_widget_set_cursor_from_name(vv->view, linkUnder(vv, x, y) ? "pointer" : (vv->readOnly ? "default" : "text"));
}


inline View* from(GtkWidget* tv) { return static_cast<View*>(g_object_get_data(G_OBJECT(tv), "md-view")); }

inline std::string markdownOf(GtkWidget* tv) {
  GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(b, &s, &e);
  char* t = gtk_text_buffer_get_text(b, &s, &e, TRUE);
  std::string out = toMarkdown(t ? t : "");
  g_free(t);
  return out;
}

inline GtkWidget* create(const std::string& markdown, bool readOnly) {
  auto* v = new View();
  v->readOnly = readOnly;
  v->view = gtk_text_view_new();
  v->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(v->view));
  g_object_set_data_full(G_OBJECT(v->view), "md-view", v, [](gpointer p) { delete static_cast<View*>(p); });
  registry().push_back(v);
  GtkWidget* tv = v->view;
  gtk_widget_add_css_class(tv, "bare");
  if (readOnly) gtk_widget_add_css_class(tv, "md-ro");
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(tv), FALSE);  // Tab walks the editor's fields (bullets nest it themselves)
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 2);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv), 2);
  v->makeTags();
  gtk_text_buffer_set_text(v->buf, toBuffer(markdown).c_str(), -1);

  if (readOnly) {
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    gtk_widget_set_focusable(tv, FALSE);
  } else {
    GtkEventController* kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(onKey), v);
    gtk_widget_add_controller(tv, kc);
  }
  if (!readOnly) {
    GtkEventController* fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "enter", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer p) { auto* x = static_cast<View*>(p); x->focused = true; x->restyle(); }), v);
    g_signal_connect(fc, "leave", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer p) { auto* x = static_cast<View*>(p); x->focused = false; x->restyle(); }), v);
    gtk_widget_add_controller(tv, fc);
  }
  g_signal_connect(v->buf, "changed", G_CALLBACK(onChanged), v);
  g_signal_connect(v->buf, "notify::cursor-position", G_CALLBACK(onCursor), v);

  // a click on a link opens it; the pointer says so
  GtkGesture* g = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
  g_signal_connect(g, "released", G_CALLBACK(onLinkClick), v);
  gtk_widget_add_controller(tv, GTK_EVENT_CONTROLLER(g));
  GtkEventController* mc = gtk_event_controller_motion_new();
  g_signal_connect(mc, "motion", G_CALLBACK(onLinkHover), v);
  gtk_widget_add_controller(tv, mc);
  v->restyle();
  return tv;
}


// ---- read-only rendering -------------------------------------------------------------------------------------------
//
// A GtkLabel is the dependable way to show wrapped text at a size the layout can predict (a GtkTextView's wrapped
// height settles late), so the description under a project's title is rendered to Pango markup. The same parser as
// the editor does the work: style a scratch buffer, then read its tags back out.

inline std::string hexOf(const Rgba& c) {
  char b[16];
  std::snprintf(b, sizeof b, "#%02x%02x%02x", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
  return b;
}

inline std::string toPango(const std::string& markdown) {
  View v;
  v.readOnly = true;
  v.buf = gtk_text_buffer_new(nullptr);
  v.makeTags();
  gtk_text_buffer_set_text(v.buf, toBuffer(markdown).c_str(), -1);
  v.restyle();
  std::string out;
  GtkTextIter it, end;
  gtk_text_buffer_get_start_iter(v.buf, &it);
  gtk_text_buffer_get_end_iter(v.buf, &end);
  while (!gtk_text_iter_equal(&it, &end)) {
    GtkTextIter next = it;
    if (!gtk_text_iter_forward_to_tag_toggle(&next, nullptr)) next = end;
    GSList* tags = gtk_text_iter_get_tags(&it);
    std::vector<std::string> names;
    for (GSList* l = tags; l; l = l->next) {
      gchar* n = nullptr;
      g_object_get(l->data, "name", &n, nullptr);
      if (n) names.push_back(n);
      g_free(n);
    }
    g_slist_free(tags);
    auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    char* raw = gtk_text_buffer_get_text(v.buf, &it, &next, TRUE);
    std::string text = raw ? raw : "";
    g_free(raw);
    if (!has("hide")) {
      for (int l = 1; l < 6; ++l)  // nested bullets: indent the line once, at its start
        if (has(("ind" + std::to_string(l)).c_str()) && gtk_text_iter_starts_line(&it)) out += std::string(l * 4, ' ');
      char* esc = g_markup_escape_text(text.c_str(), -1);
      std::string open, close;
      auto wrap = [&](const std::string& o, const char* c) {
        open += o;
        close = std::string(c) + close;
      };
      if (has("h1")) wrap("<span size=\"x-large\" weight=\"bold\">", "</span>");
      if (has("h2")) wrap("<span size=\"large\" weight=\"bold\">", "</span>");
      if (has("h3")) wrap("<b>", "</b>");
      if (has("bold")) wrap("<b>", "</b>");
      if (has("italic")) wrap("<i>", "</i>");
      if (has("strike")) wrap("<s>", "</s>");
      if (has("code") || has("codeblock"))
        wrap("<span font_family=\"monospace\" bgcolor=\"" + hexOf(gPal.fg) + "\" bgalpha=\"9%\">", "</span>");
      if (has("bullet")) wrap("<span foreground=\"" + hexOf(gPal.accent) + "\">", "</span>");
      if (has("link")) {
        int off = gtk_text_iter_get_offset(&it);
        for (auto& l : v.links)
          if (off >= l.start && off < l.end) {
            char* u = g_markup_escape_text(l.url.c_str(), -1);
            wrap(std::string("<a href=\"") + u + "\">", "</a>");
            g_free(u);
            break;
          }
      }
      out += open + esc + close;
      g_free(esc);
    }
    it = next;
  }
  g_object_unref(v.buf);
  v.buf = nullptr;
  return out;
}

// Re-colours every live note view after a theme change.
inline void repaintAll() {
  for (View* v : registry()) {
    v->paintTags();
    v->restyle();
  }
}

}  // namespace md
