#pragma once
// Drives the read-only Markdown mirror + git remote: validating a remote
// URL, cloning/initializing the local mirror checkout, and running the
// export -> commit -> push cycle. Shells out to the system `git` binary
// (no libgit2 dependency, so Nix packaging stays lightweight) via fork/exec
// -- never through a shell, so a remote URL can never be interpreted as
// shell syntax.
//
// Everything that reaches the remote is encrypted (see crypto.h/bundle.h):
// sync() bundles the whole rendered content/ tree into one blob and
// encrypts it with AES-256-GCM before it's ever written into the git
// checkout, so nothing pushed to GitHub (or wherever the remote lives) is
// readable there -- not task titles, not even file/directory names, which
// would otherwise leak through the old per-item filenames. The encryption
// key lives only in local config (mirror_key), never committed.

#include <filesystem>
#include <string>
#include <vector>

#include "crypto.h"
#include "mirror_import.h"

class Store;
class Config;

struct GitResult {
  int code = -1;
  std::string output;  // combined stdout+stderr
  bool ok() const { return code == 0; }
};

// Which halves of a sync cycle to run.
//   Both     -- pull anything the remote has that we don't, then export + commit + push (what `--sync` always did)
//   PullOnly -- fetch and reconcile remote changes into the local database; never export, commit or push
//   PushOnly -- export + commit + push; never fetch or reconcile (a push the remote has moved past is refused,
//               not forced -- see SyncOutcome::pushRejected)
// A first-ever sync on this device always pulls first regardless of mode: pushing over remote content this
// database has never seen is exactly the data loss the mirror is designed to rule out.
enum class SyncMode { Both, PullOnly, PushOnly };

struct SyncOutcome {
  bool changed = false;      // a commit was made
  bool pushed = false;       // the push succeeded (only meaningful if changed, or if a prior commit was still unpushed)
  bool pulled = false;       // remote changes were reconciled into the local database
  ReconcileStats pulledStats;
  bool pushRejected = false; // the remote has commits we don't -- pull first
  bool fetchFailed = false;  // couldn't reach the remote to look for changes (offline, auth, ...)
  bool skipped = false;      // another sync (e.g. the systemd timer) holds the lock; nothing was done
  bool keyJustGenerated = false;
  std::string generatedKeyHex;  // set iff keyJustGenerated -- caller must show this to the user once
  std::string message;          // human-readable summary/error for logging
};

class GitSync {
 public:
  GitSync(std::filesystem::path dataDir, Config& config);

  // Empty remote (no refs yet) or one already holding a Stride mirror
  // (marker file present) -> valid. Anything else -> rejected, so Stride
  // never pushes into an unrelated repo. Network/auth failures are also
  // reported as invalid, with the git error attached.
  struct Validation {
    bool ok = false;
    std::string reason;
  };
  Validation validateRemote(const std::string& url);

  // Reads mirror_remote from config; empty if unset.
  std::string configuredRemote() const;
  void setRemote(const std::string& url);  // caller must have validated already

  // Reads mirror_key from config; if unset, generates a fresh AES-256 key
  // and persists it. Idempotent and safe to call from multiple places --
  // only the first call (ever, for this device) actually generates one.
  struct KeySetup {
    CryptoKey key;
    bool justGenerated = false;
  };
  KeySetup ensureKey();

  // Clones the remote into dataDir/mirror if not already checked out.
  // Requires configuredRemote() to be non-empty. Returns true if a clone
  // just happened (as opposed to reusing an existing checkout) -- the
  // caller needs to know, because a fresh clone's content has never been
  // reconciled into *this* device's database yet.
  bool ensureCheckout();

  // Regenerates the mirror from `store`, commits if anything actually
  // changed, and pushes. Safe to call often -- a no-op (no commit, no
  // push attempt) when nothing changed since the last sync. Throws if a
  // remote's encrypted blob fails to decrypt (wrong mirror_key, or the
  // blob was corrupted/tampered with) -- deliberately: silently treating
  // that as "nothing to reconcile" risks then pushing this device's state
  // over data this device simply couldn't read.
  SyncOutcome sync(Store& store, SyncMode mode = SyncMode::Both);

  // Cheap read-only probe for UIs: fetches, then reports how far the remote is ahead and whether the local
  // database has changes the remote hasn't seen. Never reconciles, commits or pushes.
  struct Status {
    bool configured = false;
    std::string remote;
    bool busy = false;        // another sync holds the lock; the rest is unknown
    bool firstSync = false;   // no local checkout yet -- the first sync will pull everything
    bool reachable = false;   // fetch succeeded
    int behind = 0;           // commits on the remote we haven't pulled
    bool localDirty = false;  // local data differs from what was last synced
    std::string error;
  };
  Status status(Store& store);

  std::filesystem::path mirrorDir() const { return dataDir_ / "mirror"; }

 private:
  std::filesystem::path dataDir_;
  Config& config_;
};

// Runs `git <args>` with the given working directory (git's own -C is used
// so this works before/without a checkout existing). Never touches a
// shell. 30s timeout would be nice but isn't implemented here -- network
// calls (push/clone) can legitimately take a while on a slow link.
GitResult runGit(const std::vector<std::string>& args, const std::filesystem::path& cwd = ".");
