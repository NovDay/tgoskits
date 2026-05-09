use core::any::Any;

use ax_errno::AxError;
use axfs_ng_vfs::{DeviceId, NodeFlags, VfsResult};
use starry_vm::{VmMutPtr, VmPtr, vm_write_slice};

use crate::pseudofs::DeviceOps;

pub const CARD0_DEVICE_ID: DeviceId = DeviceId::new(226, 0);

const DRM_IOCTL_VERSION: u32 = 0xc040_6400;
const DRM_IOCTL_GET_CAP: u32 = 0xc010_640c;
const DRM_IOCTL_SET_MASTER: u32 = 0x641e;
const DRM_IOCTL_DROP_MASTER: u32 = 0x641f;
const DRM_IOCTL_MODE_GETRESOURCES: u32 = 0xc040_64a0;

const DRM_CAP_DUMB_BUFFER: u64 = 0x1;
const DRM_CAP_TIMESTAMP_MONOTONIC: u64 = 0x6;

const DRIVER_NAME: &[u8] = b"starrydrm";
const DRIVER_DATE: &[u8] = b"20260509";
const DRIVER_DESC: &[u8] = b"StarryOS framebuffer-backed DRM stub";

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmVersion {
    version_major: i32,
    version_minor: i32,
    version_patchlevel: i32,
    name_len: usize,
    name: *mut u8,
    date_len: usize,
    date: *mut u8,
    desc_len: usize,
    desc: *mut u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmGetCap {
    capability: u64,
    value: u64,
}

pub struct DrmCard;

impl DrmCard {
    pub fn new() -> Self {
        Self
    }
}

impl Default for DrmCard {
    fn default() -> Self {
        Self::new()
    }
}

impl DeviceOps for DrmCard {
    fn read_at(&self, _buf: &mut [u8], _offset: u64) -> VfsResult<usize> {
        Err(AxError::InvalidInput)
    }

    fn write_at(&self, _buf: &[u8], _offset: u64) -> VfsResult<usize> {
        Err(AxError::InvalidInput)
    }

    fn ioctl(&self, cmd: u32, arg: usize) -> VfsResult<usize> {
        match cmd {
            DRM_IOCTL_VERSION => drm_version(arg),
            DRM_IOCTL_GET_CAP => drm_get_cap(arg),
            DRM_IOCTL_SET_MASTER | DRM_IOCTL_DROP_MASTER => Ok(0),
            DRM_IOCTL_MODE_GETRESOURCES => Err(AxError::Unsupported),
            _ => Err(AxError::Unsupported),
        }
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn flags(&self) -> NodeFlags {
        NodeFlags::NON_CACHEABLE
    }
}

fn drm_version(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut version = unsafe { (arg as *const DrmVersion).vm_read_uninit()?.assume_init() };
    version.version_major = 0;
    version.version_minor = 1;
    version.version_patchlevel = 0;
    copy_drm_string(version.name, &mut version.name_len, DRIVER_NAME)?;
    copy_drm_string(version.date, &mut version.date_len, DRIVER_DATE)?;
    copy_drm_string(version.desc, &mut version.desc_len, DRIVER_DESC)?;
    (arg as *mut DrmVersion).vm_write(version)?;
    Ok(0)
}

fn drm_get_cap(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut cap = unsafe { (arg as *const DrmGetCap).vm_read_uninit()?.assume_init() };
    cap.value = match cap.capability {
        DRM_CAP_DUMB_BUFFER => 0,
        DRM_CAP_TIMESTAMP_MONOTONIC => 1,
        _ => 0,
    };
    (arg as *mut DrmGetCap).vm_write(cap)?;
    Ok(0)
}

fn copy_drm_string(dst: *mut u8, len: &mut usize, value: &[u8]) -> VfsResult<()> {
    let user_len = *len;
    *len = value.len();
    if dst.is_null() || user_len == 0 {
        return Ok(());
    }
    let copy_len = user_len.min(value.len());
    Ok(vm_write_slice(dst, &value[..copy_len])?)
}
