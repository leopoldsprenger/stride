# The actual build recipe, kept separate from flake.nix so it can also be
# used from a non-flake nixpkgs overlay/callPackage if you'd rather not
# pull in flakes at all.
{ lib
, stdenv
, cmake
, pkg-config
, ncurses
, sqlite
, openssl
, git
, makeWrapper
}:

stdenv.mkDerivation {
  pname = "stride";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    filter = name: type:
      let base = baseNameOf name; in
      # keep the build out of the source tree Nix hashes, and skip the
      # git/editor cruft that would otherwise bust the build cache on
      # every commit that doesn't actually touch the source
      base != "build" && base != ".git" && base != ".cache";
  };

  nativeBuildInputs = [ cmake pkg-config makeWrapper ];
  buildInputs = [
    ncurses  # nixpkgs' ncurses is built with wide-char (ncursesw) support by
             # default; if your channel's differs, override with
             # `ncurses.override { unicode = true; }`
    sqlite
    openssl  # AES-256-GCM for the encrypted git mirror (src/crypto.cpp)
  ];

  # `stride --sync` shells out to the `git` binary by name (see src/sync.cpp)
  # rather than linking libgit2, so it needs `git` on PATH at runtime -- Nix
  # builds don't have an ambient PATH, so wrap it in explicitly.
  postFixup = ''
    wrapProgram $out/bin/stride --prefix PATH : ${lib.makeBinPath [ git ]}
  '';

  meta = {
    description = "A terminal task manager with a Things-3-inspired TUI and git-mirrored, multi-device storage";
    license = lib.licenses.mit;  # placeholder -- set this to whatever license you actually want
    platforms = lib.platforms.unix;
    mainProgram = "stride";
  };
}
