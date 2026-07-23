# Stride

Stride is a quiet, keyboard-first GTD console for people who want their next action close at hand. Its shape borrows from Things: capture into **Inbox**, focus **Today**, plan **Upcoming**, browse **Anytime** and **Someday**, and keep completions in **Logbook**.

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
| `j` / `k`, `↑` / `↓` | Select task |
| `h` / `l`, `←` / `→` | Switch list |
| `b` | Toggle sidebar |
| `n` | New task (centered floating form) |
| `p` | New project (centered floating form) |
| `Enter` | Task details (centered floating window) |
| `e` | Edit title, description, tags, list, and project |
| `x` | Mark complete |
| `m` | Move to Inbox, Today, Upcoming, Anytime, or Someday |
| `?` | Show shortcuts |
| `q` | Quit |

## Notes

Stride deliberately uses your terminal’s default background so it belongs with Ghostty, Kitty, and any theme you choose. For the small symbolic glyphs in the sidebar, set your terminal font to **JetBrainsMono Nerd Font**.

Projects are grouped by area and appear in the sidebar as soon as you create them.
