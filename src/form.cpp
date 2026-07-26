#include "form.h"

#include <algorithm>

#include "input.h"
#include "util.h"

WINDOW* openDialog(int h, int w, const std::string& title) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  h = std::min(h, std::max(3, rows - 2));
  w = std::min(w, std::max(10, cols - 2));
  WINDOW* win = newwin(h, w, std::max(0, (rows - h) / 2), std::max(0, (cols - w) / 2));
  keypad(win, TRUE);
  box(win, 0, 0);
  if (!title.empty()) {
    wattron(win, A_BOLD | COLOR_PAIR(1));
    mvwprintw(win, 1, 2, "%s", clip(title, w - 4).c_str());
    wattroff(win, A_BOLD | COLOR_PAIR(1));
  }
  return win;
}

FormResult runForm(WINDOW* win, std::vector<FormField>& fields, int startY, int labelX, int valueX, int valueW) {
  if (fields.empty()) return FormResult::Saved;
  std::vector<int> rowY(fields.size());
  int y = startY;
  for (size_t i = 0; i < fields.size(); ++i) {
    rowY[i] = y;
    y += fields[i].multiline ? std::max(2, fields[i].height) : 1;
  }
  for (size_t i = 0; i < fields.size(); ++i) mvwprintw(win, rowY[i], labelX, "%s", fields[i].label.c_str());

  int focus = 0;
  while (true) {
    for (size_t i = 0; i < fields.size(); ++i) {
      if ((int)i == focus) continue;
      if (fields[i].multiline) {
        auto lines = wrapText(fields[i].value, valueW);
        for (int r = 0; r < fields[i].height; ++r) {
          clearRect(win, rowY[i] + r, valueX, valueW);
          std::string content = r < (int)lines.size() ? lines[r] : "";
          wattron(win, A_DIM);
          mvwprintw(win, rowY[i] + r, valueX, "%s", clip(content, valueW).c_str());
          wattroff(win, A_DIM);
        }
      } else {
        clearRect(win, rowY[i], valueX, valueW);
        wattron(win, A_DIM);
        mvwprintw(win, rowY[i], valueX, "%s", clip(fields[i].value, valueW).c_str());
        wattroff(win, A_DIM);
      }
    }
    wrefresh(win);
    FieldOutcome outcome = fields[focus].multiline
                                ? editArea(win, rowY[focus], valueX, valueW, fields[focus].height, fields[focus].value)
                                : editLine(win, rowY[focus], valueX, valueW, fields[focus].value);
    if (outcome == FieldOutcome::Cancelled) return FormResult::Cancelled;
    if (outcome == FieldOutcome::Saved) return FormResult::Saved;
    if (outcome == FieldOutcome::Next) {
      if (focus + 1 >= (int)fields.size()) return FormResult::Saved;
      focus++;
    } else if (outcome == FieldOutcome::Prev) {
      focus = std::max(0, focus - 1);
    }
  }
}

bool confirmDialog(const std::string& message) {
  int w = std::clamp((int)message.size() + 10, 30, 70);
  WINDOW* win = openDialog(5, w, "");
  int wh, ww;
  getmaxyx(win, wh, ww);
  (void)wh;
  mvwprintw(win, 2, 2, "%s", clip(message + " (y/N)", ww - 4).c_str());
  wrefresh(win);
  KeyEvent k = readKey(win);
  delwin(win);
  return k.type == Key::Char && (k.ch == 'y' || k.ch == 'Y');
}
