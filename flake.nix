{
  description = "Stride -- a keyboard-first to-do manager (TUI + GTK4) with git-mirrored, multi-device storage";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        packages.default = pkgs.callPackage ./nix/package.nix { };
        # same build, but a bare `stride` opens the GTK4 app: `nix run .#gui`
        packages.gui = self.packages.${system}.default.override { defaultInterface = "gui"; };

        devShells.default = pkgs.mkShell {
          packages = [ pkgs.cmake pkgs.ncurses pkgs.sqlite pkgs.openssl pkgs.gtk4 pkgs.pkg-config pkgs.git ];
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/stride";
        };
        apps.gui = {
          type = "app";
          program = "${self.packages.${system}.gui}/bin/stride";
        };
      }) // {
      homeManagerModules.default = import ./nix/home-manager-module.nix self;
    };
}
