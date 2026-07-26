#pragma once
// Fuzzy matching and the live-filter popup used by 'f' find, 'm' move,
// and the tag filter's tag picker.

#include <string>
#include <vector>

#include "util.h"

struct PickerItem {
  std::string icon;
  std::string label;
  std::string sub;
  char kind = 0;  // caller-defined tag for interpreting the result
  Item raw;
};

// Ordered-subsequence match: -1 if `needle` isn't a subsequence of
// `haystack`, otherwise a score rewarding early, contiguous hits.
int fuzzyScore(const std::string& needle, const std::string& haystack);

// Live-filtering single-select popup: matches/reorders on every keystroke.
// Returns the chosen index into `items`, or -1 on Escape / no match.
int runPicker(const std::string& title, const std::vector<PickerItem>& items);

// Multi-select tag picker. Enter toggles the highlighted tag into/out of the
// comma-separated `selected` list and keeps the popup open; Tab (or Enter on
// the "Done" row) finishes and returns the accumulated selection; Escape
// cancels and returns the original `selected` unchanged. A synthetic "none
// (untagged)" entry is always offered.
std::string runTagPicker(const std::vector<std::string>& existingTags, std::string selected);

std::string friendlyDate(const std::string& iso);
// Buckets a completed_at timestamp into Today / Yesterday / "Month Year"
// (current year) / "Year" (past years), for the Logbook-style views.
std::string bucketLabel(const std::string& completedAtIso);
