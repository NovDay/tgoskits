use alloc::{
    borrow::Cow,
    collections::{BTreeMap, VecDeque},
    string::{String, ToString},
    sync::{Arc, Weak},
    vec::Vec,
};
use core::{
    sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering},
    task::Context,
};

use ax_errno::{AxError, AxResult};
use ax_task::future::{block_on, poll_io};
use axpoll::{IoEvents, PollSet, Pollable};
use linux_raw_sys::general::{
    IN_ATTRIB, IN_CLOSE_NOWRITE, IN_CLOSE_WRITE, IN_DELETE_SELF, IN_IGNORED, IN_ISDIR, IN_MASK_ADD,
    IN_MASK_CREATE, IN_MODIFY, IN_OPEN, inotify_event,
};
use spin::{Lazy, Mutex};

use crate::file::{FileLike, IoDst, IoSrc};

const EVENT_HEADER_SIZE: usize = size_of::<inotify_event>();

#[derive(Debug, Clone)]
struct Watch {
    path: String,
    mask: u32,
}

#[derive(Debug, Clone)]
struct QueuedEvent {
    wd: i32,
    mask: u32,
    cookie: u32,
    name: String,
}

#[derive(Debug, Default)]
struct InotifyState {
    watches: BTreeMap<i32, Watch>,
    events: VecDeque<QueuedEvent>,
}

pub struct InotifyFd {
    state: Mutex<InotifyState>,
    next_wd: AtomicI32,
    non_blocking: AtomicBool,
    poll_rx: PollSet,
}

static INOTIFY_FDS: Lazy<Mutex<Vec<Weak<InotifyFd>>>> = Lazy::new(|| Mutex::new(Vec::new()));
static NEXT_COOKIE: AtomicU32 = AtomicU32::new(1);

impl InotifyFd {
    pub fn new() -> Arc<Self> {
        let this = Arc::new(Self {
            state: Mutex::new(InotifyState::default()),
            next_wd: AtomicI32::new(1),
            non_blocking: AtomicBool::new(false),
            poll_rx: PollSet::new(),
        });
        INOTIFY_FDS.lock().push(Arc::downgrade(&this));
        this
    }

    pub fn add_watch(&self, path: String, mask: u32) -> AxResult<i32> {
        if mask == 0 {
            return Err(AxError::InvalidInput);
        }

        let mut state = self.state.lock();
        let existing = state
            .watches
            .iter()
            .find_map(|(wd, watch)| (watch.path == path).then_some(*wd));

        if let Some(wd) = existing {
            if mask & IN_MASK_CREATE != 0 {
                return Err(AxError::AlreadyExists);
            }
            let watch = state.watches.get_mut(&wd).ok_or(AxError::InvalidInput)?;
            if mask & IN_MASK_ADD != 0 {
                watch.mask |= mask;
            } else {
                watch.mask = mask;
            }
            return Ok(wd);
        }

        let wd = self.next_wd.fetch_add(1, Ordering::AcqRel);
        state.watches.insert(wd, Watch { path, mask });
        Ok(wd)
    }

    pub fn remove_watch(&self, wd: i32) -> AxResult {
        let mut state = self.state.lock();
        state.watches.remove(&wd).ok_or(AxError::InvalidInput)?;
        state.events.push_back(QueuedEvent {
            wd,
            mask: IN_IGNORED,
            cookie: 0,
            name: String::new(),
        });
        drop(state);

        self.poll_rx.wake();
        Ok(())
    }

    fn queue_path_event(&self, path: &str, mask: u32, cookie: u32) {
        let queued = {
            let mut state = self.state.lock();
            let events: Vec<_> = state
                .watches
                .iter()
                .filter_map(|(wd, watch)| {
                    (watch.path == path && watch.mask & mask != 0).then_some(QueuedEvent {
                        wd: *wd,
                        mask,
                        cookie,
                        name: String::new(),
                    })
                })
                .collect();
            let queued = !events.is_empty();
            state.events.extend(events);
            queued
        };
        if queued {
            self.poll_rx.wake();
        }
    }

    fn queue_child_event(&self, parent_path: &str, name: &str, mask: u32, cookie: u32) {
        let queued = {
            let mut state = self.state.lock();
            let events: Vec<_> = state
                .watches
                .iter()
                .filter_map(|(wd, watch)| {
                    (watch.path == parent_path && watch.mask & mask != 0).then_some(QueuedEvent {
                        wd: *wd,
                        mask,
                        cookie,
                        name: name.to_string(),
                    })
                })
                .collect();
            let queued = !events.is_empty();
            state.events.extend(events);
            queued
        };
        if queued {
            self.poll_rx.wake();
        }
    }

    fn queue_deleted_event(&self, path: &str, mask: u32) {
        let queued = {
            let mut state = self.state.lock();
            let mut removed = Vec::new();
            let mut events = Vec::new();
            for (wd, watch) in state.watches.iter() {
                if watch.path == path {
                    if watch.mask & mask != 0 {
                        events.push(QueuedEvent {
                            wd: *wd,
                            mask,
                            cookie: 0,
                            name: String::new(),
                        });
                    }
                    events.push(QueuedEvent {
                        wd: *wd,
                        mask: IN_IGNORED,
                        cookie: 0,
                        name: String::new(),
                    });
                    removed.push(*wd);
                }
            }
            for wd in removed {
                state.watches.remove(&wd);
            }
            let queued = !events.is_empty();
            state.events.extend(events);
            queued
        };
        if queued {
            self.poll_rx.wake();
        }
    }

    fn pop_event(&self, dst: &mut IoDst) -> AxResult<usize> {
        let (event, name_len, event_size) = {
            let mut state = self.state.lock();
            let event = state.events.front().ok_or(AxError::WouldBlock)?;
            let name_len = if event.name.is_empty() {
                0
            } else {
                (event.name.len() + 1).next_multiple_of(align_of::<u32>())
            };
            let event_size = EVENT_HEADER_SIZE + name_len;
            if dst.remaining_mut() < event_size {
                return Err(AxError::InvalidInput);
            }
            let event = state.events.pop_front().ok_or(AxError::WouldBlock)?;
            (event, name_len, event_size)
        };

        dst.write(&event.wd.to_ne_bytes())?;
        dst.write(&event.mask.to_ne_bytes())?;
        dst.write(&event.cookie.to_ne_bytes())?;
        dst.write(&(name_len as u32).to_ne_bytes())?;
        if name_len > 0 {
            dst.write(event.name.as_bytes())?;
            dst.write(&[0])?;
            for _ in event.name.len() + 1..name_len {
                dst.write(&[0])?;
            }
        }
        Ok(event_size)
    }
}

fn with_live_inotify_fds(mut f: impl FnMut(&Arc<InotifyFd>)) {
    let mut registry = INOTIFY_FDS.lock();
    registry.retain(|weak| {
        if let Some(inotify) = weak.upgrade() {
            f(&inotify);
            true
        } else {
            false
        }
    });
}

pub fn next_event_cookie() -> u32 {
    NEXT_COOKIE.fetch_add(1, Ordering::AcqRel)
}

pub fn notify_path_event(path: &str, mask: u32, cookie: u32) {
    with_live_inotify_fds(|inotify| inotify.queue_path_event(path, mask, cookie));
}

pub fn notify_child_event(parent_path: &str, name: &str, mask: u32, cookie: u32) {
    with_live_inotify_fds(|inotify| inotify.queue_child_event(parent_path, name, mask, cookie));
}

pub fn notify_file_modified(path: &str) {
    notify_path_event(path, IN_MODIFY, 0);
    if let Some((parent, name)) = split_parent_name(path) {
        notify_child_event(parent, name, IN_MODIFY, 0);
    }
}

pub fn notify_opened(path: &str, is_dir: bool) {
    let mask = IN_OPEN | if is_dir { IN_ISDIR } else { 0 };
    notify_path_event(path, mask, 0);
    if let Some((parent, name)) = split_parent_name(path) {
        notify_child_event(parent, name, mask, 0);
    }
}

pub fn notify_closed(path: &str, writable: bool, is_dir: bool) {
    let mask = (if writable {
        IN_CLOSE_WRITE
    } else {
        IN_CLOSE_NOWRITE
    }) | if is_dir { IN_ISDIR } else { 0 };
    notify_path_event(path, mask, 0);
    if let Some((parent, name)) = split_parent_name(path) {
        notify_child_event(parent, name, mask, 0);
    }
}

pub fn notify_attrib(path: &str) {
    notify_path_event(path, IN_ATTRIB, 0);
    if let Some((parent, name)) = split_parent_name(path) {
        notify_child_event(parent, name, IN_ATTRIB, 0);
    }
}

pub fn notify_deleted(path: &str, is_dir: bool) {
    let self_mask = IN_DELETE_SELF | if is_dir { IN_ISDIR } else { 0 };
    with_live_inotify_fds(|inotify| inotify.queue_deleted_event(path, self_mask));
}

fn split_parent_name(path: &str) -> Option<(&str, &str)> {
    let path = path.trim_end_matches('/');
    if path.is_empty() || path == "/" {
        return None;
    }
    let index = path.rfind('/')?;
    let parent = if index == 0 { "/" } else { &path[..index] };
    let name = &path[index + 1..];
    (!name.is_empty()).then_some((parent, name))
}

impl FileLike for InotifyFd {
    fn read(&self, dst: &mut IoDst) -> ax_io::Result<usize> {
        block_on(poll_io(self, IoEvents::IN, self.nonblocking(), || {
            self.pop_event(dst)
        }))
    }

    fn write(&self, _src: &mut IoSrc) -> ax_io::Result<usize> {
        Err(AxError::InvalidInput)
    }

    fn nonblocking(&self) -> bool {
        self.non_blocking.load(Ordering::Acquire)
    }

    fn set_nonblocking(&self, non_blocking: bool) -> ax_io::Result {
        self.non_blocking.store(non_blocking, Ordering::Release);
        Ok(())
    }

    fn path(&self) -> Cow<'_, str> {
        "anon_inode:inotify".into()
    }
}

impl Pollable for InotifyFd {
    fn poll(&self) -> IoEvents {
        let state = self.state.lock();
        if state.events.is_empty() {
            IoEvents::empty()
        } else {
            IoEvents::IN
        }
    }

    fn register(&self, context: &mut Context<'_>, events: IoEvents) {
        if events.contains(IoEvents::IN) {
            self.poll_rx.register(context.waker());
        }
    }
}
