# The actual build recipe, kept separate from flake.nix so it can also be
# used from a non-flake nixpkgs overlay/callPackage if you'd rather not
# pull in flakes at all.
#
# Switching what a bare `stride` opens is a one-argument change, whoever is consuming this file:
#
#   pkgs.callPackage ./nix/package.nix { defaultInterface = "gui"; }
#   stride.packages.${system}.default.override { defaultInterface = "gui"; }
#
# (the home-manager module's `programs.stride.interface = "gui";` does exactly that).
{ lib
, stdenv
, cmake
, pkg-config
, ncurses
, sqlite
, openssl
, git
, makeWrapper
, wrapGAppsHook4
, gtk4
, gsettings-desktop-schemas
, adwaita-icon-theme
, withGtk4 ? true  # Builds the GTK4 front ends: `stride --gui` (the full app) and `stride --quick-capture`
                   # (a small floating window instead of needing a dedicated floating terminal for the ncurses
                   # dialog -- see the "Quick capture" section of README.md). Set to false for a smaller closure
                   # (e.g. a headless box); `--gui` then reports it's unavailable and `stride` falls back to the
                   # TUI, and --quick-capture always uses the ncurses dialog, same as before.
, defaultInterface ? "tui"  # What a bare `stride` opens: "tui" (the ncurses app, unchanged) or "gui" (GTK4).
                            # `stride --tui` / `stride --gui` (and `stride-tui` / `stride-gui`) always work.
, guiTheme ? "auto"         # "auto" takes every colour from the active GTK theme, live; "light" / "dark" force the built-in palettes.
, guiThemeCss ? null       # Path to a GTK stylesheet (@define-color lines) to take colours from instead of the live theme.
, guiAccent ? null          # A hex colour like "#5b9cff" for check marks and selection; null = built-in blue.
, guiDecorations ? false    # true = a titlebar from the compositor/GTK; false = a clean undecorated window.
}:

assert lib.assertOneOf "defaultInterface" defaultInterface [ "tui" "gui" ];
assert lib.assertOneOf "guiTheme" guiTheme [ "auto" "light" "dark" ];

stdenv.mkDerivation {
  pname = "stride";
  version = "0.2.0";

  src = lib.cleanSourceWith {
    src = ../.;
    filter = name: type:
      let base = baseNameOf name; in
      # keep the build out of the source tree Nix hashes, and skip the
      # git/editor cruft that would otherwise bust the build cache on
      # every commit that doesn't actually touch the source
      base != "build" && base != ".git" && base != ".cache";
  };

  nativeBuildInputs = [ cmake pkg-config makeWrapper ] ++ lib.optional withGtk4 wrapGAppsHook4;
  buildInputs = [
    ncurses  # nixpkgs' ncurses is built with wide-char (ncursesw) support by
             # default; if your channel's differs, override with
             # `ncurses.override { unicode = true; }`
    sqlite
    openssl  # AES-256-GCM for the encrypted git mirror (src/crypto.cpp)
  ] ++ lib.optionals withGtk4 [
    gtk4
    gsettings-desktop-schemas  # lets the GUI follow the desktop's light/dark preference
    adwaita-icon-theme
  ];

  cmakeFlags = lib.optional (!withGtk4) "-DSTRIDE_NO_GTK4=ON";

  # `stride --sync` shells out to the `git` binary by name (see src/sync.cpp)
  # rather than linking libgit2, so it needs `git` on PATH at runtime -- Nix
  # builds don't have an ambient PATH, so wrap it in explicitly.
  #
  # The same wrapper carries the declarative settings as *defaults* (--set-default): an environment variable or
  # command-line flag given at runtime still wins. With GTK4, wrapGAppsHook4 wraps the binary itself, adding the
  # GSettings/icon environment around these arguments.
  preFixup = ''
    ${if withGtk4 then "gappsWrapperArgs" else "wrapperArgs"}+=(
      --prefix PATH : ${lib.makeBinPath [ git ]}
      --set-default STRIDE_INTERFACE ${if withGtk4 then defaultInterface else "tui"}
      --set-default STRIDE_THEME ${guiTheme}
      --set-default STRIDE_DECORATIONS ${if guiDecorations then "1" else "0"}
      ${lib.optionalString (guiThemeCss != null) "--set-default STRIDE_THEME_CSS ${lib.escapeShellArg (toString guiThemeCss)}"}
      ${lib.optionalString (guiAccent != null) "--set-default STRIDE_ACCENT ${lib.escapeShellArg guiAccent}"}
    )
  '';

  postFixup =
    lib.optionalString (!withGtk4) ''
      wrapProgram $out/bin/stride "''${wrapperArgs[@]}"
    ''
    + lib.optionalString withGtk4 ''
      # explicit entry points, independent of the default chosen above
      makeWrapper $out/bin/stride $out/bin/stride-tui --add-flags --tui
      makeWrapper $out/bin/stride $out/bin/stride-gui --add-flags --gui
    '';

  meta = {
    description = "A quiet, keyboard-first to-do manager: a Things-inspired TUI and GTK4 app with git-mirrored, multi-device storage";
    license = lib.licenses.mit;  # placeholder -- set this to whatever license you actually want
    platforms = lib.platforms.unix;
    mainProgram = "stride";
  };
}
