{ lib
, stdenv
, rustPlatform
, llvmPackages
, bcachefs
, pkg-config

, udev
, liburcu
, zstd
, keyutils
, libaio
		
, lz4 # liblz4
, libsodium
, libuuid
, zlib # zlib1g
, libscrypt

, rustfmt

, glibc
, ...
}: let 
	include = {
		glibc = "${glibc.dev}/include";
		clang = let libc = llvmPackages.libclang; in
			"${libc.lib}/lib/clang/${libc.version}/include";
		urcu = "${liburcu}/include";
		zstd = "${zstd.dev}/include";
	};
	cargo = lib.trivial.importTOML ./Cargo.toml;
in rustPlatform.buildRustPackage {
	pname = cargo.package.name;
	version = cargo.package.version;
	
	src = builtins.path { path = ./.; name = "bch_bindgen"; };

	cargoLock = { lockFile = ./Cargo.lock; };

	nativeBuildInputs = [ rustfmt pkg-config ];
	buildInputs = [
		
		# libaio
		keyutils # libkeyutils
		lz4 # liblz4
		libsodium
		liburcu
		libuuid
		zstd # libzstd
		zlib # zlib1g
		udev
		libscrypt
		libaio
	];
	
	LIBBCACHEFS_LIB ="${bcachefs.tools}/lib";
	LIBBCACHEFS_INCLUDE = bcachefs.tools.src;
	LIBCLANG_PATH = "${llvmPackages.libclang.lib}/lib";
	BINDGEN_EXTRA_CLANG_ARGS = lib.replaceStrings ["\n" "\t"] [" " ""] ''
		-std=gnu99
		-I${include.glibc}
		-I${include.clang}
		-I${include.urcu}
		-I${include.zstd}
	'';

	postPatch = ''
		cp ${./Cargo.lock} Cargo.lock
	'';
	

	doCheck = true;
	
	# NIX_DEBUG = 4;
}