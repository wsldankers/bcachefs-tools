{
	description = "Userspace tools for bcachefs";

	# Nixpkgs / NixOS version to use.
	inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
	inputs.utils.url = "github:numtide/flake-utils";
	inputs.filter.url = "github:numtide/nix-filter";

	outputs = { self, nixpkgs, utils, filter, ... }@inputs:
		let
			# System types to support.
			supportedSystems = [ "x86_64-linux" ];
		in
		{
			version = "${builtins.substring 0 8 self.lastModifiedDate}-${self.shortRev or "dirty"}";

			overlay = import ./nix/overlay.nix inputs;
			nixosModule = self.nixosModules.bcachefs;
			nixosModules.bcachefs = import ./rust-src/mount/module.nix;
			nixosModules.bcachefs-enable-boot = ({config, pkgs, lib, ... }:{
				# Disable Upstream NixOS Module when this is in use
				disabledModules = [ "tasks/filesystems/bcachefs.nix" ];
				# Import needed packages
				nixpkgs.overlays = [ self.overlay ];

				# Add bcachefs to boot and kernel
				boot.initrd.supportedFilesystems = [ "bcachefs" ];
				boot.supportedFilesystems = [ "bcachefs" ];
			});
		}
		// utils.lib.eachSystem supportedSystems (system: 
		let pkgs = import nixpkgs { 
			inherit system; 
			overlays = [ self.overlay ]; 
		}; 
		in rec {
			
			# A Nixpkgs overlay.

			# Provide some binary packages for selected system types.
			defaultPackage = pkgs.bcachefs.tools;
			packages = {
				inherit (pkgs.bcachefs)
					tools
					toolsValgrind
					toolsDebug
					mount
					bch_bindgen
					kernel;

				tools-musl = pkgs.pkgsMusl.bcachefs.tools;
				mount-musl = pkgs.pkgsMusl.bcachefs.mount;
			};

			checks = { 
				kernelSrc = packages.kernel.src;
				inherit (packages) 
					mount
					bch_bindgen
					toolsValgrind;
			};

			devShell = devShells.tools;
			devShells.tools = pkgs.bcachefs.tools.override { inShell = true; };
			devShells.mount = pkgs.bcachefs.mount.override { inShell = true; };
		});
}
