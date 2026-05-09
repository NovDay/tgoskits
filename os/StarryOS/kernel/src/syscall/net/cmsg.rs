use alloc::{sync::Arc, vec::Vec};

use ax_errno::{AxError, AxResult};
use linux_raw_sys::{
    ctypes::c_long,
    net::{SCM_CREDENTIALS, SCM_RIGHTS, SOL_SOCKET, cmsghdr, ucred},
};

use crate::{
    file::{FileLike, get_file_like},
    mm::{UserConstPtr, UserPtr},
};

pub enum CMsg {
    Rights { fds: Vec<Arc<dyn FileLike>> },
    Credentials,
}

pub fn cmsg_align(len: usize) -> usize {
    let align = size_of::<c_long>();
    (len + align - 1) & !(align - 1)
}

fn cmsg_align_down(len: usize) -> usize {
    let align = size_of::<c_long>();
    len & !(align - 1)
}

impl CMsg {
    pub fn parse(hdr: &cmsghdr) -> AxResult<Self> {
        if hdr.cmsg_len < size_of::<cmsghdr>() {
            return Err(AxError::InvalidInput);
        }

        let data =
            UserConstPtr::<u8>::from((hdr as *const cmsghdr as usize) + size_of::<cmsghdr>())
                .get_as_slice(hdr.cmsg_len - size_of::<cmsghdr>())?;
        Ok(match (hdr.cmsg_level as u32, hdr.cmsg_type as u32) {
            (SOL_SOCKET, SCM_RIGHTS) => {
                if data.len() % size_of::<i32>() != 0 {
                    return Err(AxError::InvalidInput);
                }
                let mut fds = Vec::new();
                for fd in data.chunks_exact(size_of::<i32>()) {
                    let fd = i32::from_ne_bytes(fd.try_into().unwrap());
                    if fd < 0 {
                        return Err(AxError::BadFileDescriptor);
                    }
                    let f = get_file_like(fd)?;
                    fds.push(f);
                }
                Self::Rights { fds }
            }
            (SOL_SOCKET, SCM_CREDENTIALS) => {
                if data.len() < size_of::<ucred>() {
                    return Err(AxError::InvalidInput);
                }
                Self::Credentials
            }
            _ => {
                return Err(AxError::InvalidInput);
            }
        })
    }
}

pub struct CMsgBuilder<'a> {
    hdr: UserPtr<cmsghdr>,
    len: &'a mut usize,
    capacity: usize,
}
impl<'a> CMsgBuilder<'a> {
    pub fn new(msg: UserPtr<cmsghdr>, len: &'a mut usize) -> Self {
        let capacity = *len;
        *len = 0;
        Self {
            hdr: msg,
            len,
            capacity,
        }
    }

    pub fn push(
        &mut self,
        level: u32,
        ty: u32,
        body: impl FnOnce(&mut [u8]) -> AxResult<usize>,
    ) -> AxResult<bool> {
        let Some(available) = self.capacity.checked_sub(*self.len) else {
            return Ok(false);
        };
        let Some(body_capacity) = cmsg_align_down(available).checked_sub(size_of::<cmsghdr>())
        else {
            return Ok(false);
        };

        let hdr = self.hdr.get_as_mut()?;
        hdr.cmsg_level = level as _;
        hdr.cmsg_type = ty as _;

        let data = UserPtr::<u8>::from(self.hdr.address().as_usize() + size_of::<cmsghdr>())
            .get_as_mut_slice(body_capacity)?;
        let body_len = body(data)?;

        let cmsg_len = size_of::<cmsghdr>() + body_len;
        let aligned_len = cmsg_align(cmsg_len);
        if aligned_len > available {
            return Ok(false);
        }
        hdr.cmsg_len = cmsg_len;
        self.hdr = UserPtr::from(hdr as *const _ as usize + aligned_len);
        *self.len += aligned_len;
        Ok(true)
    }
}
