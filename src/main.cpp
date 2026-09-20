#include <cstring>
#include <filesystem>
#include <iostream>

#include "app.h"
#include "config.h"
#include "store.h"
#include "sync.h"
#include "things_import.h"
#include "util.h"

namespace {

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

int runSync() {
  auto dir = dataDir();
  Config config((dir / "config").string());
  GitSync sync(dir, config);
  if (sync.configuredRemote().empty()) {
    std::cerr << "stride --sync: no mirror remote configured (set mirror_remote in "
              << (dir / "config").string() << ", or run `stride` once interactively to be prompted, "
              << "or declare it via the home-manager module on NixOS)\n";
    return 1;
  }
  Store store((dir / "stride.db").string());
  auto outcome = sync.sync(store);
  std::cout << "stride --sync: " << outcome.message << '\n';
  return (outcome.changed && !outcome.pushed) ? 1 : 0;  // committed-but-unpushed is worth a nonzero exit for cron/systemd logs
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

}  // namespace

int main(int argc, char** argv) {
  try {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--sync") == 0) return runSync();
      if (std::strcmp(argv[i], "--import-things") == 0 && i + 1 < argc) return runImportThings(argv[i + 1]);
    }

    auto dir = dataDir();
    std::filesystem::create_directories(dir);
    Store store((dir / "stride.db").string());

    Config config((dir / "config").string());
    GitSync sync(dir, config);
    if (sync.configuredRemote().empty()) {
      auto url = promptForRemote(sync);
      if (!url.empty()) sync.setRemote(url);
    }

    App(store).run();
  } catch (const std::exception& e) {
    std::cerr << "stride: " << e.what() << '\n';
    return 1;
  }
}
