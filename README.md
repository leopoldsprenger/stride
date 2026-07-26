# Stride

Stride is a quiet, keyboard-first GTD console for people who want their next action close at hand. Its shape borrows from Things: capture into **Inbox**, focus **Today**, plan **Upcoming**, browse **Anytime** and **Someday**, and keep completions in **Logbook**. Areas hold standalone tasks and projects; projects are richer lists with a description, dates, headings, and a lifecycle.

![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white) ![SQLite](https://img.shields.io/badge/data-SQLite-003B57?logo=sqlite&logoColor=white)

## Install

Requirements: CMake 3.20+, a C++23 compiler, `ncursesw`, and `sqlite3`.

```sh
# macOS (Homebrew)
brew install cmake ncurses sqlite

# Debian/Ubuntu
sudo apt install cmake g++ libncursesw5-dev libsqlite3-dev pkg-config

git clone git@github.com:YOUR-USER/stride.git
cd stride
cmake -S . -B build
cmake --build build
cmake --install build --prefix ~/.local
```

Ensure `~/.local/bin` is on your `PATH`, then launch it from anywhere:

```sh
stride
```

The database lives in an OS-appropriate location: `~/Library/Application Support/stride/stride.db` on macOS, `$XDG_DATA_HOME/stride/stride.db` (or `~/.local/share/stride/stride.db`) on Linux. Override either with `STRIDE_DATA_DIR`. It uses SQLite's WAL mode -- compact, durable, and easy to back up or inspect.

## Source layout

```
src/util.h     data structs (Item, Ref, ChecklistItem) + pure-function helpers
src/input.*    raw-escape-safe key reader; single-line and multi-line field editors
src/form.*     dialog window + multi-field form with Tab/Shift+Tab navigation
src/store.*    all SQLite access
src/finder.*   fuzzy matching + the live-filter popups (find, move, tag picker)
src/app.*      view logic, rendering, input dispatch
src/main.cpp   entry point
```

## Keyboard

| Key | Action |
| --- | --- |
| `j`/`k`, `↑`/`↓` | Move the selection |
| `h`/`l`, `←`/`→`, `Esc` | Switch sidebar list; inside a project/area, steps back out instead |
| `J`/`K` | Reorder the selected task or project; dragging a task past a heading moves it into that section |
| `b` | Toggle the sidebar |
| `n` / `p` / `a` | New task / project / area -- defaults to the project/area you're in, and a new task inherits the selected heading (or the selected task's heading, placed right after it) |
| `N` | New heading (inside a project) |
| `Enter` | Edit a task; open a project; rename/delete a heading |
| `e` | Edit the selected task, project, or area |
| `c` | Edit the selected task's checklist |
| `x` | Complete a task, or open the complete/cancel/delete dialog for a project or area |
| `X` | Same complete/cancel/delete dialog, for the project or area you're currently *inside* |
| `m` | Move the selected task to a different project (fuzzy-searchable) |
| `f` | Fuzzy find -- live-filters every list, area, project, and task as you type; Enter jumps straight to it |
| `T` | Filter by tags -- fuzzy-searches your existing tags instead of guessing; Enter toggles a tag, Tab applies |
| `A` | Toggle grouping by area, then project, within it |
| `v` | Visual mode: `j`/`k` extend a selection, then `x` completes, `m` moves, `T` tags, `s`/`S` set a do date/deadline for all of them at once |
| `u` | Revive (un-complete / un-cancel) an item in Logbook, Logged Projects, or Archived Areas |
| `d` | Permanently delete (only inside Logbook, Logged Projects, or Archived Areas -- no confirmation for tasks, confirmation required for projects/areas) |
| `?` | Show shortcuts |
| `q` | Quit |

**In any dialog:** every field starts prepopulated with its current value, cursor at the end, ready to edit -- Left/Right/Home/End/Backspace/Delete work as expected and never print raw escape codes. Tab/Shift+Tab or Up/Down move between fields without losing what you've typed elsewhere in the form. **Escape always cancels the whole dialog**, consistently, from any field. **Shift+Enter saves immediately**, skipping whatever fields you haven't reached yet -- this needs a terminal that reports modified keys (Ghostty, Kitty, WezTerm, foot); elsewhere, just Tab/Enter through to the last field as usual.

## Notes

Stride uses your terminal's default background so it belongs with Ghostty, Kitty, or whatever theme you run. Set your terminal font to **JetBrainsMono Nerd Font** for the sidebar glyphs.

Each task and project row shows small right-aligned indicators: a flag with its date for a deadline (red once due), a circle with its date for a do date, and single-glyph markers for a non-empty description, a checklist, and tags -- so you can tell what's on a task without opening it. A task's checklist is a simple add/check/delete list edited with `c`; it never appears in list views itself, only its indicator does.

## Dates, lifecycle, and hidden views

Use `YYYY-MM-DD` for a **do date** or **deadline**, or type `someday` as the do date. Items due or overdue surface automatically in Today; Upcoming groups future work by do date; Anytime shows everything open except Someday (including what's due today or later). Someday tasks are dimmed everywhere they appear. Inbox holds only unscheduled, unassigned captures -- give a task a do date, area, or project and it moves on.

An area lists its open projects (by do date), then its own standalone tasks split into **Tasks** (no do date) and **Scheduled**, then **Someday**. A project's own description sits at the top of its page whether it's active, completed, or cancelled. Its tasks sort by do date within whatever headings you've created (`N`), with ungrouped tasks first and heading membership shown by indentation.

In Today/Tomorrow/Upcoming, a project and its tasks share one freely-reorderable list: if every open task in a project is due the same day as the project itself, only the project is shown; otherwise the specific tasks that match are shown alongside it. Turning on grouping (`A`) sorts tasks with no area or project to the very top, then by area, then by project underneath it -- projects show under their area by name alone, not "Area / Project".

`f` also reaches **Tomorrow**, **Deadlines**, **Logged Projects**, and **Archived Areas** without crowding the sidebar -- and, unlike a plain jump list, it fuzzy-matches every area, project, and open task too, taking you straight to wherever that item lives. Logbook and Logged Projects sort by completion time (tracked to the second) and group by Today, Yesterday, month (this year), or year; `u` brings anything back from any of these views.
