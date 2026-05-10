use alloc::{collections::VecDeque, vec::Vec};
use core::{any::Any, mem::size_of, slice, task::Context};

use ax_alloc::GlobalPage;
use ax_errno::{AxError, ax_err};
use ax_hal::{mem::virt_to_phys, time::monotonic_time};
use ax_memory_addr::{PAGE_SIZE_4K, PhysAddrRange, align_up_4k};
use ax_sync::Mutex;
use axfs_ng_vfs::{DeviceId, NodeFlags, VfsResult};
use axpoll::{IoEvents, PollSet, Pollable};
use starry_vm::{VmMutPtr, VmPtr, vm_write_slice};

use crate::pseudofs::{DeviceMmap, DeviceOps};

pub const CARD0_DEVICE_ID: DeviceId = DeviceId::new(226, 0);

const DRM_IOCTL_VERSION: u32 = 0xc040_6400;
const DRM_IOCTL_GET_CAP: u32 = 0xc010_640c;
const DRM_IOCTL_SET_CLIENT_CAP: u32 = 0x4010_640d;
const DRM_IOCTL_SET_MASTER: u32 = 0x641e;
const DRM_IOCTL_DROP_MASTER: u32 = 0x641f;
const DRM_IOCTL_MODE_GETRESOURCES: u32 = 0xc040_64a0;
const DRM_IOCTL_MODE_GETCRTC: u32 = 0xc068_64a1;
const DRM_IOCTL_MODE_SETCRTC: u32 = 0xc068_64a2;
const DRM_IOCTL_MODE_GETENCODER: u32 = 0xc014_64a6;
const DRM_IOCTL_MODE_GETCONNECTOR: u32 = 0xc050_64a7;
const DRM_IOCTL_MODE_GETPROPERTY: u32 = 0xc040_64aa;
const DRM_IOCTL_MODE_PAGE_FLIP: u32 = 0xc018_64b0;
const DRM_IOCTL_MODE_GETPLANERESOURCES: u32 = 0xc010_64b5;
const DRM_IOCTL_MODE_GETPLANE: u32 = 0xc020_64b6;
const DRM_IOCTL_MODE_ADDFB2: u32 = 0xc068_64b8;
const DRM_IOCTL_MODE_OBJ_GETPROPERTIES: u32 = 0xc020_64b9;
const DRM_IOCTL_MODE_CREATE_DUMB: u32 = 0xc020_64b2;
const DRM_IOCTL_MODE_MAP_DUMB: u32 = 0xc010_64b3;
const DRM_IOCTL_MODE_DESTROY_DUMB: u32 = 0xc004_64b4;

const DRM_CAP_DUMB_BUFFER: u64 = 0x1;
const DRM_CAP_TIMESTAMP_MONOTONIC: u64 = 0x6;
const DRM_CLIENT_CAP_UNIVERSAL_PLANES: u64 = 2;

const DRIVER_NAME: &[u8] = b"starrydrm";
const DRIVER_DATE: &[u8] = b"20260509";
const DRIVER_DESC: &[u8] = b"StarryOS framebuffer-backed DRM stub";

const CRTC_ID: u32 = 32;
const CONNECTOR_ID: u32 = 64;
const ENCODER_ID: u32 = 96;
const PLANE_ID: u32 = 112;
const PLANE_TYPE_PROPERTY_ID: u32 = 128;

const DRM_MODE_TYPE_PREFERRED: u32 = 1 << 3;
const DRM_MODE_TYPE_DRIVER: u32 = 1 << 6;
const DRM_MODE_PROP_IMMUTABLE: u32 = 1 << 2;
const DRM_MODE_PROP_ENUM: u32 = 1 << 3;
const DRM_MODE_CONNECTOR_VIRTUAL: u32 = 15;
const DRM_MODE_CONNECTED: u32 = 1;
const DRM_MODE_SUBPIXEL_UNKNOWN: u32 = 1;
const DRM_MODE_ENCODER_VIRTUAL: u32 = 5;
const DRM_MODE_OBJECT_CRTC: u32 = 0xcccc_cccc;
const DRM_MODE_OBJECT_CONNECTOR: u32 = 0xc0c0_c0c0;
const DRM_MODE_OBJECT_ENCODER: u32 = 0xe0e0_e0e0;
const DRM_MODE_OBJECT_PLANE: u32 = 0xeeee_eeee;
const DRM_MODE_OBJECT_ANY: u32 = 0;
const DRM_MODE_PAGE_FLIP_EVENT: u32 = 0x1;
const DRM_EVENT_FLIP_COMPLETE: u32 = 0x02;
const DRM_PLANE_TYPE_PRIMARY: u64 = 1;
const DRM_FORMAT_XRGB8888: u32 = fourcc_code(b'X', b'R', b'2', b'4');
const DRM_FORMAT_ARGB8888: u32 = fourcc_code(b'A', b'R', b'2', b'4');
const PLANE_FORMATS: &[u32] = &[DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888];
const MAX_DUMB_BUFFERS: usize = 8;
const MAX_FRAMEBUFFERS: usize = 8;
const MAX_DRM_EVENTS: usize = 16;

const fn fourcc_code(a: u8, b: u8, c: u8, d: u8) -> u32 {
    a as u32 | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
}

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

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmSetClientCap {
    capability: u64,
    value: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeCardRes {
    fb_id_ptr: u64,
    crtc_id_ptr: u64,
    connector_id_ptr: u64,
    encoder_id_ptr: u64,
    count_fbs: u32,
    count_crtcs: u32,
    count_connectors: u32,
    count_encoders: u32,
    min_width: u32,
    max_width: u32,
    min_height: u32,
    max_height: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeModeInfo {
    clock: u32,
    hdisplay: u16,
    hsync_start: u16,
    hsync_end: u16,
    htotal: u16,
    hskew: u16,
    vdisplay: u16,
    vsync_start: u16,
    vsync_end: u16,
    vtotal: u16,
    vscan: u16,
    vrefresh: u32,
    flags: u32,
    type_: u32,
    name: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeCrtc {
    set_connectors_ptr: u64,
    count_connectors: u32,
    crtc_id: u32,
    fb_id: u32,
    x: u32,
    y: u32,
    gamma_size: u32,
    mode_valid: u32,
    mode: DrmModeModeInfo,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeGetEncoder {
    encoder_id: u32,
    encoder_type: u32,
    crtc_id: u32,
    possible_crtcs: u32,
    possible_clones: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeGetConnector {
    encoders_ptr: u64,
    modes_ptr: u64,
    props_ptr: u64,
    prop_values_ptr: u64,
    count_modes: u32,
    count_props: u32,
    count_encoders: u32,
    encoder_id: u32,
    connector_id: u32,
    connector_type: u32,
    connector_type_id: u32,
    connection: u32,
    mm_width: u32,
    mm_height: u32,
    subpixel: u32,
    pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeGetProperty {
    values_ptr: u64,
    enum_blob_ptr: u64,
    prop_id: u32,
    flags: u32,
    name: [u8; 32],
    count_values: u32,
    count_enum_blobs: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModePropertyEnum {
    value: u64,
    name: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeGetPlaneRes {
    plane_id_ptr: u64,
    count_planes: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeGetPlane {
    plane_id: u32,
    crtc_id: u32,
    fb_id: u32,
    possible_crtcs: u32,
    gamma_size: u32,
    count_format_types: u32,
    format_type_ptr: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeObjGetProperties {
    props_ptr: u64,
    prop_values_ptr: u64,
    count_props: u32,
    obj_id: u32,
    obj_type: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeCreateDumb {
    height: u32,
    width: u32,
    bpp: u32,
    flags: u32,
    handle: u32,
    pitch: u32,
    size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeMapDumb {
    handle: u32,
    pad: u32,
    offset: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeDestroyDumb {
    handle: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeFbCmd2 {
    fb_id: u32,
    width: u32,
    height: u32,
    pixel_format: u32,
    flags: u32,
    handles: [u32; 4],
    pitches: [u32; 4],
    offsets: [u32; 4],
    modifier: [u64; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmModeCrtcPageFlip {
    crtc_id: u32,
    fb_id: u32,
    flags: u32,
    reserved: u32,
    user_data: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmEvent {
    type_: u32,
    length: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DrmEventVblank {
    base: DrmEvent,
    user_data: u64,
    tv_sec: u32,
    tv_usec: u32,
    sequence: u32,
    crtc_id: u32,
}

struct DumbBuffer {
    handle: u32,
    width: u32,
    height: u32,
    bpp: u32,
    pitch: u32,
    size: usize,
    pages: GlobalPage,
}

impl DumbBuffer {
    fn mmap_offset(&self) -> u64 {
        (self.handle as u64) << 12
    }

    fn range(&self) -> PhysAddrRange {
        PhysAddrRange::from_start_size(self.pages.start_paddr(virt_to_phys), self.pages.size())
    }
}

struct DrmFramebuffer {
    id: u32,
    handle: u32,
    width: u32,
    height: u32,
    pitch: u32,
    format: u32,
}

#[derive(Default)]
struct DrmState {
    next_handle: u32,
    next_fb_id: u32,
    current_fb_id: u32,
    sequence: u32,
    dumb_buffers: Vec<DumbBuffer>,
    framebuffers: Vec<DrmFramebuffer>,
    events: VecDeque<DrmEventVblank>,
}

static DRM_STATE: Mutex<DrmState> = Mutex::new(DrmState {
    next_handle: 1,
    next_fb_id: 128,
    current_fb_id: 0,
    sequence: 0,
    dumb_buffers: Vec::new(),
    framebuffers: Vec::new(),
    events: VecDeque::new(),
});
static DRM_POLL_RX: PollSet = PollSet::new();

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
    fn read_at(&self, buf: &mut [u8], _offset: u64) -> VfsResult<usize> {
        drm_read(buf)
    }

    fn write_at(&self, _buf: &[u8], _offset: u64) -> VfsResult<usize> {
        Err(AxError::InvalidInput)
    }

    fn ioctl(&self, cmd: u32, arg: usize) -> VfsResult<usize> {
        match cmd {
            DRM_IOCTL_VERSION => drm_version(arg),
            DRM_IOCTL_GET_CAP => drm_get_cap(arg),
            DRM_IOCTL_SET_CLIENT_CAP => drm_set_client_cap(arg),
            DRM_IOCTL_SET_MASTER | DRM_IOCTL_DROP_MASTER => Ok(0),
            DRM_IOCTL_MODE_GETRESOURCES => drm_mode_getresources(arg),
            DRM_IOCTL_MODE_GETCRTC => drm_mode_getcrtc(arg),
            DRM_IOCTL_MODE_SETCRTC => drm_mode_setcrtc(arg),
            DRM_IOCTL_MODE_GETENCODER => drm_mode_getencoder(arg),
            DRM_IOCTL_MODE_GETCONNECTOR => drm_mode_getconnector(arg),
            DRM_IOCTL_MODE_GETPROPERTY => drm_mode_getproperty(arg),
            DRM_IOCTL_MODE_PAGE_FLIP => drm_mode_page_flip(arg),
            DRM_IOCTL_MODE_GETPLANERESOURCES => drm_mode_getplaneresources(arg),
            DRM_IOCTL_MODE_GETPLANE => drm_mode_getplane(arg),
            DRM_IOCTL_MODE_ADDFB2 => drm_mode_addfb2(arg),
            DRM_IOCTL_MODE_OBJ_GETPROPERTIES => drm_mode_obj_getproperties(arg),
            DRM_IOCTL_MODE_CREATE_DUMB => drm_mode_create_dumb(arg),
            DRM_IOCTL_MODE_MAP_DUMB => drm_mode_map_dumb(arg),
            DRM_IOCTL_MODE_DESTROY_DUMB => drm_mode_destroy_dumb(arg),
            _ => Err(AxError::Unsupported),
        }
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_pollable(&self) -> Option<&dyn Pollable> {
        Some(self)
    }

    fn flags(&self) -> NodeFlags {
        NodeFlags::NON_CACHEABLE
    }

    fn mmap(&self, offset: u64) -> DeviceMmap {
        drm_mmap(offset)
    }
}

impl Pollable for DrmCard {
    fn poll(&self) -> IoEvents {
        let mut events = IoEvents::OUT;
        events.set(IoEvents::IN, !DRM_STATE.lock().events.is_empty());
        events
    }

    fn register(&self, context: &mut Context<'_>, events: IoEvents) {
        if events.contains(IoEvents::IN) {
            DRM_POLL_RX.register(context.waker());
        }
    }
}

fn drm_read(buf: &mut [u8]) -> VfsResult<usize> {
    if buf.len() < size_of::<DrmEvent>() {
        return Err(AxError::InvalidInput);
    }
    let mut state = DRM_STATE.lock();
    let Some(event) = state.events.front().copied() else {
        return Err(AxError::WouldBlock);
    };
    let event_size = size_of::<DrmEventVblank>();
    if buf.len() < event_size {
        return Err(AxError::InvalidInput);
    }
    state.events.pop_front();
    drop(state);

    let bytes = unsafe {
        slice::from_raw_parts((&event as *const DrmEventVblank).cast::<u8>(), event_size)
    };
    buf[..event_size].copy_from_slice(bytes);
    Ok(event_size)
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
        DRM_CAP_DUMB_BUFFER => 1,
        DRM_CAP_TIMESTAMP_MONOTONIC => 1,
        _ => 0,
    };
    (arg as *mut DrmGetCap).vm_write(cap)?;
    Ok(0)
}

fn drm_set_client_cap(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let cap = unsafe {
        (arg as *const DrmSetClientCap)
            .vm_read_uninit()?
            .assume_init()
    };
    match (cap.capability, cap.value) {
        (DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0 | 1) => Ok(0),
        _ => Err(AxError::InvalidInput),
    }
}

fn drm_mode_getresources(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut res = unsafe {
        (arg as *const DrmModeCardRes)
            .vm_read_uninit()?
            .assume_init()
    };
    let info = ax_display::framebuffer_info();

    if res.crtc_id_ptr != 0 && res.count_crtcs != 0 {
        vm_write_drm_ids(res.crtc_id_ptr, &[CRTC_ID], res.count_crtcs)?;
    }
    if res.connector_id_ptr != 0 && res.count_connectors != 0 {
        vm_write_drm_ids(res.connector_id_ptr, &[CONNECTOR_ID], res.count_connectors)?;
    }
    if res.encoder_id_ptr != 0 && res.count_encoders != 0 {
        vm_write_drm_ids(res.encoder_id_ptr, &[ENCODER_ID], res.count_encoders)?;
    }

    res.count_fbs = 0;
    res.count_crtcs = 1;
    res.count_connectors = 1;
    res.count_encoders = 1;
    res.min_width = 0;
    res.max_width = info.width;
    res.min_height = 0;
    res.max_height = info.height;
    (arg as *mut DrmModeCardRes).vm_write(res)?;
    Ok(0)
}

fn drm_mode_getcrtc(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut crtc = unsafe { (arg as *const DrmModeCrtc).vm_read_uninit()?.assume_init() };
    if crtc.crtc_id != CRTC_ID {
        return Err(AxError::InvalidInput);
    }
    crtc.fb_id = DRM_STATE.lock().current_fb_id;
    crtc.x = 0;
    crtc.y = 0;
    crtc.gamma_size = 0;
    crtc.mode_valid = 1;
    crtc.mode = current_modeinfo();
    (arg as *mut DrmModeCrtc).vm_write(crtc)?;
    Ok(0)
}

fn drm_mode_setcrtc(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let crtc = unsafe { (arg as *const DrmModeCrtc).vm_read_uninit()?.assume_init() };
    if crtc.crtc_id != CRTC_ID {
        return Err(AxError::InvalidInput);
    }
    if crtc.count_connectors != 1 || crtc.set_connectors_ptr == 0 {
        return Err(AxError::InvalidInput);
    }
    let connector_ptr = crtc.set_connectors_ptr as *const u32;
    let connector_id = unsafe { connector_ptr.vm_read_uninit()?.assume_init() };
    if connector_id != CONNECTOR_ID || crtc.x != 0 || crtc.y != 0 || crtc.mode_valid == 0 {
        return Err(AxError::InvalidInput);
    }

    let state = DRM_STATE.lock();
    let fb = state
        .framebuffers
        .iter()
        .find(|fb| fb.id == crtc.fb_id)
        .ok_or(AxError::InvalidInput)?;
    let buffer = state
        .dumb_buffers
        .iter()
        .find(|buffer| buffer.handle == fb.handle)
        .ok_or(AxError::InvalidInput)?;
    blit_framebuffer_to_display(fb, buffer)?;
    drop(state);

    DRM_STATE.lock().current_fb_id = crtc.fb_id;
    Ok(0)
}

fn drm_mode_page_flip(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let page_flip = unsafe {
        (arg as *const DrmModeCrtcPageFlip)
            .vm_read_uninit()?
            .assume_init()
    };
    if page_flip.crtc_id != CRTC_ID
        || page_flip.fb_id == 0
        || page_flip.reserved != 0
        || page_flip.flags & !DRM_MODE_PAGE_FLIP_EVENT != 0
    {
        return Err(AxError::InvalidInput);
    }

    let mut state = DRM_STATE.lock();
    if page_flip.flags & DRM_MODE_PAGE_FLIP_EVENT != 0 && state.events.len() >= MAX_DRM_EVENTS {
        return Err(AxError::WouldBlock);
    }
    let fb = state
        .framebuffers
        .iter()
        .find(|fb| fb.id == page_flip.fb_id)
        .ok_or(AxError::InvalidInput)?;
    let buffer = state
        .dumb_buffers
        .iter()
        .find(|buffer| buffer.handle == fb.handle)
        .ok_or(AxError::InvalidInput)?;
    blit_framebuffer_to_display(fb, buffer)?;

    state.current_fb_id = page_flip.fb_id;
    state.sequence = state.sequence.wrapping_add(1);
    if page_flip.flags & DRM_MODE_PAGE_FLIP_EVENT != 0 {
        let now = monotonic_time();
        let sequence = state.sequence;
        state.events.push_back(DrmEventVblank {
            base: DrmEvent {
                type_: DRM_EVENT_FLIP_COMPLETE,
                length: size_of::<DrmEventVblank>() as u32,
            },
            user_data: page_flip.user_data,
            tv_sec: now.as_secs() as u32,
            tv_usec: now.subsec_micros(),
            sequence,
            crtc_id: CRTC_ID,
        });
        DRM_POLL_RX.wake();
    }
    Ok(0)
}

fn drm_mode_getencoder(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut encoder = unsafe {
        (arg as *const DrmModeGetEncoder)
            .vm_read_uninit()?
            .assume_init()
    };
    if encoder.encoder_id != ENCODER_ID {
        return Err(AxError::InvalidInput);
    }
    encoder.encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    encoder.crtc_id = CRTC_ID;
    encoder.possible_crtcs = 1;
    encoder.possible_clones = 0;
    (arg as *mut DrmModeGetEncoder).vm_write(encoder)?;
    Ok(0)
}

fn drm_mode_getconnector(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut connector = unsafe {
        (arg as *const DrmModeGetConnector)
            .vm_read_uninit()?
            .assume_init()
    };
    if connector.connector_id != CONNECTOR_ID {
        return Err(AxError::InvalidInput);
    }

    if connector.encoders_ptr != 0 && connector.count_encoders != 0 {
        vm_write_drm_ids(
            connector.encoders_ptr,
            &[ENCODER_ID],
            connector.count_encoders,
        )?;
    }
    if connector.modes_ptr != 0 && connector.count_modes != 0 {
        vm_write_drm_modes(
            connector.modes_ptr,
            &[current_modeinfo()],
            connector.count_modes,
        )?;
    }

    connector.count_modes = 1;
    connector.count_props = 0;
    connector.count_encoders = 1;
    connector.encoder_id = ENCODER_ID;
    connector.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    connector.connector_type_id = 1;
    connector.connection = DRM_MODE_CONNECTED;
    connector.mm_width = 0;
    connector.mm_height = 0;
    connector.subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    connector.pad = 0;
    (arg as *mut DrmModeGetConnector).vm_write(connector)?;
    Ok(0)
}

fn drm_mode_getproperty(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut property = unsafe {
        (arg as *const DrmModeGetProperty)
            .vm_read_uninit()?
            .assume_init()
    };
    if property.prop_id != PLANE_TYPE_PROPERTY_ID {
        return Err(AxError::InvalidInput);
    }
    if property.values_ptr != 0 && property.count_values != 0 {
        vm_write_drm_values(
            property.values_ptr,
            &[0, DRM_PLANE_TYPE_PRIMARY, 2],
            property.count_values,
        )?;
    }
    if property.enum_blob_ptr != 0 && property.count_enum_blobs != 0 {
        vm_write_drm_property_enums(
            property.enum_blob_ptr,
            &[
                DrmModePropertyEnum::new(0, b"Overlay"),
                DrmModePropertyEnum::new(DRM_PLANE_TYPE_PRIMARY, b"Primary"),
                DrmModePropertyEnum::new(2, b"Cursor"),
            ],
            property.count_enum_blobs,
        )?;
    }
    property.flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
    property.name = nul_padded_name(b"type");
    property.count_values = 3;
    property.count_enum_blobs = 3;
    (arg as *mut DrmModeGetProperty).vm_write(property)?;
    Ok(0)
}

fn drm_mode_getplaneresources(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut resources = unsafe {
        (arg as *const DrmModeGetPlaneRes)
            .vm_read_uninit()?
            .assume_init()
    };
    if resources.plane_id_ptr != 0 && resources.count_planes != 0 {
        vm_write_drm_ids(resources.plane_id_ptr, &[PLANE_ID], resources.count_planes)?;
    }
    resources.count_planes = 1;
    (arg as *mut DrmModeGetPlaneRes).vm_write(resources)?;
    Ok(0)
}

fn drm_mode_getplane(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut plane = unsafe {
        (arg as *const DrmModeGetPlane)
            .vm_read_uninit()?
            .assume_init()
    };
    if plane.plane_id != PLANE_ID {
        return Err(AxError::InvalidInput);
    }
    if plane.format_type_ptr != 0 && plane.count_format_types != 0 {
        vm_write_drm_ids(
            plane.format_type_ptr,
            PLANE_FORMATS,
            plane.count_format_types,
        )?;
    }
    let state = DRM_STATE.lock();
    plane.crtc_id = CRTC_ID;
    plane.fb_id = state.current_fb_id;
    drop(state);
    plane.possible_crtcs = 1;
    plane.gamma_size = 0;
    plane.count_format_types = PLANE_FORMATS.len() as u32;
    (arg as *mut DrmModeGetPlane).vm_write(plane)?;
    Ok(0)
}

fn drm_mode_obj_getproperties(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut properties = unsafe {
        (arg as *const DrmModeObjGetProperties)
            .vm_read_uninit()?
            .assume_init()
    };
    if !is_known_drm_object(properties.obj_id, properties.obj_type) {
        return Err(AxError::InvalidInput);
    }
    if properties.obj_type == DRM_MODE_OBJECT_PLANE {
        if properties.props_ptr != 0 && properties.count_props != 0 {
            vm_write_drm_ids(
                properties.props_ptr,
                &[PLANE_TYPE_PROPERTY_ID],
                properties.count_props,
            )?;
        }
        if properties.prop_values_ptr != 0 && properties.count_props != 0 {
            vm_write_drm_values(
                properties.prop_values_ptr,
                &[DRM_PLANE_TYPE_PRIMARY],
                properties.count_props,
            )?;
        }
        properties.count_props = 1;
    } else {
        properties.count_props = 0;
    }
    (arg as *mut DrmModeObjGetProperties).vm_write(properties)?;
    Ok(0)
}

fn drm_mode_create_dumb(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut create = unsafe {
        (arg as *const DrmModeCreateDumb)
            .vm_read_uninit()?
            .assume_init()
    };
    if create.flags != 0 || create.width == 0 || create.height == 0 || create.bpp != 32 {
        return Err(AxError::InvalidInput);
    }
    let pitch = create
        .width
        .checked_mul(create.bpp / 8)
        .ok_or(AxError::InvalidInput)?;
    let size = (pitch as u64)
        .checked_mul(create.height as u64)
        .ok_or(AxError::InvalidInput)?;
    let alloc_size = align_up_4k(size as usize);
    let pages = GlobalPage::alloc_contiguous(alloc_size / PAGE_SIZE_4K, PAGE_SIZE_4K)?;
    let mut state = DRM_STATE.lock();
    if state.dumb_buffers.len() >= MAX_DUMB_BUFFERS {
        return Err(AxError::NoMemory);
    }
    let handle = state.next_handle;
    state.next_handle = state.next_handle.checked_add(1).ok_or(AxError::NoMemory)?;
    state.dumb_buffers.push(DumbBuffer {
        handle,
        width: create.width,
        height: create.height,
        bpp: create.bpp,
        pitch,
        size: alloc_size,
        pages,
    });

    create.handle = handle;
    create.pitch = pitch;
    create.size = alloc_size as u64;
    (arg as *mut DrmModeCreateDumb).vm_write(create)?;
    Ok(0)
}

fn drm_mode_map_dumb(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut map = unsafe {
        (arg as *const DrmModeMapDumb)
            .vm_read_uninit()?
            .assume_init()
    };
    let state = DRM_STATE.lock();
    let buffer = state
        .dumb_buffers
        .iter()
        .find(|buffer| buffer.handle == map.handle)
        .ok_or(AxError::InvalidInput)?;
    map.offset = buffer.mmap_offset();
    (arg as *mut DrmModeMapDumb).vm_write(map)?;
    Ok(0)
}

fn drm_mode_destroy_dumb(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let destroy = unsafe {
        (arg as *const DrmModeDestroyDumb)
            .vm_read_uninit()?
            .assume_init()
    };
    let mut state = DRM_STATE.lock();
    if state
        .framebuffers
        .iter()
        .any(|fb| fb.handle == destroy.handle && fb.id == state.current_fb_id)
    {
        return Err(AxError::InvalidInput);
    }
    state.framebuffers.retain(|fb| fb.handle != destroy.handle);
    let len = state.dumb_buffers.len();
    state
        .dumb_buffers
        .retain(|buffer| buffer.handle != destroy.handle);
    if state.dumb_buffers.len() == len {
        return Err(AxError::InvalidInput);
    }
    Ok(0)
}

fn drm_mode_addfb2(arg: usize) -> VfsResult<usize> {
    if arg == 0 {
        return Err(AxError::BadAddress);
    }
    let mut fb = unsafe {
        (arg as *const DrmModeFbCmd2)
            .vm_read_uninit()?
            .assume_init()
    };
    if fb.width == 0 || fb.height == 0 || fb.handles[0] == 0 || fb.offsets[0] != 0 {
        return Err(AxError::InvalidInput);
    }
    if fb.pixel_format != DRM_FORMAT_XRGB8888 && fb.pixel_format != DRM_FORMAT_ARGB8888 {
        return Err(AxError::InvalidInput);
    }
    if fb.handles[1..].iter().any(|handle| *handle != 0)
        || fb.pitches[1..].iter().any(|pitch| *pitch != 0)
        || fb.offsets[1..].iter().any(|offset| *offset != 0)
    {
        return Err(AxError::InvalidInput);
    }

    let mut state = DRM_STATE.lock();
    if state.framebuffers.len() >= MAX_FRAMEBUFFERS {
        return Err(AxError::NoMemory);
    }
    let (handle, pitch, bpp) = {
        let buffer = state
            .dumb_buffers
            .iter()
            .find(|buffer| buffer.handle == fb.handles[0])
            .ok_or(AxError::InvalidInput)?;
        if fb.width > buffer.width
            || fb.height > buffer.height
            || fb.pitches[0] != buffer.pitch
            || buffer.bpp != 32
        {
            return Err(AxError::InvalidInput);
        }
        (buffer.handle, buffer.pitch, buffer.bpp)
    };
    if bpp != 32 {
        return Err(AxError::InvalidInput);
    }
    let fb_id = state.next_fb_id;
    state.next_fb_id = state.next_fb_id.checked_add(1).ok_or(AxError::NoMemory)?;
    state.framebuffers.push(DrmFramebuffer {
        id: fb_id,
        handle,
        width: fb.width,
        height: fb.height,
        pitch,
        format: fb.pixel_format,
    });
    fb.fb_id = fb_id;
    (arg as *mut DrmModeFbCmd2).vm_write(fb)?;
    Ok(0)
}

fn current_modeinfo() -> DrmModeModeInfo {
    let info = ax_display::framebuffer_info();
    let width = info.width.min(u16::MAX as u32) as u16;
    let height = info.height.min(u16::MAX as u32) as u16;
    let hblank = (width / 16).max(32);
    let vblank = (height / 32).max(16);
    let hsync = (hblank / 2).max(8);
    let vsync = (vblank / 4).max(4);
    let hsync_start = width.saturating_add(hblank / 4);
    let hsync_end = hsync_start.saturating_add(hsync);
    let htotal = width.saturating_add(hblank);
    let vsync_start = height.saturating_add(vblank / 4);
    let vsync_end = vsync_start.saturating_add(vsync);
    let vtotal = height.saturating_add(vblank);
    let clock = (htotal as u32)
        .saturating_mul(vtotal as u32)
        .saturating_mul(60)
        / 1000;
    let mut name = [0; 32];
    write_mode_name(&mut name, info.width, info.height);
    DrmModeModeInfo {
        clock,
        hdisplay: width,
        hsync_start,
        hsync_end,
        htotal,
        hskew: 0,
        vdisplay: height,
        vsync_start,
        vsync_end,
        vtotal,
        vscan: 0,
        vrefresh: 60,
        flags: 0,
        type_: DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER,
        name,
    }
}

fn is_known_drm_object(id: u32, object_type: u32) -> bool {
    match object_type {
        DRM_MODE_OBJECT_ANY => {
            id == CRTC_ID || id == CONNECTOR_ID || id == ENCODER_ID || id == PLANE_ID
        }
        DRM_MODE_OBJECT_CRTC => id == CRTC_ID,
        DRM_MODE_OBJECT_CONNECTOR => id == CONNECTOR_ID,
        DRM_MODE_OBJECT_ENCODER => id == ENCODER_ID,
        DRM_MODE_OBJECT_PLANE => id == PLANE_ID,
        _ => false,
    }
}

impl DrmModePropertyEnum {
    const fn new(value: u64, name: &[u8]) -> Self {
        Self {
            value,
            name: nul_padded_name(name),
        }
    }
}

const fn nul_padded_name(name: &[u8]) -> [u8; 32] {
    let mut out = [0; 32];
    let mut idx = 0;
    while idx < name.len() && idx < out.len() {
        out[idx] = name[idx];
        idx += 1;
    }
    out
}

fn drm_mmap(offset: u64) -> DeviceMmap {
    let Some(handle) = map_handle_from_offset(offset) else {
        return DeviceMmap::None;
    };
    let state = DRM_STATE.lock();
    let Some(buffer) = state
        .dumb_buffers
        .iter()
        .find(|buffer| buffer.handle == handle)
    else {
        return DeviceMmap::None;
    };
    DeviceMmap::Physical(buffer.range())
}

fn map_handle_from_offset(offset: u64) -> Option<u32> {
    if offset & ((PAGE_SIZE_4K as u64) - 1) != 0 {
        return None;
    }
    let handle = u32::try_from(offset >> 12).ok()?;
    (handle != 0).then_some(handle)
}

fn blit_framebuffer_to_display(fb: &DrmFramebuffer, buffer: &DumbBuffer) -> VfsResult<()> {
    let info = ax_display::framebuffer_info();
    if fb.format != DRM_FORMAT_XRGB8888 && fb.format != DRM_FORMAT_ARGB8888 {
        return Err(AxError::InvalidInput);
    }
    let src_len = (fb.pitch as usize)
        .checked_mul(fb.height as usize)
        .ok_or(AxError::InvalidInput)?;
    if src_len > buffer.size {
        return Err(AxError::InvalidInput);
    }
    if fb.width > info.width || fb.height > info.height {
        return Err(AxError::InvalidInput);
    }
    let dst_pitch = info.fb_size / info.height as usize;
    let copy_bytes = (fb.width as usize)
        .checked_mul(4)
        .ok_or(AxError::InvalidInput)?;
    if copy_bytes > fb.pitch as usize || copy_bytes > dst_pitch {
        return Err(AxError::InvalidInput);
    }
    let src = buffer.pages.as_slice();
    let dst = unsafe { slice::from_raw_parts_mut(info.fb_base_vaddr as *mut u8, info.fb_size) };
    for y in 0..fb.height as usize {
        let src_start = y * fb.pitch as usize;
        let dst_start = y * dst_pitch;
        dst[dst_start..dst_start + copy_bytes]
            .copy_from_slice(&src[src_start..src_start + copy_bytes]);
    }
    if !ax_display::framebuffer_flush() {
        return ax_err!(InvalidInput);
    }
    Ok(())
}

fn vm_write_drm_ids(dst: u64, ids: &[u32], user_count: u32) -> VfsResult<()> {
    let copy_count = (user_count as usize).min(ids.len());
    Ok(vm_write_slice(dst as *mut u32, &ids[..copy_count])?)
}

fn vm_write_drm_values(dst: u64, values: &[u64], user_count: u32) -> VfsResult<()> {
    let copy_count = (user_count as usize).min(values.len());
    Ok(vm_write_slice(dst as *mut u64, &values[..copy_count])?)
}

fn vm_write_drm_property_enums(
    dst: u64,
    values: &[DrmModePropertyEnum],
    user_count: u32,
) -> VfsResult<()> {
    let copy_count = (user_count as usize).min(values.len());
    Ok(vm_write_slice(
        dst as *mut DrmModePropertyEnum,
        &values[..copy_count],
    )?)
}

fn vm_write_drm_modes(dst: u64, modes: &[DrmModeModeInfo], user_count: u32) -> VfsResult<()> {
    let copy_count = (user_count as usize).min(modes.len());
    Ok(vm_write_slice(
        dst as *mut DrmModeModeInfo,
        &modes[..copy_count],
    )?)
}

fn write_mode_name(dst: &mut [u8; 32], width: u32, height: u32) {
    let mut pos = 0;
    write_decimal(dst, &mut pos, width);
    if pos < dst.len() {
        dst[pos] = b'x';
        pos += 1;
    }
    write_decimal(dst, &mut pos, height);
}

fn write_decimal(dst: &mut [u8; 32], pos: &mut usize, value: u32) {
    let mut digits = [0; 10];
    let mut len = 0;
    let mut value = value;
    loop {
        digits[len] = b'0' + (value % 10) as u8;
        len += 1;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    while len > 0 && *pos < dst.len().saturating_sub(1) {
        len -= 1;
        dst[*pos] = digits[len];
        *pos += 1;
    }
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
