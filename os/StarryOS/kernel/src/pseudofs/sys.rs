use alloc::{format, string::String, vec::Vec};

use ax_errno::LinuxResult;
use ax_fs::{FS_CONTEXT, File, FsContext};
use axfs_ng_vfs::{
    Location, MetadataUpdate, NodePermission,
    path::{Path, PathBuf},
};
use spin::{Lazy, Mutex};

const DIR_PERMISSION: NodePermission = NodePermission::from_bits_truncate(0o755);
const FILE_PERMISSION: NodePermission = NodePermission::from_bits_truncate(0o444);
const FRAMEBUFFER_MAJOR: u32 = 29;
const FRAMEBUFFER_MINOR: u32 = 0;
const DRM_MAJOR: u32 = 226;
const DRM_CARD0_MINOR: u32 = 0;

static INPUT_DEVICES: Lazy<Mutex<Vec<SysfsDevice>>> = Lazy::new(|| Mutex::new(Vec::new()));

#[derive(Clone)]
#[allow(dead_code)]
pub(crate) struct SysfsDevice {
    pub name: String,
    pub class: &'static str,
    pub devname: String,
    pub major: u32,
    pub minor: u32,
    pub parent_name: Option<String>,
}

pub(crate) fn framebuffer_device() -> Option<SysfsDevice> {
    ax_display::has_display().then(|| SysfsDevice {
        name: "fb0".into(),
        class: "graphics",
        devname: "fb0".into(),
        major: FRAMEBUFFER_MAJOR,
        minor: FRAMEBUFFER_MINOR,
        parent_name: None,
    })
}

pub(crate) fn drm_card0_device() -> Option<SysfsDevice> {
    #[cfg(all(feature = "rknpu", not(any(windows, unix))))]
    {
        None
    }
    #[cfg(not(all(feature = "rknpu", not(any(windows, unix)))))]
    {
        ax_display::has_display().then(|| SysfsDevice {
            name: "card0".into(),
            class: "drm",
            devname: "dri/card0".into(),
            major: DRM_MAJOR,
            minor: DRM_CARD0_MINOR,
            parent_name: None,
        })
    }
}

#[allow(dead_code)]
pub(crate) fn register_input_device(name: String, major: u32, minor: u32) {
    let mut devices = INPUT_DEVICES.lock();
    if let Some(device) = devices.iter_mut().find(|device| device.name == name) {
        device.major = major;
        device.minor = minor;
        return;
    }
    let parent_name = name
        .strip_prefix("event")
        .and_then(|idx| idx.parse::<usize>().ok())
        .map(|idx| format!("input{idx}"));
    devices.push(SysfsDevice {
        devname: format!("input/{name}"),
        name,
        class: "input",
        major,
        minor,
        parent_name,
    });
}

pub(crate) fn input_devices() -> Vec<SysfsDevice> {
    INPUT_DEVICES.lock().clone()
}

#[allow(dead_code)]
pub(crate) fn virtual_devices() -> Vec<SysfsDevice> {
    framebuffer_device()
        .into_iter()
        .chain(input_devices())
        .collect()
}

pub(crate) fn input_udev_properties(name: &str) -> &'static [&'static str] {
    if matches!(name, "mice" | "event0") {
        &["ID_INPUT=1", "ID_INPUT_MOUSE=1", "ID_SEAT=seat0"]
    } else if name == "event1" {
        &["ID_INPUT=1", "ID_INPUT_KEYBOARD=1", "ID_SEAT=seat0"]
    } else {
        &["ID_INPUT=1", "ID_SEAT=seat0"]
    }
}

fn ensure_dir(fs: &FsContext, path: &str) -> LinuxResult<Location> {
    if fs.resolve(path).is_err() {
        fs.create_dir(path, DIR_PERMISSION)?;
    }
    let loc = fs.resolve(path)?;
    loc.update_metadata(MetadataUpdate {
        mode: Some(DIR_PERMISSION),
        ..Default::default()
    })?;
    Ok(loc)
}

fn ensure_path_dirs(fs: &FsContext, path: &str) -> LinuxResult<()> {
    let mut cur = PathBuf::new();
    for comp in Path::new(path).components() {
        cur.push(comp.as_str());
        ensure_dir(fs, cur.as_str())?;
    }
    Ok(())
}

fn write_file(fs: &FsContext, path: &str, content: &str) -> LinuxResult<()> {
    let file = File::create(fs, path)?;
    file.write(content.as_bytes())?;
    file.location().update_metadata(MetadataUpdate {
        mode: Some(FILE_PERMISSION),
        ..Default::default()
    })?;
    Ok(())
}

#[allow(dead_code)]
fn write_uevent(
    fs: &FsContext,
    path: &str,
    devname: &str,
    major: u32,
    minor: u32,
) -> LinuxResult<()> {
    write_file(
        fs,
        path,
        &format!("MAJOR={major}\nMINOR={minor}\nDEVNAME={devname}\n"),
    )
}

fn write_udev_data(
    fs: &FsContext,
    major: u32,
    minor: u32,
    devname: &str,
    properties: &[&str],
    tags: &[&str],
) -> LinuxResult<()> {
    ensure_path_dirs(fs, "/run/udev/data")?;
    let mut content = format!("I:1\nE:DEVNAME=/dev/{devname}\n");
    for property in properties {
        content.push_str("E:");
        content.push_str(property);
        content.push('\n');
    }
    for tag in tags {
        content.push_str("G:");
        content.push_str(tag);
        content.push('\n');
        content.push_str("Q:");
        content.push_str(tag);
        content.push('\n');
    }
    content.push_str("V:1\n");
    write_file(fs, &format!("/run/udev/data/c{major}:{minor}"), &content)
}

#[allow(dead_code)]
fn ensure_symlink(fs: &FsContext, target: &str, path: &str) -> LinuxResult<()> {
    match fs.resolve_no_follow(path) {
        Ok(_) => Ok(()),
        Err(_) => Ok(fs.symlink(target, path).map(|_| ())?),
    }
}

#[allow(dead_code)]
fn ensure_dev_char_link(
    fs: &FsContext,
    major: u32,
    minor: u32,
    device_path: &str,
) -> LinuxResult<()> {
    ensure_path_dirs(fs, "/sys/dev/char")?;
    ensure_symlink(fs, device_path, &format!("/sys/dev/char/{major}:{minor}"))
}

#[allow(dead_code)]
fn populate_graphics(fs: &FsContext) -> LinuxResult<()> {
    if let Some(device) = framebuffer_device() {
        let major = device.major;
        let minor = device.minor;
        ensure_path_dirs(fs, "/sys/class/graphics/fb0")?;
        write_file(
            fs,
            "/sys/class/graphics/fb0/dev",
            &format!("{major}:{minor}\n"),
        )?;
        write_file(fs, "/sys/class/graphics/fb0/name", "fb0\n")?;
        write_uevent(fs, "/sys/class/graphics/fb0/uevent", "fb0", major, minor)?;
        ensure_symlink(
            fs,
            "../../../class/graphics",
            "/sys/class/graphics/fb0/subsystem",
        )?;
        ensure_symlink(
            fs,
            "../../../devices/virtual/graphics/fb0",
            "/sys/class/graphics/fb0/device",
        )?;

        ensure_path_dirs(fs, "/sys/devices/virtual/graphics/fb0")?;
        ensure_dev_char_link(fs, major, minor, "../../devices/virtual/graphics/fb0")?;
        write_file(
            fs,
            "/sys/devices/virtual/graphics/fb0/dev",
            &format!("{major}:{minor}\n"),
        )?;
        write_file(fs, "/sys/devices/virtual/graphics/fb0/name", "fb0\n")?;
        write_uevent(
            fs,
            "/sys/devices/virtual/graphics/fb0/uevent",
            "fb0",
            major,
            minor,
        )?;
        write_udev_data(fs, major, minor, &device.devname, &[], &["seat"])?;
        ensure_symlink(
            fs,
            "../../../../class/graphics",
            "/sys/devices/virtual/graphics/fb0/subsystem",
        )?;
    }
    Ok(())
}

#[allow(dead_code)]
fn populate_drm(fs: &FsContext) -> LinuxResult<()> {
    if let Some(device) = drm_card0_device() {
        let major = device.major;
        let minor = device.minor;
        ensure_path_dirs(fs, "/sys/class/drm/card0")?;
        write_file(
            fs,
            "/sys/class/drm/card0/dev",
            &format!("{major}:{minor}\n"),
        )?;
        write_file(fs, "/sys/class/drm/card0/name", "card0\n")?;
        write_uevent(
            fs,
            "/sys/class/drm/card0/uevent",
            &device.devname,
            major,
            minor,
        )?;
        ensure_symlink(fs, "../../../class/drm", "/sys/class/drm/card0/subsystem")?;
        ensure_symlink(
            fs,
            "../../../devices/virtual/drm/card0",
            "/sys/class/drm/card0/device",
        )?;

        ensure_path_dirs(fs, "/sys/devices/virtual/drm/card0")?;
        ensure_dev_char_link(fs, major, minor, "../../devices/virtual/drm/card0")?;
        write_file(
            fs,
            "/sys/devices/virtual/drm/card0/dev",
            &format!("{major}:{minor}\n"),
        )?;
        write_file(fs, "/sys/devices/virtual/drm/card0/name", "card0\n")?;
        write_uevent(
            fs,
            "/sys/devices/virtual/drm/card0/uevent",
            &device.devname,
            major,
            minor,
        )?;
        write_udev_data(
            fs,
            major,
            minor,
            &device.devname,
            &["ID_PATH=platform-starry-drm"],
            &["seat", "seat0", "master-of-seat"],
        )?;
        ensure_symlink(
            fs,
            "../../../../class/drm",
            "/sys/devices/virtual/drm/card0/subsystem",
        )?;
    }
    Ok(())
}

#[allow(dead_code)]
fn populate_input(fs: &FsContext) -> LinuxResult<()> {
    let input_devices = input_devices();
    if input_devices.is_empty() {
        return Ok(());
    }

    ensure_path_dirs(fs, "/sys/class/input")?;
    ensure_path_dirs(fs, "/sys/devices/virtual/input")?;

    for device in input_devices {
        let name = device.name.as_str();
        let major = device.major;
        let minor = device.minor;
        let class_dir = format!("/sys/class/input/{name}");
        let device_dir = input_sysfs_device_dir(&device);
        let parent_dir = device
            .parent_name
            .as_ref()
            .map(|parent| format!("/sys/devices/virtual/input/{parent}"));
        ensure_path_dirs(fs, &device_dir)?;
        if let (Some(parent), Some(parent_dir)) = (&device.parent_name, &parent_dir) {
            ensure_path_dirs(fs, parent_dir)?;
            write_file(fs, &format!("{parent_dir}/name"), &format!("{parent}\n"))?;
            write_file(
                fs,
                &format!("{parent_dir}/uevent"),
                &format!("PRODUCT=0/0/0/0\nNAME={parent}\n"),
            )?;
            ensure_symlink(
                fs,
                "../../../../class/input",
                &format!("{parent_dir}/subsystem"),
            )?;
        }
        ensure_dev_char_link(fs, major, minor, &dev_char_device_target(&device))?;
        write_file(
            fs,
            &format!("{device_dir}/dev"),
            &format!("{major}:{minor}\n"),
        )?;
        write_file(fs, &format!("{device_dir}/name"), &format!("{name}\n"))?;
        write_uevent(
            fs,
            &format!("{device_dir}/uevent"),
            &format!("input/{name}"),
            major,
            minor,
        )?;
        write_udev_data(
            fs,
            major,
            minor,
            &device.devname,
            input_udev_properties(name),
            &["seat", "seat0"],
        )?;
        ensure_symlink(fs, &class_input_entry_target(&device), &class_dir)?;
        ensure_symlink(
            fs,
            &input_device_parent_target(&device),
            &format!("{device_dir}/device"),
        )?;
        ensure_symlink(
            fs,
            &input_device_subsystem_target(&device),
            &format!("{device_dir}/subsystem"),
        )?;
    }

    Ok(())
}

pub(crate) fn input_sysfs_device_dir(device: &SysfsDevice) -> String {
    if device.class == "input" {
        if let Some(parent) = &device.parent_name {
            return format!("/sys/devices/virtual/input/{parent}/{}", device.name);
        }
    }
    format!("/sys/devices/virtual/{}/{}", device.class, device.name)
}

pub(crate) fn class_input_entry_target(device: &SysfsDevice) -> String {
    if device.class == "input" {
        if let Some(parent) = &device.parent_name {
            return format!("../../devices/virtual/input/{parent}/{}", device.name);
        }
    }
    format!("../../devices/virtual/{}/{}", device.class, device.name)
}

pub(crate) fn dev_char_device_target(device: &SysfsDevice) -> String {
    if device.class == "input" {
        if let Some(parent) = &device.parent_name {
            return format!("../../devices/virtual/input/{parent}/{}", device.name);
        }
    }
    format!("../../devices/virtual/{}/{}", device.class, device.name)
}

fn input_device_subsystem_target(device: &SysfsDevice) -> &'static str {
    if device.class == "input" && device.parent_name.is_some() {
        "../../../../../class/input"
    } else {
        "../../../../class/input"
    }
}

fn input_device_parent_target(device: &SysfsDevice) -> &'static str {
    if device.class == "input" && device.parent_name.is_some() {
        ".."
    } else {
        "."
    }
}

#[allow(dead_code)]
pub fn populate_udev_data() -> LinuxResult<()> {
    let fs = FS_CONTEXT.lock();
    if let Some(device) = framebuffer_device() {
        write_udev_data(
            &fs,
            device.major,
            device.minor,
            &device.devname,
            &[],
            &["seat"],
        )?;
    }
    if let Some(device) = drm_card0_device() {
        write_udev_data(
            &fs,
            device.major,
            device.minor,
            &device.devname,
            &["ID_PATH=platform-starry-drm"],
            &["seat", "seat0", "master-of-seat"],
        )?;
    }
    for device in input_devices() {
        write_udev_data(
            &fs,
            device.major,
            device.minor,
            &device.devname,
            input_udev_properties(&device.name),
            &["seat", "seat0"],
        )?;
    }
    Ok(())
}

#[allow(dead_code)]
pub fn populate_sysfs() -> LinuxResult<()> {
    let fs = FS_CONTEXT.lock();
    populate_graphics(&fs)?;
    populate_drm(&fs)?;
    populate_input(&fs)?;
    drop(fs);

    Ok(())
}
