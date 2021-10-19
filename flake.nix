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

			nixosConfigurations.netboot-bcachefs = self.systems.netboot-bcachefs "x86_64-linux";
			systems.netboot-bcachefs = system: (nixpkgs.lib.nixosSystem { 
					inherit system; modules = [
						self.nixosModule 
						self.nixosModules.bcachefs-enable-boot
						("${nixpkgs}/nixos/modules/installer/netboot/netboot-minimal.nix")
						({ lib, pkgs, config, ... }: {
							# installation disk autologin
							services.getty.autologinUser = lib.mkForce "root";
							users.users.root.initialPassword = "toor";
							
							# Symlink everything together
							system.build.netboot = pkgs.symlinkJoin {
								name = "netboot";
								paths = with config.system.build; [
									netbootRamdisk
									kernel
									netbootIpxeScript
								];
								preferLocalBuild = true;
							};
						})
					]; 
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

				# Build and test initrd with bcachefs and bcachefs.mount installed
				# Disabled Test because it takes a while to build the kernel
				# bootStage1Module = self.nixosConfigurations.netboot-bcachefs.config.system.build.bootStage1;
			};

			devShell = devShells.tools;
			devShells.tools = pkgs.bcachefs.tools.override { inShell = true; };
			devShells.mount = pkgs.bcachefs.mount.override { inShell = true; };
		});
}
