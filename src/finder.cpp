#include "finder.h"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "form.h"
#include "input.h"

int fuzzyScore(const std::string& needleRaw, const std::string& haystackRaw) {
  if (needleRaw.empty()) return 0;
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

std::string friendlyDate(const std::string& iso) {
  if (iso.empty()) return "No date";
  std::tm tmv{};
  strptime(iso.c_str(), "%Y-%m-%d", &tmv);
  std::mktime(&tmv);
  char buf[24];
  std::strftime(buf, sizeof(buf), "%a, %b %d", &tmv);
  return buf;
}

std::string bucketLabel(const std::string& completedAtIso) {
  if (completedAtIso.size() < 10) return "Earlier";
  std::string date = completedAtIso.substr(0, 10);
  std::string tdy = today();
  if (date == tdy) return "Today";
  std::tm tmv{};
  strptime(tdy.c_str(), "%Y-%m-%d", &tmv);
  std::time_t t = std::mktime(&tmv) - 86400;
  char yb[11];
  std::strftime(yb, sizeof(yb), "%F", std::localtime(&t));
  if (date == yb) return "Yesterday";
  int y = std::stoi(date.substr(0, 4)), m = std::stoi(date.substr(5, 2));
  int cy = std::stoi(tdy.substr(0, 4));
  static const char* months[] = {"",     "January", "February", "March",     "April",   "May",      "June",
                                  "July", "August",  "September", "October", "November", "December"};
  if (y == cy) return std::string(months[m]) + " " + std::to_string(y);
  return std::to_string(y);
}

int runPicker(const std::string& title, const std::vector<PickerItem>& items) {
  std::string query;
  int sel = 0;
  // Same reasoning as openDialog() in form.cpp: force a clean repaint
  // before opening, in case a previous popup closed without the main loop
  // getting a redraw in between.
  touchwin(stdscr);
  wnoutrefresh(stdscr);
  doupdate();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int h = std::min(rows - 4, 20), w = std::min(cols - 4, 72);
  WINDOW* win = newwin(h, w, std::max(0, (rows - h) / 2), std::max(0, (cols - w) / 2));
  keypad(win, TRUE);
  int result = -1;
  while (true) {
    std::vector<std::pair<int, int>> scored;  // (score, index)
    for (int i = 0; i < (int)items.size(); ++i) {
      int sc = fuzzyScore(query, items[i].label + " " + items[i].sub);
      if (sc >= 0) scored.push_back({sc, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    int shown = std::min((int)scored.size(), h - 5);
    sel = std::clamp(sel, 0, std::max(0, shown - 1));
    werase(win);
    box(win, 0, 0);
    wattron(win, A_BOLD | COLOR_PAIR(1));
    mvwprintw(win, 1, 2, "%s", clip(title, w - 4).c_str());
    wattroff(win, A_BOLD | COLOR_PAIR(1));
    mvwprintw(win, 2, 2, "> %s", clip(query, w - 6).c_str());
    mvwhline(win, 3, 1, ACS_HLINE, w - 2);
    for (int i = 0; i < shown; ++i) {
      auto& it = items[scored[i].second];
      if (i == sel) wattron(win, A_REVERSE);
      mvwprintw(win, 4 + i, 2, "%s", clip(it.icon + " " + it.label, w - 26).c_str());
      wattron(win, A_DIM);
      mvwprintw(win, 4 + i, w - 22, "%s", clip(it.sub, 20).c_str());
      wattroff(win, A_DIM);
      if (i == sel) wattroff(win, A_REVERSE);
    }
    if (scored.empty()) {
      wattron(win, A_DIM);
      mvwprintw(win, 4, 2, "No matches");
      wattroff(win, A_DIM);
    }
    curs_set(1);
    wmove(win, 2, 4 + displayWidth(query));
    wrefresh(win);
    KeyEvent k = readKey(win);
    if (k.type == Key::Escape) break;
    if (k.type == Key::Enter) {
      if (!scored.empty()) result = scored[sel].second;
      break;
    }
    if (k.type == Key::Up) sel = std::max(0, sel - 1);
    else if (k.type == Key::Down) sel++;
    else if (k.type == Key::Backspace) {
      if (!query.empty()) query.pop_back();
    } else if (k.type == Key::Char) {
      query.push_back(k.ch);
    }
  }
  curs_set(0);
  delwin(win);
  return result;
}

std::string runTagPicker(const std::vector<std::string>& existingTags, std::string selected) {
  std::string original = selected;
  std::string query;
  int sel = 0;
  touchwin(stdscr);
  wnoutrefresh(stdscr);
  doupdate();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int h = std::min(rows - 4, 20), w = std::min(cols - 4, 60);
  WINDOW* win = newwin(h, w, std::max(0, (rows - h) / 2), std::max(0, (cols - w) / 2));
  keypad(win, TRUE);
  auto isSelected = [&](const std::string& tag) {
    for (auto& t : splitComma(selected))
      if (lower(trimmed(t)) == lower(tag)) return true;
    return false;
  };
  auto toggle = [&](const std::string& tag) {
    if (tag == "none") {
      selected = selected == "none" ? "" : "none";
      return;
    }
    auto parts = splitComma(selected);
    bool removed = false;
    std::string rebuilt;
    for (auto& t : parts) {
      if (lower(trimmed(t)) == lower(tag)) {
        removed = true;
        continue;
      }
      if (trimmed(t) == "none") continue;
      if (!rebuilt.empty()) rebuilt += ",";
      rebuilt += trimmed(t);
    }
    if (!removed) {
      if (!rebuilt.empty()) rebuilt += ",";
      rebuilt += tag;
    }
    selected = rebuilt;
  };
  while (true) {
    std::vector<std::string> candidates = {"none"};
    candidates.insert(candidates.end(), existingTags.begin(), existingTags.end());
    std::vector<std::pair<int, std::string>> scored;
    for (auto& t : candidates) {
      int sc = fuzzyScore(query, t);
      if (sc >= 0) scored.push_back({sc, t});
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    int shown = std::min((int)scored.size(), h - 7);
    sel = std::clamp(sel, 0, std::max(0, shown - 1));
    werase(win);
    box(win, 0, 0);
    wattron(win, A_BOLD | COLOR_PAIR(1));
    mvwprintw(win, 1, 2, "FILTER BY TAGS");
    wattroff(win, A_BOLD | COLOR_PAIR(1));
    mvwprintw(win, 2, 2, "> %s", clip(query, w - 6).c_str());
    wattron(win, A_DIM);
    mvwprintw(win, 3, 2, "%s",
              clip(selected.empty() ? "(no filter)" : "selected: " + selected, w - 4).c_str());
    wattroff(win, A_DIM);
    mvwhline(win, 4, 1, ACS_HLINE, w - 2);
    for (int i = 0; i < shown; ++i) {
      const std::string& tag = scored[i].second;
      bool on = isSelected(tag);
      if (i == sel) wattron(win, A_REVERSE);
      mvwprintw(win, 5 + i, 2, "%s %s", on ? "\xe2\x97\x89" : "\xe2\x97\x8b",
                clip(tag == "none" ? "none (untagged)" : tag, w - 6).c_str());
      if (i == sel) wattroff(win, A_REVERSE);
    }
    wattron(win, A_DIM);
    mvwprintw(win, h - 2, 2, "%s", clip("Enter toggles \xc2\xb7 Tab applies \xc2\xb7 Esc cancels", w - 4).c_str());
    wattroff(win, A_DIM);
    curs_set(1);
    wmove(win, 2, 4 + displayWidth(query));
    wrefresh(win);
    KeyEvent k = readKey(win);
    if (k.type == Key::Escape) {
      selected = original;
      break;
    }
    if (k.type == Key::Tab) break;
    if (k.type == Key::Enter) {
      if (!scored.empty()) toggle(scored[sel].second);
      query.clear();
    } else if (k.type == Key::Up) sel = std::max(0, sel - 1);
    else if (k.type == Key::Down) sel++;
    else if (k.type == Key::Backspace) {
      if (!query.empty()) query.pop_back();
    } else if (k.type == Key::Char) {
      query.push_back(k.ch);
    }
  }
  curs_set(0);
  delwin(win);
  return selected;
}
