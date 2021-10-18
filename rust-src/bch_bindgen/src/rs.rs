use crate::bcachefs;

pub const SUPERBLOCK_MAGIC: uuid::Uuid = uuid::Uuid::from_u128(
	0x_c68573f6_4e1a_45ca_8265_f57f48ba6d81
);
	
extern "C" {
	pub static stdout: *mut libc::FILE;
}

pub enum ReadSuperErr {
	Io(std::io::Error),
}

type RResult<T> = std::io::Result<std::io::Result<T>>;

#[tracing_attributes::instrument(skip(opts))]
pub fn read_super_opts(path: &std::path::Path, mut opts: bcachefs::bch_opts) -> RResult<bcachefs::bch_sb_handle> {
	// let devp = camino::Utf8Path::from_path(devp).unwrap();

	use std::os::unix::ffi::OsStrExt;
	let path = std::ffi::CString::new(path.as_os_str().as_bytes())?;

	let mut sb = std::mem::MaybeUninit::zeroed();

	// use gag::{BufferRedirect};
	// // Stop libbcachefs from spamming the output
	// let gag = BufferRedirect::stderr().unwrap();
	// tracing::trace!("entering libbcachefs");

	let ret = unsafe { crate::bcachefs::bch2_read_super(path.as_ptr(), &mut opts, sb.as_mut_ptr()) };
	tracing::trace!(%ret);

	match -ret {
		libc::EACCES => Err(std::io::Error::new(
			std::io::ErrorKind::PermissionDenied,
			"Access Permission Denied",
		)),
		0 => Ok(Ok(unsafe { sb.assume_init() })),
		22 => Ok(Err(std::io::Error::new(
			std::io::ErrorKind::InvalidData,
			"Not a BCacheFS SuperBlock",
		))),
		code => {
			tracing::debug!(msg = "BCacheFS return error code", ?code);
			Ok(Err(std::io::Error::new(
				std::io::ErrorKind::Other,
				"Failed to Read SuperBlock",
			)))
		}
	}
}

#[tracing_attributes::instrument]
pub fn read_super(path: &std::path::Path) -> RResult<bcachefs::bch_sb_handle> {
	let opts = bcachefs::bch_opts::default(); //unsafe {std::mem::MaybeUninit::zeroed().assume_init()};
	read_super_opts(path, opts)
}
