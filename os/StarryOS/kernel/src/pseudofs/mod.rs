//! Basic virtual filesystem support

pub mod dev;
mod device;
mod dir;
mod file;
mod fs;
mod proc;
pub(crate) mod sys;
mod tmp;
#[cfg(feature = "plat-dyn")]
pub(crate) mod usbfs;

use alloc::sync::Arc;

use ax_errno::LinuxResult;
use ax_fs::{FS_CONTEXT, FsContext};
use axfs_ng_vfs::{
    DirNodeOps, FileNodeOps, Filesystem, MetadataUpdate, NodePermission, WeakDirEntry,
};
pub use tmp::MemoryFs;

pub use self::{device::*, dir::*, file::*, fs::*};

/// A callback that builds a `Arc<dyn DirNodeOps>` for a given
/// `WeakDirEntry`.
pub type DirMaker = Arc<dyn Fn(WeakDirEntry) -> Arc<dyn DirNodeOps> + Send + Sync>;

/// An enum containing either a directory ([`DirMaker`]) or a file (`Arc<dyn
/// FileNodeOps>`).
#[derive(Clone)]
pub enum NodeOpsMux {
    /// A directory node.
    Dir(DirMaker),
    /// A file node.
    File(Arc<dyn FileNodeOps>),
}

impl From<DirMaker> for NodeOpsMux {
    fn from(maker: DirMaker) -> Self {
        Self::Dir(maker)
    }
}

impl<T: FileNodeOps> From<Arc<T>> for NodeOpsMux {
    fn from(ops: Arc<T>) -> Self {
        Self::File(ops)
    }
}

const DIR_PERMISSION: NodePermission = NodePermission::from_bits_truncate(0o755);
const RUNTIME_DIR_PERMISSION: NodePermission = NodePermission::from_bits_truncate(0o700);
const STICKY_TMP_PERMISSION: NodePermission = NodePermission::from_bits_truncate(0o1777);

fn mount_at(fs: &FsContext, path: &str, mount_fs: Filesystem) -> LinuxResult<()> {
    if fs.resolve(path).is_err() {
        fs.create_dir(path, DIR_PERMISSION)?;
    }
    fs.resolve(path)?.mount(&mount_fs)?;
    info!("Mounted {} at {}", mount_fs.name(), path);
    Ok(())
}

fn ensure_dir(fs: &FsContext, path: &str, mode: NodePermission) -> LinuxResult<()> {
    if fs.resolve(path).is_err() {
        fs.create_dir(path, mode)?;
    }
    fs.resolve(path)?.update_metadata(MetadataUpdate {
        mode: Some(mode),
        ..Default::default()
    })?;
    Ok(())
}

/// Mount all filesystems
pub fn mount_all() -> LinuxResult<()> {
    info!("Initialize pseudofs...");

    let fs = FS_CONTEXT.lock();
    mount_at(&fs, "/dev", dev::new_devfs())?;
    #[cfg(feature = "plat-dyn")]
    mount_at(&fs, "/dev/bus/usb", usbfs::new_usbfs()?)?;
    mount_at(&fs, "/dev/shm", tmp::MemoryFs::new())?;
    ensure_dir(&fs, "/dev/shm", STICKY_TMP_PERMISSION)?;
    mount_at(&fs, "/tmp", tmp::MemoryFs::new())?;
    ensure_dir(&fs, "/tmp", STICKY_TMP_PERMISSION)?;
    mount_at(&fs, "/run", tmp::MemoryFs::new())?;
    ensure_dir(&fs, "/run", DIR_PERMISSION)?;
    ensure_dir(&fs, "/run/user", DIR_PERMISSION)?;
    ensure_dir(&fs, "/run/user/0", RUNTIME_DIR_PERMISSION)?;
    mount_at(&fs, "/proc", proc::new_procfs())?;

    #[cfg(feature = "plat-dyn")]
    mount_at(&fs, "/sys", usbfs::new_sysfs())?;
    #[cfg(not(feature = "plat-dyn"))]
    mount_at(&fs, "/sys", tmp::MemoryFs::new())?;
    drop(fs);
    #[cfg(not(feature = "plat-dyn"))]
    sys::populate_sysfs()?;
    #[cfg(feature = "plat-dyn")]
    sys::populate_udev_data()?;

    #[cfg(feature = "dev-log")]
    dev::bind_dev_log().expect("Failed to bind /dev/log");

    Ok(())
}
