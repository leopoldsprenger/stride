#pragma once
// Packs a directory tree into a single in-memory blob and back. Used so the
// git mirror's encrypted payload is exactly one opaque file -- no
// filenames, no directory shape, no per-file sizes visible to GitHub, just
// one blob whose size changes over time. That's a deliberate trade against
// the old plaintext mirror's per-file git diffs: once "GitHub must not see
// anything sensitive" is the goal, a meaningful diff (which task changed,
// by how much) is itself a leak, so there's nothing to preserve here.

#include <filesystem>
#include <string>

// Every file under `dir` (recursively), each as
// [4-byte LE path length][path, '/'-separated, relative to dir][8-byte LE
// content length][content bytes], sorted by relative path for a
// deterministic bundle (so re-bundling identical content produces an
// identical blob, which matters once it's encrypted with a fresh nonce
// anyway -- more for testability than for git diffing).
std::string packDir(const std::filesystem::path& dir);

// Inverse of packDir: recreates the files (and their directories) under
// `outDir`, which is created if it doesn't exist. Throws on a malformed
// blob rather than silently extracting a partial/garbage tree.
void unpackToDir(const std::string& blob, const std::filesystem::path& outDir);
