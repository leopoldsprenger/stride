#include <cstring>
#include <filesystem>
#include <iostream>

#include "app.h"
#include "cli_json.h"
#include "gui.h"
#include "config.h"
#include "crypto.h"
#include "mirror.h"
#include "quickcapture.h"
#include "store.h"
#include "sync.h"
#include "things_import.h"
#include "util.h"

namespace {

void printGeneratedKey(const std::string& hex) {
  std::cout << "\n"
               "A new mirror encryption key was generated: this is what keeps your data\n"
               "unreadable to GitHub (or wherever the mirror remote lives) -- everything\n"
               "pushed there is encrypted with it, and it never leaves this config file.\n\n"
               "    "
            << hex
            << "\n\n"
               "Copy this to every other device you'll run Stride on (e.g. via the\n"
               "home-manager module's mirrorEncryptionKey option on NixOS, or by copying\n"
               "it into their config file's mirror_key= line directly). Without it, other\n"
               "devices can't decrypt what this one pushes. This is shown once.\n\n";
}

// Prompts on stdin/stdout (before ncurses starts) for the mirror's git
// remote, validating each answer before accepting it -- so Stride never
// ends up pushing into an unrelated repository. Only reachable when
// running the interactive TUI; --sync has no one to ask and errors out
// instead if unconfigured.
std::string promptForRemote(GitSync& sync) {
  std::cout << "No mirror git remote is configured yet.\n"
               "Enter the SSH URL of an empty repo, or one that already holds a Stride mirror\n"
               "(git@github.com:you/your-repo.git), or leave blank to skip syncing for now:\n> ";
  std::string url;
  std::getline(std::cin, url);
  url = trimmed(url);
  if (url.empty()) return "";
  std::cout << "Checking...\n";
  auto v = sync.validateRemote(url);
  if (!v.ok) {
    std::cout << "That repo was refused: " << v.reason << "\nTry again, or leave blank to skip.\n";
    return promptForRemote(sync);
  }
  std::cout << v.reason << '\n';
  return url;
}

// `--sync` (pull then push), `--pull` (remote -> local only), `--push` (local -> remote only). Same exit-code
// contract for all three so timers and scripts can treat them alike.
int runSync(SyncMode mode, const char* flag) {
  auto dir = dataDir();
  Config config((dir / "config").string());
  GitSync sync(dir, config);
  if (sync.configuredRemote().empty()) {
    std::cerr << "stride " << flag << ": no mirror remote configured (set mirror_remote in "
              << (dir / "config").string() << ", or run `stride` once interactively to be prompted, "
              << "or declare it via the home-manager module on NixOS)\n";
    return 1;
  }
  std::filesystem::create_directories(dir);
  Store store((dir / "stride.db").string());
  auto outcome = sync.sync(store, mode);
  if (outcome.keyJustGenerated) printGeneratedKey(outcome.generatedKeyHex);
  std::cout << "stride " << flag << ": " << outcome.message << '\n';
  if (outcome.skipped) return 0;  // another sync is mid-flight; not an error, the next run catches up
  if (outcome.fetchFailed && mode == SyncMode::PullOnly) return 1;
  if (outcome.pushRejected) return 1;
  return (outcome.changed && !outcome.pushed) ? 1 : 0;  // committed-but-unpushed is worth a nonzero exit for cron/systemd logs
}

// Which front end a bare `stride` opens. Precedence: explicit --gui/--tui flag, then $STRIDE_INTERFACE
// (the home-manager module's `programs.stride.interface` sets this in the launcher it installs), then the TUI.
enum class Interface { Tui, Gui };
Interface chooseInterface(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--gui") == 0) return Interface::Gui;
    if (std::strcmp(argv[i], "--tui") == 0) return Interface::Tui;
  }
  if (const char* env = std::getenv("STRIDE_INTERFACE")) {
    if (std::strcmp(env, "gui") == 0) return Interface::Gui;
  }
  return Interface::Tui;
}

int runImportThings(const std::string& path) {
  auto dir = dataDir();
  std::filesystem::create_directories(dir);
  Store store((dir / "stride.db").string());
  try {
    auto stats = importThings(store, path);
    std::cout << "Imported " << stats.areas << " area(s), " << stats.projects << " project(s), " << stats.headings
              << " heading(s), " << stats.tasks << " task(s).\n";
    if (!stats.warnings.empty()) {
      std::cout << stats.warnings.size() << " item(s) skipped or had issues:\n";
      for (auto& w : stats.warnings) std::cout << "  - " << w << '\n';
    }
  } catch (const std::exception& e) {
    std::cerr << "stride --import-things: " << e.what() << '\n';
    return 1;
  }
  return 0;
}

// Plain-text export for humans -- the counterpart to the encrypted git
// mirror, which is deliberately unreadable. Always renders straight from
// the local database (the actual source of truth), not from whatever's
// currently sitting in the git checkout, so it's accurate even if you
// haven't synced recently. Skips the marker file and README.md, which
// only make sense inside the git mirror itself.
int runQuickCaptureCmd(bool forceTui) {
  auto dir = dataDir();
  std::filesystem::create_directories(dir);
  Store store((dir / "stride.db").string());
  return runQuickCapture(store, forceTui);
}

// `--json <view> [arg]`, `--complete <id> [--kind t|p]`, `--quick-add
// <title> [--list <name>] [--date <date>]` -- the scriptable surface used
// by scripts and status bars (anything that'd rather shell out than link
// against Stride). See cli_json.h for what each view prints.
int runJsonCmd(int argc, char** argv, int start) {
  std::string view = start < argc ? argv[start] : "";
  std::string arg = (start + 1 < argc) ? argv[start + 1] : "";
  auto dir = dataDir();
  Store store((dir / "stride.db").string());
  return runJson(store, view, arg);
}

int runCompleteCmd(int argc, char** argv, int start) {
  char kind = 't';
  int id = 0;
  bool haveId = false;
  for (int j = start; j < argc; ++j) {
    std::string a = argv[j];
    if (a == "--kind" && j + 1 < argc)
      kind = argv[++j][0];
    else if (!haveId) {
      id = std::atoi(a.c_str());
      haveId = true;
    }
  }
  auto dir = dataDir();
  Store store((dir / "stride.db").string());
  return runComplete(store, id, kind);
}

int runQuickAddCmd(int argc, char** argv, int start) {
  std::string title, list, date;
  bool haveTitle = false;
  for (int j = start; j < argc; ++j) {
    std::string a = argv[j];
    if (a == "--list" && j + 1 < argc)
      list = argv[++j];
    else if (a == "--date" && j + 1 < argc)
      date = argv[++j];
    else if (!haveTitle) {
      title = a;
      haveTitle = true;
    }
  }
  auto dir = dataDir();
  std::filesystem::create_directories(dir);
  Store store((dir / "stride.db").string());
  return runQuickAdd(store, title, list, date);
}

int runDump(const std::string& path) {
  auto dir = dataDir();
  Store store((dir / "stride.db").string());
  std::filesystem::path outDir(path);
  MirrorExporter(store).exportTo(outDir);
  std::error_code ec;
  std::filesystem::remove(outDir / MirrorExporter::kMarkerFile, ec);
  std::filesystem::remove(outDir / "README.md", ec);
  std::cout << "Dumped to " << outDir.string() << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--sync") == 0) return runSync(SyncMode::Both, "--sync");
      if (std::strcmp(argv[i], "--pull") == 0) return runSync(SyncMode::PullOnly, "--pull");
      if (std::strcmp(argv[i], "--push") == 0) return runSync(SyncMode::PushOnly, "--push");
      if (std::strcmp(argv[i], "--import-things") == 0 && i + 1 < argc) return runImportThings(argv[i + 1]);
      if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) return runDump(argv[i + 1]);
      if (std::strcmp(argv[i], "--quick-capture") == 0) {
        // --tui may appear before or after --quick-capture; either order
        // forces the ncurses dialog even when Stride was built with GTK4
        // (e.g. for testing, or a keybinding meant to run inside a terminal
        // deliberately). Without it, the GTK4 window is used when available
        // and there's a display to show it on; see quickcapture.cpp.
        bool forceTui = false;
        for (int j = 1; j < argc; ++j) forceTui |= std::strcmp(argv[j], "--tui") == 0;
        return runQuickCaptureCmd(forceTui);
      }
      if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) return runJsonCmd(argc, argv, i + 1);
      if (std::strcmp(argv[i], "--complete") == 0 && i + 1 < argc) return runCompleteCmd(argc, argv, i + 1);
      if (std::strcmp(argv[i], "--quick-add") == 0 && i + 1 < argc) return runQuickAddCmd(argc, argv, i + 1);
    }

    auto dir = dataDir();
    std::filesystem::create_directories(dir);
    Store store((dir / "stride.db").string());

    if (chooseInterface(argc, argv) == Interface::Gui) {
      // The GUI never prompts on a terminal (it may not have one): a missing remote is surfaced in its sync
      // dialog instead, and key generation is left to the first sync. If there's no display, or this build has
      // no GTK4, it says so and we carry on into the TUI rather than failing.
      int rc = runGui(store);
      if (rc != kGuiUnavailable) return rc;
      std::cerr << "stride: falling back to the terminal interface\n";
    }

    Config config((dir / "config").string());
    GitSync sync(dir, config);
    if (sync.configuredRemote().empty()) {
      auto url = promptForRemote(sync);
      if (!url.empty()) sync.setRemote(url);
    }
    if (!sync.configuredRemote().empty()) {
      auto ks = sync.ensureKey();
      if (ks.justGenerated) printGeneratedKey(keyToHex(ks.key));
    }

    App(store).run();
  } catch (const std::exception& e) {
    std::cerr << "stride: " << e.what() << '\n';
    return 1;
  }
}
