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
#     interface = "gui";   # one toggle: what a bare `stride` opens ("tui" is the default)
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
  # The interface toggle and GUI look are build-time arguments of the package (they become defaults baked into its
  # wrapper), so one package works whether this module or a plain callPackage consumes it. Only re-parameterise
  # when something differs from the package's own defaults, so the common case stays a cache hit.
  finalPackage =
    if cfg.interface == "tui" && cfg.gui.theme == "auto" && cfg.gui.themeCss == null && cfg.gui.accent == null && !cfg.gui.decorations
    then cfg.package
    else cfg.package.override {
      defaultInterface = cfg.interface;
      guiTheme = cfg.gui.theme;
      guiThemeCss = cfg.gui.themeCss;
      guiAccent = cfg.gui.accent;
      guiDecorations = cfg.gui.decorations;
    };
  syncFlag = { both = "--sync"; pull = "--pull"; push = "--push"; }.${cfg.syncDirection};
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

    interface = lib.mkOption {
      type = lib.types.enum [ "tui" "gui" ];
      default = "tui";
      example = "gui";
      description = ''
        Which front end a bare `stride` opens: the terminal app (`"tui"`, the default) or the GTK4 app (`"gui"`).
        This is the whole switch -- the other one stays one command away either way (`stride --tui`,
        `stride --gui`, or the `stride-tui` / `stride-gui` commands), the launcher entry always starts the GUI, and
        the sync timer, quick capture and the scripting flags don't care which you pick. Falls back to the TUI if the
        package was built without GTK4 or no display is reachable.
      '';
    };

    gui = {
      theme = lib.mkOption {
        type = lib.types.enum [ "auto" "light" "dark" ];
        default = "auto";
        description = ''
          GUI colours. `auto` takes every colour (background, text, accent, even the list icons) from the GTK theme
          that is active right now, and follows it live when it changes. `light` / `dark` force the built-in palettes.
        '';
      };
      themeCss = lib.mkOption {
        type = lib.types.nullOr (lib.types.either lib.types.path lib.types.str);
        default = null;
        example = "/home/me/.config/gtk-4.0/gtk.css";
        description = ''
          Optional path to a GTK stylesheet whose `@define-color` lines supply the colours, for themes that GTK
          itself doesn't expose. `null` (the default) reads the active GTK theme directly.
        '';
      };
      accent = lib.mkOption {
        type = lib.types.nullOr (lib.types.strMatching "#[0-9a-fA-F]{6}");
        default = null;
        example = "#bb9af7";
        description = "Accent colour for check marks and selection, as `#rrggbb`. `null` keeps the built-in blue.";
      };
      decorations = lib.mkOption {
        type = lib.types.bool;
        default = false;
        description = ''
          Whether the GUI window asks for a titlebar. Off by default: a clean undecorated window suits tiling
          compositors, and the page header is draggable where a compositor supports it.
        '';
      };
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

    syncDirection = lib.mkOption {
      type = lib.types.enum [ "both" "pull" "push" ];
      default = "both";
      description = ''
        What the periodic timer does each run: `both` (pull other devices' changes, then push yours -- the
        default), `pull` (only bring remote changes into this machine, never push; handy on a read-mostly
        device) or `push` (only back this machine up).
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

        Once installed, the timer is enabled the normal systemd way (its
        `Install.WantedBy = [ "timers.target" ]`), which starts it at every
        login without anything further from you, and this module also
        starts it immediately on the `home-manager switch` that first turns
        it on, so you don't need to log out and back in. On a headless or
        server-style NixOS machine where a user might not interactively log
        in at all, add `users.users.<you>.linger = true;` to your *system*
        (not home-manager) config -- the declarative equivalent of
        `loginctl enable-linger` -- so the timer starts at boot instead of
        waiting for a session.
      '';
    };
  };

  config = lib.mkIf cfg.enable (lib.mkMerge [
    {
      home.packages = [ finalPackage ];
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
          ExecStart = "${finalPackage}/bin/stride ${syncFlag}";
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
      # `Install.WantedBy` above is what makes systemd *enable* the timer --
      # i.e. symlink it so it starts at every future login on its own -- and
      # home-manager does that enabling as a normal part of activation no
      # matter what. `startServices` is the separate question of whether
      # *this* `home-manager switch` also starts it right now, instead of
      # making you log out and back in first. `mkDefault` here so it only
      # takes effect if you haven't already set your own preference
      # elsewhere in your config.
      systemd.user.startServices = lib.mkDefault "sd-switch";
    })
  ]);
}
