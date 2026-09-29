#include "quickcapture.h"

#include <clocale>
#include <csignal>
#include <cstdio>

#include "form.h"
#include "input.h"
#include "util.h"

namespace {

// Same-name resolution taskForm() uses (see app.cpp), duplicated here since
// this path never constructs an App -- it's meant to start faster and pull
// in less than the full TUI.
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

}  // namespace

int runQuickCaptureTui(Store& s) {
  setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  set_escdelay(25);
  curs_set(0);
  start_color();
  use_default_colors();
  init_pair(1, COLOR_CYAN, -1);
  // Same Kitty-keyboard-protocol opt-in App::run() does, so Shift+Enter
  // saves immediately here too.
  std::fputs("\x1b[>1u", stdout);
  std::fflush(stdout);
  std::signal(SIGPIPE, SIG_IGN);

  std::vector<FormField> fields = {
      {"Title", "", true, 2, true},
      {"List", "", false, 1},     // blank = Inbox; an existing area or project name to redirect
      {"Do date", "", false, 1},  // YYYY-MM-DD or 'someday'; blank = unscheduled
  };
  int w = 56, valueX = 11, valueW = w - valueX - 3;
  int startY = 3, labelX = 2;
  int footerY = startY;
  for (auto& f : fields) footerY += f.multiline ? std::max(1, f.height) : 1;
  int h = footerY + 3;
  WINDOW* win = openDialog(h, w, "QUICK CAPTURE");
  getmaxyx(win, h, w);
  wattron(win, A_DIM);
  mvwprintw(win, footerY + 1, 2, "%s",
            clip("blank List = Inbox \xc2\xb7 Tab next \xc2\xb7 Enter/\xe2\x87\xa7"
                 "Enter save \xc2\xb7 Esc cancel",
                 w - 4)
                .c_str());
  wattroff(win, A_DIM);
  wrefresh(win);
  FormResult r = runForm(win, fields, startY, labelX, valueX, valueW);
  delwin(win);
  std::fputs("\x1b[<1u", stdout);
  std::fflush(stdout);
  endwin();

  if (r == FormResult::Cancelled) return 1;
  return saveQuickCapture(s, fields[0].value, fields[1].value, fields[2].value);
}

int saveQuickCapture(Store& s, const std::string& title, const std::string& list, const std::string& doDate) {
  if (trimmed(title).empty()) return 1;
  Item t;
  t.title = title;
  t.doDate = doDate;  // Store::saveTask turns the literal "someday" into the someday flag
  int projectId = findProjectIdByName(s, list);
  int areaId = projectId ? s.projectAreaId(projectId) : findAreaIdByName(s, list);
  s.saveTask(t, areaId, projectId);
  return 0;
}

int runQuickCapture(Store& s, bool forceTui) {
#ifdef STRIDE_HAVE_GTK4
  if (!forceTui) {
    int code = 1;
    if (runQuickCaptureGtk(s, code)) return code;
    // No display reachable (tty, ssh, ...): fall through to the terminal UI.
  }
#else
  (void)forceTui;
#endif
  return runQuickCaptureTui(s);
}
