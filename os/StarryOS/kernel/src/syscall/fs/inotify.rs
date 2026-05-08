use alloc::string::ToString;

use ax_errno::{AxError, AxResult};
use bitflags::bitflags;
use linux_raw_sys::general::{
    AT_FDCWD, AT_SYMLINK_NOFOLLOW, IN_ACCESS, IN_ATTRIB, IN_CLOEXEC, IN_CLOSE_NOWRITE,
    IN_CLOSE_WRITE, IN_CREATE, IN_DELETE, IN_DELETE_SELF, IN_DONT_FOLLOW, IN_EXCL_UNLINK,
    IN_MASK_ADD, IN_MASK_CREATE, IN_MODIFY, IN_MOVE_SELF, IN_MOVED_FROM, IN_MOVED_TO, IN_NONBLOCK,
    IN_ONESHOT, IN_ONLYDIR, IN_OPEN, IN_Q_OVERFLOW, IN_UNMOUNT, O_CLOEXEC, O_NONBLOCK, S_IFDIR,
    S_IFMT,
};

use crate::{
    file::{FileLike, add_file_like, inotify::InotifyFd, resolve_at},
    mm::vm_load_string,
};

const IN_ALL_EVENTS: u32 = IN_ACCESS
    | IN_MODIFY
    | IN_ATTRIB
    | IN_CLOSE_WRITE
    | IN_CLOSE_NOWRITE
    | IN_OPEN
    | IN_MOVED_FROM
    | IN_MOVED_TO
    | IN_CREATE
    | IN_DELETE
    | IN_DELETE_SELF
    | IN_MOVE_SELF;

const IN_VALID_ADD_MASK: u32 = IN_ALL_EVENTS
    | IN_UNMOUNT
    | IN_Q_OVERFLOW
    | IN_ONLYDIR
    | IN_DONT_FOLLOW
    | IN_EXCL_UNLINK
    | IN_MASK_CREATE
    | IN_MASK_ADD
    | IN_ONESHOT;

bitflags! {
    #[derive(Debug, Clone, Copy, Default)]
    pub struct InotifyInitFlags: u32 {
        const CLOEXEC = IN_CLOEXEC;
        const NONBLOCK = IN_NONBLOCK;
    }
}

pub fn sys_inotify_init1(flags: u32) -> AxResult<isize> {
    debug!("sys_inotify_init1 <= flags: {flags:#x}");
    let flags = InotifyInitFlags::from_bits(flags).ok_or(AxError::InvalidInput)?;
    let inotify = InotifyFd::new();
    inotify.set_nonblocking(flags.contains(InotifyInitFlags::NONBLOCK))?;
    add_file_like(inotify as _, flags.contains(InotifyInitFlags::CLOEXEC)).map(|fd| fd as _)
}

pub fn sys_inotify_add_watch(fd: i32, pathname: *const u8, mask: u32) -> AxResult<isize> {
    let path = vm_load_string(pathname.cast())?;
    debug!("sys_inotify_add_watch <= fd: {fd}, path: {path:?}, mask: {mask:#x}");

    if mask == 0 || mask & !IN_VALID_ADD_MASK != 0 || mask & IN_ALL_EVENTS == 0 {
        return Err(AxError::InvalidInput);
    }
    if mask & IN_MASK_ADD != 0 && mask & IN_MASK_CREATE != 0 {
        return Err(AxError::InvalidInput);
    }
    let flags = if mask & IN_DONT_FOLLOW != 0 {
        AT_SYMLINK_NOFOLLOW
    } else {
        0
    };
    let resolved = resolve_at(AT_FDCWD as _, Some(&path), flags)?;
    let stat = resolved.stat()?;
    if mask & IN_ONLYDIR != 0 && stat.mode & S_IFMT != S_IFDIR {
        return Err(AxError::NotADirectory);
    }
    let watch_path = resolved
        .into_file()
        .and_then(|loc| loc.absolute_path().ok().map(|path| path.to_string()))
        .unwrap_or(path);

    let inotify = InotifyFd::from_fd(fd)?;
    inotify.add_watch(watch_path, mask).map(|wd| wd as _)
}

pub fn sys_inotify_rm_watch(fd: i32, wd: i32) -> AxResult<isize> {
    debug!("sys_inotify_rm_watch <= fd: {fd}, wd: {wd}");
    let inotify = InotifyFd::from_fd(fd)?;
    inotify.remove_watch(wd)?;
    Ok(0)
}

const _: () = {
    assert!(IN_CLOEXEC == O_CLOEXEC);
    assert!(IN_NONBLOCK == O_NONBLOCK);
};
