{ filter, self, ... }:
final: prev: {
	bcachefs = {
		tools = final.callPackage ../default.nix {
			testWithValgrind = false;
			filter = filter.lib;
			lastModified = builtins.substring 0 8 self.lastModifiedDate;
			versionString = self.version;
		};
		toolsValgrind = final.bcachefs.tools.override {
			testWithValgrind = true;
		};
		toolsDebug = final.bcachefs.toolsValgrind.override {
			debugMode = true;
		};

		bch_bindgen = final.callPackage ../rust-src/bch_bindgen {};

		mount = final.callPackage ../rust-src/mount {};

		kernelPackages = final.recurseIntoAttrs (final.linuxPackagesFor final.bcachefs.kernel);
		kernel = final.callPackage ./bcachefs-kernel.nix {
			commit = final.bcachefs.tools.bcachefs_revision;
			# This needs to be recalculated for every revision change
			sha256 = builtins.readFile ./bcachefs.rev.sha256;
			kernelPatches = [];
		};
	};
}
