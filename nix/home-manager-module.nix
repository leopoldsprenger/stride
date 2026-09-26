# Home-manager module: `programs.stride`. Import via the flake's
# `homeManagerModules.default`, e.g. in your flake.nix:
#
#   home-manager.users.<you>.imports = [ stride.homeManagerModules.default ];
#
# then in your home-manager config:
#
#   programs.stride = {
#     enable = true;
#     mirrorRemote = "git@github.com:you/stride-data.git";
#   };
#
# `mirrorRemote` must point at either an empty repo or one that already
# holds a Stride mirror -- Stride itself refuses (on the first `--sync`
# run, which the timer below triggers) to touch anything else, the same
# check it applies when you're prompted for a URL interactively on macOS.
self:
{ config, lib, pkgs, ... }:
let
  cfg = config.programs.stride;
in
{
  options.programs.stride = {
    enable = lib.mkEnableOption "Stride, a terminal task manager";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "stride.packages.<system>.default";
      description = "The stride package to install.";
    };

    mirrorRemote = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "git@github.com:you/stride-data.git";
      description = ''
        SSH URL of the git repository Stride mirrors your data to, for
        backup and for syncing between devices. Leave as `null` to skip
        declaring this and be prompted for it interactively the first time
        you run `stride` instead (the interactive prompt is the only option
        on macOS; this option is how you set it declaratively on NixOS).
      '';
    };

    mirrorEncryptionKey = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = lib.literalExpression ''builtins.readFile config.sops.secrets.stride-mirror-key.path'';
      description = ''
        The 64-hex-character (32-byte) AES-256 key Stride encrypts
        everything with before it's ever written into the git mirror --
        titles, notes, checklists, even filenames are unreadable to GitHub
        or wherever the remote lives without it. Every device sharing a
        `mirrorRemote` needs the *same* key, or they can't decrypt each
        other's syncs.

        Leave as `null` to have Stride generate one itself on first sync
        and print it once so you can copy it elsewhere by hand.

        Security note: a plain Nix string literal here ends up in the
        world-readable `/nix/store`, same as any other Nix value -- fine
        for a single-user machine you trust, but not for a shared machine
        or a config you might publish. Prefer reading it from a file a
        secrets tool manages (agenix, sops-nix, etc.), as in the example
        above, rather than writing the key directly into your config.
      '';
    };

    syncInterval = lib.mkOption {
      type = lib.types.str;
      default = "10m";
      description = ''
        How often the systemd user timer runs `stride --sync`, as a
        systemd time span (e.g. "5m", "10min", "1h"). Each run is a no-op
        (no commit, no push) if nothing actually changed since the last one.
      '';
    };

    enableSyncTimer = lib.mkOption {
      type = lib.types.bool;
      default = cfg.mirrorRemote != null;
      defaultText = lib.literalExpression "mirrorRemote != null";
      description = ''
        Whether to install the systemd user timer that periodically runs
        `stride --sync`. Only takes effect on Linux (systemd --user isn't a
        thing on Darwin); has no effect if `mirrorRemote` isn't set, since
        there'd be nothing to sync to.
      '';
    };
  };

  config = lib.mkIf cfg.enable (lib.mkMerge [
    {
      home.packages = [ cfg.package ];
      assertions = [
        {
          assertion = cfg.mirrorEncryptionKey == null || builtins.stringLength cfg.mirrorEncryptionKey == 64;
          message = "programs.stride.mirrorEncryptionKey must be exactly 64 hex characters (a 32-byte AES-256 key)"
            + " -- got ${toString (builtins.stringLength cfg.mirrorEncryptionKey)}.";
        }
      ];
    }

    (lib.mkIf (cfg.mirrorRemote != null) {
      # Stride reads this on startup/`--sync`; home-manager owns the file
      # from here on, so don't hand-edit it -- change mirrorRemote/
      # mirrorEncryptionKey instead.
      home.file.".local/share/stride/config".text = ''
        # Managed by home-manager (programs.stride) -- edits here will be overwritten.
        mirror_remote=${cfg.mirrorRemote}
      '' + lib.optionalString (cfg.mirrorEncryptionKey != null) ''
        mirror_key=${cfg.mirrorEncryptionKey}
      '';
    })

    (lib.mkIf (cfg.enableSyncTimer && cfg.mirrorRemote != null && pkgs.stdenv.hostPlatform.isLinux) {
      systemd.user.services.stride-sync = {
        Unit.Description = "Sync Stride's data to its git mirror";
        Service = {
          Type = "oneshot";
          ExecStart = "${cfg.package}/bin/stride --sync";
        };
      };
      systemd.user.timers.stride-sync = {
        Unit.Description = "Periodic trigger for stride-sync.service";
        Timer = {
          OnStartupSec = "2m";  # give the network a moment after login/boot
          OnUnitActiveSec = cfg.syncInterval;
          Persistent = true;    # catch up on a missed run after sleep/shutdown
        };
        Install.WantedBy = [ "timers.target" ];
      };
    })
  ]);
}
