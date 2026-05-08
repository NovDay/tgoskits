use alloc::{
    borrow::Cow,
    sync::{Arc, Weak},
};
use core::{
    sync::atomic::{AtomicBool, Ordering},
    task::Context,
    time::Duration,
};

use ax_errno::{AxError, AxResult};
use ax_hal::time::{TimeValue, monotonic_time, wall_time};
use ax_task::future::{block_on, poll_io, sleep};
use axpoll::{IoEvents, PollSet, Pollable};
use linux_raw_sys::general::{
    __kernel_clockid_t, CLOCK_BOOTTIME, CLOCK_MONOTONIC, CLOCK_REALTIME, itimerspec, timespec,
};
use spin::Mutex;

use crate::{
    file::{FileLike, IoDst, IoSrc},
    time::TimeValueLike,
};

#[derive(Debug, Clone, Copy)]
pub struct TimerSpec {
    pub interval: TimeValue,
    pub value: TimeValue,
}

impl TimerSpec {
    fn is_disarmed(&self) -> bool {
        self.value.is_zero()
    }
}

impl TryFrom<itimerspec> for TimerSpec {
    type Error = AxError;

    fn try_from(value: itimerspec) -> Result<Self, Self::Error> {
        Ok(Self {
            interval: value.it_interval.try_into_time_value()?,
            value: value.it_value.try_into_time_value()?,
        })
    }
}

impl From<TimerSpec> for itimerspec {
    fn from(value: TimerSpec) -> Self {
        Self {
            it_interval: timespec::from_time_value(value.interval),
            it_value: timespec::from_time_value(value.value),
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub enum TimerFdClock {
    Realtime,
    Monotonic,
    Boottime,
}

impl TimerFdClock {
    pub fn from_clock_id(clock_id: __kernel_clockid_t) -> AxResult<Self> {
        match clock_id as u32 {
            CLOCK_REALTIME => Ok(Self::Realtime),
            CLOCK_MONOTONIC => Ok(Self::Monotonic),
            CLOCK_BOOTTIME => Ok(Self::Boottime),
            _ => Err(AxError::InvalidInput),
        }
    }

    fn now(self) -> TimeValue {
        match self {
            Self::Realtime => wall_time(),
            Self::Monotonic | Self::Boottime => monotonic_time(),
        }
    }
}

#[derive(Debug, Clone, Copy)]
struct TimerFdState {
    generation: u64,
    interval: TimeValue,
    next_expiration: Option<TimeValue>,
    expirations: u64,
}

impl TimerFdState {
    fn spec(self, clock: TimerFdClock) -> TimerSpec {
        TimerSpec {
            interval: self.interval,
            value: self
                .next_expiration
                .map(|deadline| deadline.saturating_sub(clock.now()))
                .unwrap_or(TimeValue::ZERO),
        }
    }
}

pub struct TimerFd {
    clock: TimerFdClock,
    state: Mutex<TimerFdState>,
    non_blocking: AtomicBool,
    poll_rx: PollSet,
}

impl TimerFd {
    pub fn new(clock: TimerFdClock) -> Arc<Self> {
        Arc::new(Self {
            clock,
            state: Mutex::new(TimerFdState {
                generation: 0,
                interval: TimeValue::ZERO,
                next_expiration: None,
                expirations: 0,
            }),
            non_blocking: AtomicBool::new(false),
            poll_rx: PollSet::new(),
        })
    }

    pub fn settime(self: &Arc<Self>, spec: TimerSpec, absolute: bool) -> TimerSpec {
        let mut state = self.state.lock();
        self.update_expirations_locked(&mut state);
        let old = state.spec(self.clock);

        state.generation = state.generation.wrapping_add(1);
        state.interval = spec.interval;
        state.expirations = 0;
        state.next_expiration = if spec.is_disarmed() {
            None
        } else if absolute {
            Some(spec.value)
        } else {
            Some(self.clock.now() + spec.value)
        };

        let generation = state.generation;
        let deadline = state.next_expiration;
        drop(state);

        if deadline.is_some() {
            Self::spawn_waiter(self, generation);
        }
        self.poll_rx.wake();
        old
    }

    pub fn gettime(&self) -> TimerSpec {
        let mut state = self.state.lock();
        self.update_expirations_locked(&mut state);
        state.spec(self.clock)
    }

    fn spawn_waiter(this: &Arc<Self>, generation: u64) {
        let weak = Arc::downgrade(this);
        ax_task::spawn_with_name(
            move || block_on(timerfd_wait_loop(weak, generation)),
            "timerfd".into(),
        );
    }

    fn update_expirations_locked(&self, state: &mut TimerFdState) {
        let Some(next) = state.next_expiration else {
            return;
        };
        let now = self.clock.now();
        if now < next {
            return;
        }

        if state.interval.is_zero() {
            state.expirations = state.expirations.saturating_add(1);
            state.next_expiration = None;
            return;
        }

        let elapsed = now.saturating_sub(next);
        let interval_nanos = state.interval.as_nanos();
        let missed = elapsed.as_nanos() / interval_nanos + 1;
        let missed = missed.min(u64::MAX as u128) as u64;
        state.expirations = state.expirations.saturating_add(missed);
        state.next_expiration = Some(next + duration_mul(state.interval, missed));
    }
}

async fn timerfd_wait_loop(timerfd: Weak<TimerFd>, generation: u64) {
    loop {
        let Some(timerfd) = timerfd.upgrade() else {
            return;
        };

        let deadline = {
            let state = timerfd.state.lock();
            if state.generation != generation {
                return;
            }
            state.next_expiration
        };
        let Some(deadline) = deadline else {
            return;
        };

        sleep(deadline.saturating_sub(timerfd.clock.now())).await;

        let mut state = timerfd.state.lock();
        if state.generation != generation {
            return;
        }
        timerfd.update_expirations_locked(&mut state);
        let keep_waiting = state.next_expiration.is_some() && !state.interval.is_zero();
        drop(state);

        timerfd.poll_rx.wake();
        if !keep_waiting {
            return;
        }
    }
}

fn duration_mul(duration: Duration, count: u64) -> Duration {
    let nanos = duration
        .as_nanos()
        .saturating_mul(count as u128)
        .min(u64::MAX as u128) as u64;
    Duration::from_nanos(nanos)
}

impl FileLike for TimerFd {
    fn read(&self, dst: &mut IoDst) -> ax_io::Result<usize> {
        if dst.remaining_mut() < size_of::<u64>() {
            return Err(AxError::InvalidInput);
        }

        block_on(poll_io(self, IoEvents::IN, self.nonblocking(), || {
            let mut state = self.state.lock();
            self.update_expirations_locked(&mut state);
            if state.expirations == 0 {
                return Err(AxError::WouldBlock);
            }

            let expirations = state.expirations;
            state.expirations = 0;
            drop(state);

            dst.write(&expirations.to_ne_bytes())?;
            Ok(size_of::<u64>())
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
        "anon_inode:[timerfd]".into()
    }
}

impl Pollable for TimerFd {
    fn poll(&self) -> IoEvents {
        let mut state = self.state.lock();
        self.update_expirations_locked(&mut state);
        if state.expirations > 0 {
            IoEvents::IN
        } else {
            IoEvents::empty()
        }
    }

    fn register(&self, context: &mut Context<'_>, events: IoEvents) {
        if events.contains(IoEvents::IN) {
            self.poll_rx.register(context.waker());
        }
    }
}
