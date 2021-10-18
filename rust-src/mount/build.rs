fn main() {
	use std::path::PathBuf;
	use std::process::Command;

	let out_dir: PathBuf = std::env::var_os("OUT_DIR").unwrap().into();
	let top_dir: PathBuf = std::env::var_os("CARGO_MANIFEST_DIR").unwrap().into();
	let libbcachefs_inc_dir = std::env::var("LIBBCACHEFS_INCLUDE")
		.unwrap_or_else(|_| top_dir.join("libbcachefs").display().to_string());
	let libbcachefs_inc_dir = std::path::Path::new(&libbcachefs_inc_dir);
	println!("{}", libbcachefs_inc_dir.display());

	let libbcachefs_dir = top_dir.join("libbcachefs").join("libbcachefs");
	let bindings = bindgen::builder()
		.header(top_dir
			.join("src")
			.join("libbcachefs_wrapper.h")
			.display()
			.to_string())
		.clang_arg(format!(
			"-I{}",
			libbcachefs_inc_dir.join("include").display()
		))
		.clang_arg(format!("-I{}", libbcachefs_inc_dir.display()))
		.clang_arg("-DZSTD_STATIC_LINKING_ONLY")
		.clang_arg("-DNO_BCACHEFS_FS")
		.clang_arg("-D_GNU_SOURCE")
		.derive_debug(false)
		.derive_default(true)
		.default_enum_style(bindgen::EnumVariation::Rust {
			non_exhaustive: true,
		})
		.whitelist_function("bch2_read_super")
		.whitelist_function("bch2_sb_field_.*")
		.whitelist_function("bch2_chacha_encrypt_key")
		.whitelist_function("derive_passphrase")
		.whitelist_function("request_key")
		.whitelist_function("add_key")
		.whitelist_function("keyctl_search")
		.whitelist_var("BCH_.*")
		.whitelist_var("KEY_SPEC_.*")
		.whitelist_type("bch_kdf_types")
		.whitelist_type("bch_sb_field_.*")
		.whitelist_type("bch_encrypted_key")
		.whitelist_type("nonce")
		.rustified_enum("bch_kdf_types")
		.opaque_type("gendisk")
		.opaque_type("bkey")
		.generate()
		.unwrap();
	bindings.write_to_file(out_dir.join("bcachefs.rs")).unwrap();

	let keyutils = pkg_config::probe_library("libkeyutils").unwrap();
	let bindings = bindgen::builder()
		.header(top_dir
			.join("src")
			.join("keyutils_wrapper.h")
			.display()
			.to_string())
		.clang_args(
			keyutils.include_paths
				.iter()
				.map(|p| format!("-I{}", p.display())),
		)
		.generate()
		.unwrap();
	bindings.write_to_file(out_dir.join("keyutils.rs")).unwrap();
}
