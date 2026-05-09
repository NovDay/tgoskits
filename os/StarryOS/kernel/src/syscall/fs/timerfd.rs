use ax_errno::{AxError, AxResult};
use bitflags::bitflags;
use linux_raw_sys::general::{
    __kernel_clockid_t, TFD_CLOEXEC, TFD_NONBLOCK, TFD_TIMER_ABSTIME, TFD_TIMER_CANCEL_ON_SET,
    itimerspec,
};
use starry_vm::{VmMutPtr, VmPtr};

use crate::file::{
    FileLike, add_file_like,
    timerfd::{TimerFd, TimerFdClock, TimerSpec},
};

bitflags! {
    #[derive(Debug, Clone, Copy, Default)]
    pub struct TimerFdCreateFlags: u32 {
        const CLOEXEC = TFD_CLOEXEC;
        const NONBLOCK = TFD_NONBLOCK;
    }
}

bitflags! {
    #[derive(Debug, Clone, Copy, Default)]
    pub struct TimerFdSettimeFlags: u32 {
        const ABSTIME = TFD_TIMER_ABSTIME;
        const CANCEL_ON_SET = TFD_TIMER_CANCEL_ON_SET;
    }
}

pub fn sys_timerfd_create(clock_id: __kernel_clockid_t, flags: u32) -> AxResult<isize> {
    debug!("sys_timerfd_create <= clock_id: {clock_id}, flags: {flags:#x}");
    let flags = TimerFdCreateFlags::from_bits(flags).ok_or(AxError::InvalidInput)?;
    let timerfd = TimerFd::new(TimerFdClock::from_clock_id(clock_id)?);
    timerfd.set_nonblocking(flags.contains(TimerFdCreateFlags::NONBLOCK))?;
    add_file_like(timerfd as _, flags.contains(TimerFdCreateFlags::CLOEXEC)).map(|fd| fd as _)
}

pub fn sys_timerfd_settime(
    fd: i32,
    flags: u32,
    new_value: *const itimerspec,
    old_value: *mut itimerspec,
) -> AxResult<isize> {
    debug!("sys_timerfd_settime <= fd: {fd}, flags: {flags:#x}");
    let flags = TimerFdSettimeFlags::from_bits(flags).ok_or(AxError::InvalidInput)?;
    let new_value = TimerSpec::try_from(unsafe { new_value.vm_read_uninit()?.assume_init() })?;

    let timerfd = TimerFd::from_fd(fd)?;
    let old = timerfd.settime(new_value, flags.contains(TimerFdSettimeFlags::ABSTIME));
    if let Some(old_value) = old_value.nullable() {
        old_value.vm_write(old.into())?;
    }
    Ok(0)
}

pub fn sys_timerfd_gettime(fd: i32, curr_value: *mut itimerspec) -> AxResult<isize> {
    debug!("sys_timerfd_gettime <= fd: {fd}");
    let timerfd = TimerFd::from_fd(fd)?;
    let curr = timerfd.gettime();
    curr_value.vm_write(curr.into())?;
    Ok(0)
}
