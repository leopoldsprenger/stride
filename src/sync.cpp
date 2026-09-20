#include "sync.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <ctime>
#include <sstream>
#include <stdexcept>

#include "config.h"
#include "mirror.h"
#include "mirror_import.h"
#include "store.h"

extern char** environ;

namespace fs = std::filesystem;

// git command output always ends in '\n' (sometimes more, on errors) --
// trimmed() in util.h only strips spaces, so a ref name or count parsed
// from git's stdout needs its own trailing-whitespace strip or it ends up
// with an embedded newline (which silently breaks it as a git ref, or is
// merely cosmetic for a number since std::stoi ignores trailing junk --
// either way, wrong).
static std::string rstrip(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

GitResult runGit(const std::vector<std::string>& args, const fs::path& cwd) {  std::vector<std::string> full = {"git", "-C", cwd.string()};
  full.insert(full.end(), args.begin(), args.end());

  std::vector<char*> argv;
  argv.reserve(full.size() + 1);
  for (auto& s : full) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);

  int outPipe[2];
  if (pipe(outPipe) != 0) return {-1, "pipe() failed"};

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, outPipe[0]);
  posix_spawn_file_actions_addclose(&actions, outPipe[1]);

  pid_t pid{};
  int rc = posix_spawnp(&pid, "git", &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(outPipe[1]);
  if (rc != 0) {
    close(outPipe[0]);
    return {-1, std::string("failed to start git: ") + std::strerror(rc)};
  }

  std::string output;
  std::array<char, 4096> buf{};
  ssize_t n;
  while ((n = read(outPipe[0], buf.data(), buf.size())) > 0) output.append(buf.data(), n);
  close(outPipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, output};
}

GitSync::GitSync(fs::path dataDir, Config& config) : dataDir_(std::move(dataDir)), config_(config) {}

std::string GitSync::configuredRemote() const { return config_.get("mirror_remote").value_or(""); }

void GitSync::setRemote(const std::string& url) { config_.set("mirror_remote", url); }

GitSync::Validation GitSync::validateRemote(const std::string& url) {
  if (url.empty()) return {false, "empty URL"};

  auto heads = runGit({"ls-remote", "--heads", url});
  if (!heads.ok()) {
    auto trimmed_out = heads.output;
    while (!trimmed_out.empty() && (trimmed_out.back() == '\n' || trimmed_out.back() == '\r')) trimmed_out.pop_back();
    return {false, "could not reach that remote (" + trimmed_out + ")"};
  }
  if (heads.output.empty()) return {true, "empty repository -- will be initialized"};

  // Non-empty remote: it must already be a Stride mirror. Do a throwaway
  // shallow clone into a temp dir and check for the marker file rather
  // than `git archive --remote`, which GitHub and most hosts disable.
  auto tmp = fs::temp_directory_path() / ("stride-validate-" + std::to_string(::getpid()) + "-" + std::to_string(std::time(nullptr)));
  std::error_code ec;
  fs::create_directories(tmp, ec);
  auto clone = runGit({"clone", "--depth", "1", "--quiet", url, "."}, tmp);
  bool hasMarker = clone.ok() && fs::exists(tmp / MirrorExporter::kMarkerFile);
  fs::remove_all(tmp, ec);
  if (!clone.ok()) return {false, "could not clone that remote to inspect it"};
  if (!hasMarker) return {false, "that repository has commits but isn't a Stride mirror (no " + std::string(MirrorExporter::kMarkerFile) + ")"};
  return {true, "existing Stride mirror -- will sync into it"};
}

bool GitSync::ensureCheckout() {
  auto remote = configuredRemote();
  if (remote.empty()) throw std::runtime_error("no mirror remote configured (set mirror_remote in config, or via the home-manager module)");
  auto dir = mirrorDir();
  if (fs::exists(dir / ".git")) return false;
  if (fs::exists(dir) && !fs::is_empty(dir)) throw std::runtime_error(dir.string() + " exists and isn't a git checkout -- refusing to touch it");

  // First-ever use of this remote on this device: validate it here too, not
  // just in the interactive prompt (promptForRemote in main.cpp) -- this is
  // also reached headlessly from `stride --sync` when mirror_remote came
  // from a declarative home-manager config rather than a live prompt, and
  // that path needs the same "empty repo or existing Stride mirror, nothing
  // else" refusal.
  auto v = validateRemote(remote);
  if (!v.ok) throw std::runtime_error("configured mirror_remote was refused: " + v.reason);

  fs::create_directories(dir.parent_path());
  auto clone = runGit({"clone", "--quiet", remote, dir.string()}, dir.parent_path());
  if (!clone.ok()) throw std::runtime_error("git clone failed: " + clone.output);
  if (runGit({"rev-parse", "--verify", "--quiet", "HEAD"}, dir).ok()) return true;  // normal case: clone checked out fine

  // HEAD didn't resolve. Either the remote is genuinely empty (no commits
  // at all -- start a fresh `main`), or it has commits but its default
  // branch pointer is stale/missing (rare, but seen from repos created by
  // tools other than GitHub) -- in that case, track whatever branch
  // actually exists rather than starting an orphan history that could
  // never fast-forward-push against the real one.
  auto refs = runGit({"for-each-ref", "--format=%(refname:short)", "refs/remotes/origin"}, dir);
  std::string chosen;
  std::istringstream iss(refs.output);
  for (std::string line; std::getline(iss, line);) {
    auto slash = line.find('/');
    std::string name = slash == std::string::npos ? line : line.substr(slash + 1);
    if (name.empty() || name == "HEAD") continue;
    if (chosen.empty()) chosen = name;
    if (name == "main" || name == "master") { chosen = name; break; }
  }
  if (!chosen.empty()) runGit({"checkout", "--quiet", "-b", chosen, "origin/" + chosen}, dir);
  else runGit({"checkout", "--quiet", "-b", "main"}, dir);
  return true;
}

SyncOutcome GitSync::sync(Store& store) {
  auto dir = mirrorDir();
  bool freshClone = ensureCheckout();

  SyncOutcome out;
  ReconcileStats rstats;

  if (freshClone) {
    // Whatever we just cloned is content from another device (or a truly
    // empty repo) that this database has never seen -- reconcile it in
    // before we ever export, or the export below would overwrite the
    // checkout with this (empty, brand-new) database's view and push that
    // over the real data.
    rstats = reconcileFromMirror(store, dir);
  } else {
    // Multi-device sync: if another machine has pushed since our last
    // sync, adopt its state wholesale (the mirror is always fully
    // regenerated anyway, so there's nothing meaningful to merge textually
    // -- see the module comment in mirror_import.h) and reconcile it into
    // our local database before we compute our own changes on top of it.
    auto fetch = runGit({"fetch", "--quiet", "origin"}, dir);
    if (fetch.ok()) {
      auto branchRes = runGit({"rev-parse", "--abbrev-ref", "HEAD"}, dir);
      std::string branch = rstrip(branchRes.output);
      if (!branch.empty()) {
        auto behindRes = runGit({"rev-list", "--count", "HEAD..origin/" + branch}, dir);
        if (behindRes.ok()) {
          int behindCount = 0;
          try {
            behindCount = std::stoi(rstrip(behindRes.output));
          } catch (...) {
          }
          if (behindCount > 0) {
            runGit({"reset", "--quiet", "--hard", "origin/" + branch}, dir);
            rstats = reconcileFromMirror(store, dir);
          }
        }
      }
    }
  }

  MirrorExporter(store).exportTo(dir);

  runGit({"add", "-A"}, dir);
  auto diff = runGit({"diff", "--cached", "--quiet"}, dir);
  bool hasStagedChanges = diff.code != 0;

  if (hasStagedChanges) {
    std::time_t t = std::time(nullptr);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    auto commit = runGit({"commit", "--quiet", "-m", std::string("Stride sync: ") + buf}, dir);
    if (!commit.ok()) {
      out.message = "commit failed: " + commit.output;
      return out;
    }
    out.changed = true;
  }

  // Push whenever there's local history the remote doesn't have yet --
  // covers both a fresh commit above and a commit that landed but failed
  // to push on a previous, offline run. `@{u}` only resolves once an
  // upstream is set, which happens on the very first push below. A
  // rejected push (another device won the race since our fetch above)
  // isn't retried here -- the next scheduled sync will fetch, reconcile,
  // and try again, same as any other "we're behind" case.
  auto unpushed = runGit({"log", "@{u}..HEAD", "--oneline"}, dir);
  bool hasUpstream = unpushed.code == 0;
  bool needsPush = hasUpstream ? !unpushed.output.empty() : out.changed;

  if (needsPush) {
    auto push = !hasUpstream ? runGit({"push", "--quiet", "-u", "origin", "HEAD"}, dir)
                              : runGit({"push", "--quiet"}, dir);
    out.pushed = push.ok();
    if (!push.ok()) out.message = "push failed (will retry next sync): " + push.output;
  }

  std::ostringstream summary;
  if (rstats.areas || rstats.projects || rstats.headings || rstats.tasks) {
    summary << "pulled " << rstats.areas << " area(s), " << rstats.projects << " project(s), " << rstats.headings
            << " heading(s), " << rstats.tasks << " task(s) from another device; ";
  }
  summary << (out.changed ? (out.pushed ? "synced" : "committed locally, push pending") : "nothing new locally");
  out.message = out.message.empty() ? summary.str() : summary.str() + " -- " + out.message;
  return out;
}
