{
  description = "Stride -- a terminal task manager with git-mirrored, multi-device storage";

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

        devShells.default = pkgs.mkShell {
          packages = [ pkgs.cmake pkgs.ncurses pkgs.sqlite pkgs.openssl pkgs.pkg-config pkgs.git ];
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/stride";
        };
      }) // {
      homeManagerModules.default = import ./nix/home-manager-module.nix self;
    };
}
