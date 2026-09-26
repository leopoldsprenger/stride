# Stride

Stride is a quiet, keyboard-first GTD console for people who want their next action close at hand. Its shape borrows from Things: capture into **Inbox**, focus **Today**, plan **Upcoming**, browse **Anytime** and **Someday**, and keep completions in **Logbook**. Areas hold standalone tasks and projects; projects are richer lists with a description, dates, headings, and a lifecycle.

![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white) ![SQLite](https://img.shields.io/badge/data-SQLite-003B57?logo=sqlite&logoColor=white)

## Install

Requirements: CMake 3.20+, a C++23 compiler, `ncursesw`, `sqlite3`, and `openssl` (for the encrypted git mirror -- see [Sync & encryption](#sync--encryption)).

```sh
# macOS (Homebrew)
brew install cmake ncurses sqlite openssl git

# Debian/Ubuntu
sudo apt install cmake g++ libncursesw5-dev libsqlite3-dev libssl-dev pkg-config git
```

On macOS, Homebrew's `openssl` is keg-only, so `cmake` may need a nudge to find it:

```sh
export PKG_CONFIG_PATH="$(brew --prefix openssl)/lib/pkgconfig:$PKG_CONFIG_PATH"
```

Then, on any platform:

```sh
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

The database lives in an OS-appropriate location: `~/Library/Application Support/stride/stride.db` on macOS, `$XDG_DATA_HOME/stride/stride.db` (or `~/.local/share/stride/stride.db`) on Linux. Override either with `STRIDE_DATA_DIR`. It uses SQLite's WAL mode -- compact, durable, and easy to back up or inspect. It's always the source of truth; everything below is built on top of it, never instead of it.

### NixOS / home-manager

`flake.nix` builds the package (`nix build`, or `nix run`), and `homeManagerModules.default` (`nix/home-manager-module.nix`) gives you `programs.stride` for declarative setup, including a systemd user timer for periodic sync. See the comment at the top of that file for a usage example. On other Nix setups, `nix/package.nix` is a plain `callPackage`-able derivation.

## Sync & encryption

Stride can mirror your data to a git remote (GitHub or otherwise) for backup and for syncing between devices. The local SQLite database is always the real source of truth -- the mirror is a derived, disposable view of it. Everything is automatic once set up: no manual export/import, no merge conflicts to resolve by hand.

**Setup.** The first time you run `stride` with no mirror configured, it asks for a git URL (an empty repo, or one that already holds a Stride mirror -- anything else is refused, so Stride can never push into an unrelated repo) and generates a random encryption key, shown once. On NixOS, both are set declaratively instead, via `programs.stride.mirrorRemote` and `programs.stride.mirrorEncryptionKey`.

**Encryption.** Everything that reaches the remote is encrypted with AES-256-GCM before it's ever written to the git checkout -- not just task content but filenames and directory structure too, which is why: the whole rendered tree is bundled into one blob and encrypted as a single opaque file (`content.enc`). GitHub (or anyone else with access to the remote) sees one binary file that changes size over time and nothing else -- no titles, no notes, no counts, no structure. The trade-off is that git diffs on the remote are meaningless ("binary file differs") -- once the goal is "GitHub can't see anything," a meaningful diff would itself be a leak. The key lives only in local config (`mirror_key` in the `config` file next to `stride.db`) and is never committed; every device that syncs to the same remote needs the same key, copied over by hand (or declared identically via home-manager, ideally sourced from a secrets tool like `sops-nix` rather than written directly into your Nix config -- see the option's doc comment). If the key is wrong, `--sync` fails loudly rather than silently discarding what it couldn't read.

**Syncing.** `stride --sync` is a separate, non-interactive command meant to be run on a timer (the NixOS module sets one up automatically; on macOS, use `launchd` or `cron`) -- it exports, and if anything actually changed, commits and pushes; if another device pushed first, it pulls their changes into your local database before pushing yours. It's a no-op otherwise, so running it often is fine.

**Reading it.** Since the remote is opaque by design, `stride --dump <path>` renders your current data as plain, human-readable Markdown into a directory you choose -- the same layout the encrypted mirror uses internally, minus the repo marker file and the summary README, which only make sense inside the mirror itself. Handy for grepping your own tasks, or just reassuring yourself about what's actually in there.

**One-time Things import.** `stride --import-things /path/to/main.sqlite` migrates a Things 3 export -- areas, projects, headings, tasks, tags, checklists, and status all map across. Matched by a stable ID embedded in each imported row, so it's safe to re-run (updates in place, never duplicates). If your Things database is in WAL mode (it usually is), bring the `.sqlite-wal`/`.sqlite-shm` files along next to it, or very recent Things changes may be missing.

## Source layout

```
src/util.h          data structs (Item, Ref, ChecklistItem) + pure-function helpers
src/input.*         raw-escape-safe key reader; single-line and multi-line field editors
src/form.*          dialog window + multi-field form with Tab/Shift+Tab navigation
src/store.*         all SQLite access
src/finder.*        fuzzy matching + the live-filter popups (find, move, tag picker, actions menu)
src/app.*           view logic, rendering, mouse + keyboard input dispatch
src/mirror.*        renders the database to human-readable Markdown+frontmatter
src/mirror_import.* parses that Markdown back into the database (multi-device reconciliation)
src/bundle.*         packs/unpacks a directory tree into one blob, for the encrypted mirror
src/crypto.*         AES-256-GCM, via OpenSSL -- everything pushed to the git mirror
src/config.*        tiny key=value config file (mirror_remote, mirror_key)
src/sync.*          git plumbing: clone/validate/commit/push, encrypt/decrypt, reconcile
src/things_import.* one-time migration from a Things 3 database
src/main.cpp        entry point + --sync / --import-things / --dump
```

## Keyboard

| Key | Action |
| --- | --- |
| `j`/`k`, `↑`/`↓` | Move the selection |
| `h`/`l`, `←`/`→`, `Esc` | Switch sidebar list; inside a project/area, steps back out instead |
| `J`/`K` | Reorder the selected task or project; dragging a task past a heading moves it into that section |
| `b` | Toggle the sidebar |
| `n` | New... -- Task / Project / Area, plus Heading when you're inside a project |
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

## Mouse

Everything above also works with the mouse, so there's nothing above that's *required* memorizing: click a sidebar entry (a Focus view, an area, a project) to jump straight to it; click a row to select it, click it again to open/edit it; right-click any row -- in the sidebar or the main list -- for a contextual actions menu (complete, edit, move, tag, delete, whatever applies).

## Notes

Stride uses your terminal's default background so it belongs with Ghostty, Kitty, or whatever theme you run. Set your terminal font to **JetBrainsMono Nerd Font** for the sidebar glyphs.

Each task and project row shows small right-aligned indicators: a flag with its date for a deadline (red once due), a circle with its date for a do date, and single-glyph markers for a non-empty description, a checklist, and tags -- so you can tell what's on a task without opening it. A task's checklist is a simple add/check/delete list edited with `c`; it never appears in list views itself, only its indicator does.

## Dates, lifecycle, and hidden views

Use `YYYY-MM-DD` for a **do date** or **deadline**, or type `someday` as the do date. Items due or overdue surface automatically in Today; Upcoming groups future work by do date; Anytime shows everything open except Someday (including what's due today or later). Someday tasks are dimmed everywhere they appear. Inbox holds only unscheduled, unassigned captures -- give a task a do date, area, or project and it moves on.

An area lists its open projects (by do date), then its own standalone tasks split into **Tasks** (no do date) and **Scheduled**, then **Someday**. A project's own description sits at the top of its page whether it's active, completed, or cancelled. Its tasks sort by do date within whatever headings you've created (`N`), with ungrouped tasks first and heading membership shown by indentation.

In Today/Tomorrow/Upcoming, a project and its tasks share one freely-reorderable list: if every open task in a project is due the same day as the project itself, only the project is shown; otherwise the specific tasks that match are shown alongside it. Turning on grouping (`A`) sorts tasks with no area or project to the very top, then by area, then by project underneath it -- projects show under their area by name alone, not "Area / Project".

`f` also reaches **Tomorrow**, **Deadlines**, **Logged Projects**, and **Archived Areas** without crowding the sidebar -- and, unlike a plain jump list, it fuzzy-matches every area, project, and open task too, taking you straight to wherever that item lives. Logbook and Logged Projects sort by completion time (tracked to the second) and group by Today, Yesterday, month (this year), or year; `u` brings anything back from any of these views.
