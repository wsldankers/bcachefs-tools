{ lib
, fetchpatch
, fetchgit
, fetchFromGitHub
, buildLinux
, commit
, diffHash ? lib.fakeSha256
, kernelVersion ? "5.13.0"
, kernelPatches ? [] # must always be defined in bcachefs' all-packages.nix entry because it's also a top-level attribute supplied by callPackage
, argsOverride ? {}
, versionString ? (builtins.substring 0 8 commit)
, ...
} @ args:

buildLinux {
	inherit kernelPatches;

	# pname = "linux";
	version = "${kernelVersion}-bcachefs-${versionString}";
	
	modDirVersion = kernelVersion;
	

	src = fetchFromGitHub {
		name = "bcachefs-kernel-src";
		owner = "koverstreet";
		repo = "bcachefs";
		rev = commit;
		sha256 = diffHash;
	};

	extraConfig = "BCACHEFS_FS m";
	# NIX_DEBUG=5;
}