# Stride

Stride is a quiet, keyboard-first GTD manager for people who want their next action close at hand, as a terminal app and as a native GTK4 app that share one database. Its shape borrows from Things: capture into **Inbox**, focus **Today**, plan **Upcoming**, browse **Anytime** and **Someday**, and keep completions in **Logbook**. Areas hold standalone tasks and projects; projects are richer lists with a description, dates, headings, and a lifecycle.

![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white) ![SQLite](https://img.shields.io/badge/data-SQLite-003B57?logo=sqlite&logoColor=white)

## Install

Requirements: CMake 3.20+, a C++23 compiler, `ncursesw`, `sqlite3`, and `openssl` (for the encrypted git mirror -- see [Sync & encryption](#sync--encryption)). `gtk4` is optional but picked up automatically if present: with it you get the full GTK4 app (`stride --gui`, see [The GTK4 app](#the-gtk4-app)) and `stride --quick-capture` opens a small floating window instead of needing a dedicated floating terminal for the ncurses dialog -- see [Quick capture](#quick-capture). Pass `-DSTRIDE_NO_GTK4=ON` to `cmake` to build without it even when `gtk4` is installed.

```sh
# macOS (Homebrew)
brew install cmake ncurses sqlite openssl gtk4 git

# Debian/Ubuntu
sudo apt install cmake g++ libncursesw5-dev libsqlite3-dev libssl-dev libgtk-4-dev pkg-config git
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

`flake.nix` builds the package (`nix build`, `nix run`, or `nix run .#gui`), and `homeManagerModules.default` (`nix/home-manager-module.nix`) gives you `programs.stride` for declarative setup, including a systemd user timer for periodic sync that's installed, enabled, *and* started automatically the first time `home-manager switch` turns it on -- no extra login needed. See the comment at the top of that file for a usage example, and `enableSyncTimer`'s doc comment for the one-line NixOS system option (`users.users.<you>.linger`) that starts it at boot on a box you don't interactively log into. On other Nix setups, `nix/package.nix` is a plain `callPackage`-able derivation.

**One toggle for TUI vs GUI.** Nothing is removed either way; this only picks what a bare `stride` opens:

```nix
programs.stride = {
  enable = true;
  interface = "gui";            # "tui" (default) | "gui"
  gui.theme = "auto";           # "auto" follows the desktop live | "light" | "dark"
  gui.accent = "#bb9af7";       # optional #rrggbb; null = built-in blue
  gui.decorations = false;      # a clean undecorated window (default)
  syncDirection = "both";       # what the timer does: "both" | "pull" | "push"
};
```

`stride --tui` / `stride --gui` (and `stride-tui` / `stride-gui`) always work whatever you pick, so you can flip
for a single session without a rebuild. If your config consumes `nix/package.nix` directly instead of the module,
the same toggle is a package argument: `stride.override { defaultInterface = "gui"; }`. The settings are baked in
as *defaults* (`STRIDE_INTERFACE`, `STRIDE_THEME`, `STRIDE_ACCENT`, `STRIDE_DECORATIONS`), so an environment
variable or flag at runtime still overrides them. With no display (ssh, a tty) or a build without GTK4, `--gui`
says so and `stride` carries on in the terminal.

## Sync & encryption

Stride can mirror your data to a git remote (GitHub or otherwise) for backup and for syncing between devices. The local SQLite database is always the real source of truth -- the mirror is a derived, disposable view of it. Everything is automatic once set up: no manual export/import, no merge conflicts to resolve by hand.

**Setup.** The first time you run `stride` with no mirror configured, it asks for a git URL (an empty repo, or one that already holds a Stride mirror -- anything else is refused, so Stride can never push into an unrelated repo) and generates a random encryption key, shown once. On NixOS, both are set declaratively instead, via `programs.stride.mirrorRemote` and `programs.stride.mirrorEncryptionKey`.

**Encryption.** Everything that reaches the remote is encrypted with AES-256-GCM before it's ever written to the git checkout -- not just task content but filenames and directory structure too, which is why: the whole rendered tree is bundled into one blob and encrypted as a single opaque file (`content.enc`). GitHub (or anyone else with access to the remote) sees one binary file that changes size over time and nothing else -- no titles, no notes, no counts, no structure. The trade-off is that git diffs on the remote are meaningless ("binary file differs") -- once the goal is "GitHub can't see anything," a meaningful diff would itself be a leak. The key lives only in local config (`mirror_key` in the `config` file next to `stride.db`) and is never committed; every device that syncs to the same remote needs the same key, copied over by hand (or declared identically via home-manager, ideally sourced from a secrets tool like `sops-nix` rather than written directly into your Nix config -- see the option's doc comment). If the key is wrong, `--sync` fails loudly rather than silently discarding what it couldn't read.

**Syncing.** Three non-interactive commands, all safe to run often and from a timer (the NixOS module sets one up automatically; on macOS, use `launchd` or `cron`):

| Command | Direction |
| --- | --- |
| `stride --sync` | Both: pulls other devices' changes into your local database, then commits and pushes yours. A no-op when nothing changed. |
| `stride --pull` | Remote → local only. Fetches and reconciles; never exports, commits or pushes. |
| `stride --push` | Local → remote only. Never pulls; if another device pushed first, the push is refused (non-zero exit, "pull first") and nothing is overwritten. |

Only one sync runs at a time per data directory (a lock file), so the timer and a GUI-triggered sync can never collide -- the second one just reports that another is running. A first sync on a new device always pulls first, whichever command you used, so an empty database can never be pushed over real data. If git has no committer identity configured (a fresh machine, a bare systemd service), Stride commits as `Stride <stride@localhost>` rather than failing.

**Reading it.** Since the remote is opaque by design, `stride --dump <path>` renders your current data as plain, human-readable Markdown into a directory you choose -- the same layout the encrypted mirror uses internally, minus the repo marker file and the summary README, which only make sense inside the mirror itself. Handy for grepping your own tasks, or just reassuring yourself about what's actually in there.

**One-time Things import.** `stride --import-things /path/to/main.sqlite` migrates a Things 3 export -- areas, projects, headings, tasks, tags, checklists, and status all map across. Matched by a stable ID embedded in each imported row, so it's safe to re-run (updates in place, never duplicates). If your Things database is in WAL mode (it usually is), bring the `.sqlite-wal`/`.sqlite-shm` files along next to it, or very recent Things changes may be missing.

## The GTK4 app

`stride --gui` opens a native window over the same database as the TUI -- same lists, same projects, areas and
headings, same sync. Both can be open at once: the window notices writes from the TUI, quick capture, the bar widget
and background syncs within a moment and refreshes itself.

It is built to be driven from the keyboard and to stay out of the way: a quiet sidebar, one column of to-dos,
motion only where it means something. Checking a to-do off draws the check, strikes the title through, lets the row
rest for a beat so you can see it land (or `z` it back), then folds it away and moves the cursor to the next one, so
a run of `x` sweeps down your list. There's no confetti. New to-dos grow in with a brief highlight.

| Key | Action |
| --- | --- |
| `j`/`k`, `↓`/`↑` | Move the selection (`g`/`G` first/last) |
| `1`-`6` | Inbox, Today, Upcoming, Anytime, Someday, Logbook |
| `h`/`←` | Focus the sidebar: `j`/`k` browse and open, `Enter`/`l`/`Esc` return |
| `Esc` | Back out of a project or area |
| `Ctrl+K`, `/` | Quick find: jump to any list, area, project or to-do, or run a command (new project/area, sync, undo...) |
| `n` | New to-do, opened in place under the selection. Type `@list`, `#tag` and `!when` inline: `Draft abstract @Thesis #writing !fri` |
| `Shift+N` | New heading (in a project) |
| `Space`, `x` | Complete / reopen |
| `z`, `Ctrl+Z` | Undo the last completion |
| `Enter`, `e` | Edit in place; opens a project |
| `s`, `d`, `t`, `m` | When, deadline, tags, move to list |
| `J`/`K` | Reorder |
| `Backspace` | Delete (asks first) |
| `Shift+S` | Sync dialog |
| `b`, `?`, `Ctrl+Q` | Toggle sidebar, shortcuts, quit |

In the editor: `Tab`/`Shift+Tab` walk the fields (title, notes, checklist, when, deadline, tags, list), `Enter` saves
and closes, `Ctrl+Enter` saves and starts the next to-do (so capturing a batch never leaves the keyboard), `Esc`
saves and closes (an empty new to-do is discarded). Dates take `today`, `tomorrow`, `fri`, `+3d`, `2w`, `oct 12`,
`2026-12-24` or `someday`, with a live preview of what was understood. The checklist field takes one item per line,
`[x]` marking it done.

**Syncing from the window.** The sync button in the footer (or `Shift+S`) opens a small dialog showing the remote and
whether it has new changes or this device has unpushed ones (a dot on the button when either is true). It offers
**Pull from remote**, **Push to remote** and **Sync both ways**; choosing one asks you to confirm first, then runs on a
background thread while the icon spins -- typing and navigation never block. When a pull brings in changes, the list
refreshes in place. If a new encryption key had to be generated, it is shown once with a Copy button.

**Look.** Light and dark follow the desktop live (or force one with `STRIDE_THEME=light|dark`); `STRIDE_ACCENT=#rrggbb`
recolours check marks and selection; `STRIDE_DECORATIONS=1` asks for a titlebar. Under NixOS these are
`programs.stride.gui.*`.

**Window rules.** The window's Wayland `app_id` (X11 `WM_CLASS`) is `io.github.leopoldsprenger.stride`, matching the
installed launcher entry. A tiling compositor needs nothing special; to float it instead, with mangowm:

```
windowrule=isfloating:1,appid:^io\.github\.leopoldsprenger\.stride$
```

## Quick capture

`stride --quick-capture` is a standalone three-field dialog (Title, List, Do date) that saves straight to the
Inbox unless you type an existing area or project name into **List** -- it never starts the full app, and exits
the instant you save or hit Escape. It's meant to be bound to a hotkey so it works even when Stride isn't open
anywhere -- this is a different, smaller thing from `f`'s fuzzy-find/actions popup inside the running app.

There are two front ends, sharing the exact same fields, keys, and save logic:

* **GTK4 window** (used automatically when Stride was built with `gtk4` -- see [Install](#install) -- and a
  display is reachable). A small, undecorated, non-resizable window that sizes itself to its own content --
  just the three fields, nothing more -- instead of needing a whole floating terminal for a dialog that only
  fills its middle third. Closes on save, on Escape, **and when it loses focus** (click elsewhere, Alt-Tab
  away), so a stray click can't leave it sitting around.
* **ncurses dialog** (the original front end; still used as a fallback with no `gtk4` at build time, no
  display reachable, or `--tui` passed explicitly), meant to run inside a small floating terminal.

Either way, wire it up with a compositor keybinding and a window rule that floats (and, for the ncurses form,
sizes and centers) that one window.

**mangowm** (`~/.config/mango/config.conf`) -- binds straight to `stride --quick-capture`, no terminal
involved, since the GTK4 window is a normal floating app window in its own right:

```
bind=SUPER+SHIFT,T,spawn,stride --quick-capture
windowrule=isfloating:1,appid:^stride-quick-capture$
```

`isfloating:1` is what takes it out of the tiling layout; mango centers a floating window on the focused
monitor by default (see mango's `no_force_center` window-rule option if you'd rather it opened somewhere
else), sizing it from the window's own requested size since the rule sets no `width`/`height` -- which for
this window is exactly its three fields, however wide that ends up being. `appid` is matched as a regex
against the Wayland `app_id` the GTK4 window sets on itself (`stride-quick-capture`, anchored here with
`^...$` so nothing else can accidentally match it); on X11 the same string goes out as the window's
`WM_CLASS` for window managers that match on that instead. Run `mango -p` (or reload the config) after
adding this to catch typos -- mango validates window rules and reports the file/line of anything it doesn't
recognize.

If you'd rather force the ncurses dialog even on a GTK4 build (e.g. to keep everything inside one terminal
session over SSH), bind to `stride --quick-capture --tui` instead and wire up a floating terminal the way the
niri/Hyprland examples below do -- `--tui` works identically under mango's `windowrule`/`bind` syntax, just
swap the command.

Two examples of the ncurses-dialog-in-a-floating-terminal approach, for compositors without a GTK4 front end
wired up (or when you've forced `--tui`), using [foot](https://codeberg.org/dnkl/foot) as the terminal --
swap in kitty/ghostty/alacritty with `-e` / `--command` as needed:

**niri** (`~/.config/niri/config.kdl`):

```kdl
binds {
    Mod+Shift+A { spawn "foot" "--app-id=stride-quick-capture" "-e" "stride" "--quick-capture" "--tui"; }
}

window-rule {
    match app-id="^stride-quick-capture$"
    open-floating true
    default-column-width { fixed 640; }
    default-window-height { fixed 220; }
}
```

**Hyprland** (`~/.config/hypr/hyprland.conf`):

```ini
bind = SUPER SHIFT, A, exec, foot --app-id=stride-quick-capture -e stride --quick-capture --tui
windowrulev2 = float, class:^(stride-quick-capture)$
windowrulev2 = size 640 220, class:^(stride-quick-capture)$
windowrulev2 = center, class:^(stride-quick-capture)$
```

foot closes itself the instant `stride --quick-capture --tui` exits, so the floating window disappears the
moment you save or cancel -- nothing lingers.

## Scripting (JSON, complete, quick-add)

A few extra flags exist for tools that want Stride's data without a terminal -- this is what the
[Noctalia bar widget](#bar-widget-noctalia) below is built on, and it's a fine base for a keybinding, a status
line, or your own script.

| Command | What it does |
| --- | --- |
| `stride --json today\|upcoming\|inbox\|logbook` | Prints that view as a JSON array of task/project objects (`id`, `kind` (`t`/`p`), `title`, `doDate`, `deadline`, `someday`, `status`, `completedAt`, `areaName`, `projectName`, `tags`, `hasNotes`, `hasChecklist`) |
| `stride --json projects` | Every open project, as `{id, name, areaName}` |
| `stride --json project <name-or-id>` | `{project: {...}, tasks: [...]}` for one project |
| `stride --complete <id> [--kind t\|p]` | Marks a task (default) or project done |
| `stride --quick-add "<title>" [--list <area-or-project>] [--date today\|YYYY-MM-DD]` | Adds a bare task; no `--list`/`--date` means the Inbox, undated |

## Bar widget (Noctalia)

[`integrations/noctalia-plugin/`](integrations/noctalia-plugin/) is a small [Noctalia](https://docs.noctalia.dev)
plugin: a bar icon showing today's item count, and a click-to-open dropdown with a Today/Upcoming/Inbox/Logbook
tab bar, a quick-add field scoped to whatever tab (or project) you're looking at, and tappable rows to check
tasks off or drill into a folded project. It's built on the `--json`/`--complete`/`--quick-add` flags above, so
it needs nothing from Stride except the binary being on `PATH` (already true once installed via home-manager).

```sh
noctalia msg plugins source add stride path ~/path/to/stride/integrations/noctalia-plugin
noctalia msg plugins enable leo/stride
```

(or **Settings → Plugins → Add source** → the same path, then toggle it on). Then add the widget to a bar the
same way as any other, e.g. in your Noctalia `config.toml`:

```toml
[widget.stride]
type = "leo/stride:widget"

[bar.default]
end = ["tray", "stride", "clock"]  # wherever you'd like it among your existing widgets
```

Noctalia's plugin system moves fast -- this targets the current Luau-based plugin API (`plugin_api = 9`) as of
this writing. If something doesn't load, `noctalia msg plugins list` and the shell's own log are the first
places to look; the plugin has no settings of its own; edit `integrations/noctalia-plugin/*.luau` directly for
anything you want to change (the icon, the polling interval, the binary name if `stride` isn't the right one on
`PATH`).

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
src/gui.*, gui_*.h  the GTK4 app: window + views (gui.cpp), palette/CSS (gui_theme.h), cairo icons and the check-off animation (gui_draw.h), date words (gui_dates.h)
src/quickcapture.*  standalone Inbox-prepopulated capture dialog for --quick-capture
src/cli_json.*      --json / --complete / --quick-add, the scripting surface the bar widget uses
src/main.cpp        entry point + --gui / --tui / --sync / --pull / --push / --import-things / --dump / --quick-capture / --json / --complete / --quick-add
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
