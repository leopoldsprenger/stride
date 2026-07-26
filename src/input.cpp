#include "input.h"

#include <algorithm>
#include <cctype>
#include <vector>

KeyEvent readKey(WINDOW* win) {
  int c = wgetch(win);
  if (c == ERR) return {Key::None};
  if (c == 27) {
    // Might be a standalone Escape, or the start of a multi-byte sequence
    // (arrow keys, or a CSI-u encoded key) that keypad() didn't recognize.
    // Peek briefly for a continuation instead of blocking indefinitely.
    wtimeout(win, 15);
    int c1 = wgetch(win);
    if (c1 == ERR) {
      wtimeout(win, -1);
      return {Key::Escape};
    }
    if (c1 == '[' || c1 == 'O') {
      std::string seq;
      int c2;
      while ((c2 = wgetch(win)) != ERR) {
        seq += (char)c2;
        if (isalpha((unsigned char)c2) || c2 == '~') break;
        if (seq.size() > 16) break;
      }
      wtimeout(win, -1);
      if (seq == "C") return {Key::Right};
      if (seq == "D") return {Key::Left};
      if (seq == "H") return {Key::Home};
      if (seq == "F") return {Key::End};
      if (seq == "A") return {Key::Up};
      if (seq == "B") return {Key::Down};
      if (seq == "Z") return {Key::ShiftTab};
      if (seq == "3~") return {Key::Delete};
      if (seq == "1~" || seq == "7~") return {Key::Home};
      if (seq == "4~" || seq == "8~") return {Key::End};
      if (!seq.empty() && seq.back() == 'u') {
        std::string body = seq.substr(0, seq.size() - 1);
        auto semi = body.find(';');
        int code = 0, mods = 1;
        try {
          code = std::stoi(semi == std::string::npos ? body : body.substr(0, semi));
        } catch (...) {
        }
        if (semi != std::string::npos) {
          try {
            mods = std::stoi(body.substr(semi + 1));
          } catch (...) {
          }
        }
        bool modified = mods > 1;
        if (code == 13 || code == 10) return modified ? KeyEvent{Key::ShiftEnter} : KeyEvent{Key::Enter};
        if (code == 9) return modified ? KeyEvent{Key::ShiftTab} : KeyEvent{Key::Tab};
        if (code == 27) return {Key::Escape};
      }
      return {Key::None};  // unrecognized sequence: swallow it, never leak raw bytes
    }
    wtimeout(win, -1);
    return {Key::Escape};
  }
  if (c == '\n' || c == '\r' || c == KEY_ENTER) return {Key::Enter};
  if (c == KEY_LEFT) return {Key::Left};
  if (c == KEY_RIGHT) return {Key::Right};
  if (c == KEY_UP) return {Key::Up};
  if (c == KEY_DOWN) return {Key::Down};
  if (c == KEY_HOME) return {Key::Home};
  if (c == KEY_END) return {Key::End};
  if (c == KEY_DC) return {Key::Delete};
  if (c == KEY_BACKSPACE || c == 127 || c == 8) return {Key::Backspace};
  if (c == KEY_BTAB) return {Key::ShiftTab};
  if (c == '\t') return {Key::Tab};
  if (c >= 32 && c < 127) return {Key::Char, (char)c};
  return {Key::None};
}

void clearRect(WINDOW* win, int y, int x, int width, int height) {
  if (width <= 0) return;
  std::string blank(width, ' ');
  for (int r = 0; r < height; ++r) mvwprintw(win, y + r, x, "%s", blank.c_str());
}

FieldOutcome editLine(WINDOW* win, int y, int x, int width, std::string& text) {
  size_t cursor = text.size();
  size_t scroll = 0;
  while (true) {
    if (cursor < scroll) scroll = cursor;
    if (width > 0 && cursor > scroll + (size_t)width - 1) scroll = cursor - width + 1;
    std::string visible = text.substr(std::min(scroll, text.size()), std::max(width, 0));
    if ((int)visible.size() < width) visible += std::string(width - visible.size(), ' ');
    mvwprintw(win, y, x, "%s", visible.c_str());
    wmove(win, y, x + (int)(cursor - scroll));
    curs_set(1);
    wrefresh(win);
    KeyEvent k = readKey(win);
    switch (k.type) {
      case Key::Char:
        text.insert(text.begin() + cursor, k.ch);
        cursor++;
        break;
      case Key::Backspace:
        if (cursor > 0) {
          text.erase(cursor - 1, 1);
          cursor--;
        }
        break;
      case Key::Delete:
        if (cursor < text.size()) text.erase(cursor, 1);
        break;
      case Key::Left:
        if (cursor > 0) cursor--;
        break;
      case Key::Right:
        if (cursor < text.size()) cursor++;
        break;
      case Key::Home:
        cursor = 0;
        break;
      case Key::End:
        cursor = text.size();
        break;
      case Key::Enter:
      case Key::Tab:
      case Key::Down:
        curs_set(0);
        return FieldOutcome::Next;
      case Key::ShiftTab:
      case Key::Up:
        curs_set(0);
        return FieldOutcome::Prev;
      case Key::ShiftEnter:
        curs_set(0);
        return FieldOutcome::Saved;
      case Key::Escape:
        curs_set(0);
        return FieldOutcome::Cancelled;
      default:
        break;
    }
  }
}

namespace {
struct WrapLine {
  size_t start;
  std::string text;
};

std::vector<WrapLine> wrapWithOffsets(const std::string& text, int width) {
  std::vector<WrapLine> lines;
  if (width < 1) width = 1;
  size_t start = 0;
  while (true) {
    size_t nl = text.find('\n', start);
    std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    size_t paraStart = start;
    if (para.empty()) {
      lines.push_back({paraStart, ""});
    } else {
      size_t i = 0;
      while (i < para.size()) {
        size_t remaining = para.size() - i;
        if (remaining <= (size_t)width) {
          lines.push_back({paraStart + i, para.substr(i)});
          break;
        }
        size_t brk = para.rfind(' ', i + width);
        size_t cut = (brk == std::string::npos || brk < i) ? i + width : brk;
        lines.push_back({paraStart + i, para.substr(i, cut - i)});
        i = cut;
        while (i < para.size() && para[i] == ' ') ++i;
      }
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  if (lines.empty()) lines.push_back({0, ""});
  return lines;
}
}  // namespace

FieldOutcome editArea(WINDOW* win, int y, int x, int width, int height, std::string& text) {
  size_t cursor = text.size();
  int scrollLine = 0;
  while (true) {
    auto lines = wrapWithOffsets(text, width);
    int curLine = 0;
    for (int i = 0; i < (int)lines.size(); ++i) {
      if (lines[i].start <= cursor) curLine = i;
      else break;
    }
    if (curLine < scrollLine) scrollLine = curLine;
    if (curLine > scrollLine + height - 1) scrollLine = curLine - height + 1;
    for (int row = 0; row < height; ++row) {
      int li = scrollLine + row;
      std::string content = li < (int)lines.size() ? lines[li].text : "";
      if ((int)content.size() < width) content += std::string(width - content.size(), ' ');
      mvwprintw(win, y + row, x, "%s", content.substr(0, width).c_str());
    }
    int curCol = (int)(cursor - lines[curLine].start);
    wmove(win, y + (curLine - scrollLine), x + std::min(curCol, width - 1));
    curs_set(1);
    wrefresh(win);
    KeyEvent k = readKey(win);
    switch (k.type) {
      case Key::Char:
        text.insert(text.begin() + cursor, k.ch);
        cursor++;
        break;
      case Key::Enter:
        text.insert(text.begin() + cursor, '\n');
        cursor++;
        break;
      case Key::Backspace:
        if (cursor > 0) {
          text.erase(cursor - 1, 1);
          cursor--;
        }
        break;
      case Key::Delete:
        if (cursor < text.size()) text.erase(cursor, 1);
        break;
      case Key::Left:
        if (cursor > 0) cursor--;
        break;
      case Key::Right:
        if (cursor < text.size()) cursor++;
        break;
      case Key::Home:
        cursor = lines[curLine].start;
        break;
      case Key::End:
        cursor = lines[curLine].start + lines[curLine].text.size();
        break;
      case Key::Up:
        if (curLine > 0) {
          int col = (int)(cursor - lines[curLine].start);
          cursor = lines[curLine - 1].start + std::min((size_t)col, lines[curLine - 1].text.size());
        } else {
          curs_set(0);
          return FieldOutcome::Prev;
        }
        break;
      case Key::Down:
        if (curLine + 1 < (int)lines.size()) {
          int col = (int)(cursor - lines[curLine].start);
          cursor = lines[curLine + 1].start + std::min((size_t)col, lines[curLine + 1].text.size());
        } else {
          curs_set(0);
          return FieldOutcome::Next;
        }
        break;
      case Key::Tab:
        curs_set(0);
        return FieldOutcome::Next;
      case Key::ShiftTab:
        curs_set(0);
        return FieldOutcome::Prev;
      case Key::ShiftEnter:
        curs_set(0);
        return FieldOutcome::Saved;
      case Key::Escape:
        curs_set(0);
        return FieldOutcome::Cancelled;
      default:
        break;
    }
  }
}
