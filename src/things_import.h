#pragma once
// One-time (but safely re-runnable) import from a Things 3 `main.sqlite`
// export into a Stride Store. Matches by the source uuid stored on each
// row, so re-running the import updates existing items in place instead of
// duplicating them.

#include <stdexcept>
#include <string>
#include <vector>

class Store;

struct ImportStats {
  int areas = 0, projects = 0, headings = 0, tasks = 0;
  std::vector<std::string> warnings;  // per-row failures (e.g. name collisions); import continues past these
};

// `sqlitePath` should point at the Things `main.sqlite` file. If sibling
// `<path>-wal` / `<path>-shm` files exist alongside it, they're used too
// (Things keeps recent changes there until its next checkpoint). Throws
// std::runtime_error if the file can't be opened or doesn't look like a
// Things database.
ImportStats importThings(Store& store, const std::string& sqlitePath);
