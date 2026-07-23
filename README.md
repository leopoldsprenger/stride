# Stride

Stride is a quiet, keyboard-first GTD console for people who want their next action close at hand. Its shape borrows from Things: capture into **Inbox**, focus **Today**, plan **Upcoming**, browse **Anytime** and **Someday**, and keep completions in **Logbook**. Areas can hold standalone tasks and projects; projects are simply richer lists with descriptions, dates, headings, and a lifecycle.

![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white) ![SQLite](https://img.shields.io/badge/data-SQLite-003B57?logo=sqlite&logoColor=white)

## Install

Requirements: CMake 3.20+, a C++23 compiler, `ncursesw`, and `sqlite3`. On macOS with Homebrew:

```sh
brew install cmake ncurses sqlite
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

The database is stored at `~/.local/share/stride/stride.db`. It uses SQLite’s WAL mode: a compact, durable local format that remains easy to back up or inspect with `sqlite3`.

## Keyboard

| Key | Action |
| --- | --- |
| `j` / `k`, `↑` / `↓` | Select an item |
| `h` / `l`, `←` / `→` | Switch sidebar list |
| `J` / `K` | Move selected task/project down or up in its manual order |
| `b` | Toggle sidebar |
| `n` | New task (centered floating form with do date/deadline) |
| `a` / `p` | New area / project |
| `N` | New heading while inside a project |
| `Enter` | Task details (centered floating window) |
| `e` | Edit title, description, tags, dates, area, and project |
| `x` | Mark complete |
| `T` | Filter visible tasks by comma-separated tags (all tags must match) |
| `f` | Fuzzy-style jump to a list, area, project, or hidden view |
| `A` | Toggle area/project grouping mode |
| `d` | Permanently delete an archived task/project/area |
| `?` | Show shortcuts |
| `q` | Quit |

## Notes

Stride deliberately uses your terminal’s default background so it belongs with Ghostty, Kitty, and any theme you choose. For the small symbolic glyphs in the sidebar, set your terminal font to **JetBrainsMono Nerd Font**.

Projects are grouped by area and appear in the sidebar as soon as you create them.

## Dates, lifecycle, and hidden views

Use `YYYY-MM-DD` for a **do date** or **deadline**. Items with either date today or overdue surface in Today; Upcoming groups unscheduled future work by do date. A task whose do date is `someday` is dimmed in Someday and stays outside everyday views. Inbox is reserved for unscheduled, unassigned captures.

`f` also exposes **Tomorrow**, **Deadlines**, **Logged Projects**, and **Archived Areas** without crowding the sidebar. Completion timestamps have second/fraction precision in SQLite and Logbook sorts newest first. Projects and areas can be completed, cancelled, or permanently deleted from their lifecycle dialog; deletion asks for confirmation. Archived task cleanup is deliberately immediate with `d`.
