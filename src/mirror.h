#pragma once
// Renders the whole database as human-readable Markdown+YAML-frontmatter
// files under a target directory, for git-based versioning/backup. This is
// a *derived* view: SQLite stays the single source of truth, so every run
// wipes and fully regenerates the content/ subtree rather than patching it.
// The caller (sync.cpp) is what turns that into git commits.

#include <filesystem>

class Store;

class MirrorExporter {
 public:
  explicit MirrorExporter(Store& store) : store_(store) {}

  // Writes mirror/.stride-mirror-repo, mirror/README.md, and a fully
  // regenerated mirror/content/ tree. Creates the directory if needed.
  void exportTo(const std::filesystem::path& mirrorDir);

  // Name of the marker file GitSync checks for to validate a remote already
  // holds a Stride mirror (as opposed to some unrelated repo).
  static constexpr const char* kMarkerFile = ".stride-mirror-repo";

 private:
  Store& store_;
};
