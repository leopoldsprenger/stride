#pragma once
// The other half of mirror.cpp: parses the Markdown+frontmatter mirror
// tree back into Store operations. Only ever called on content that just
// came from `git reset --hard origin/<branch>` (see sync.cpp) -- i.e.
// something another device committed and pushed -- never on a live local
// checkout that might have uncommitted hand edits sitting in it.
//
// Matching is by each item's stride_uuid (embedded as YAML frontmatter for
// areas/projects, and as an invisible `<!-- uuid:... -->` HTML comment for
// tasks/headings), not by title or position, so this is safe to run
// repeatedly and stays correct even if titles were renamed on the far end.

#include <filesystem>

class Store;

struct ReconcileStats {
  int areas = 0, projects = 0, headings = 0, tasks = 0;
};

ReconcileStats reconcileFromMirror(Store& store, const std::filesystem::path& mirrorDir);
