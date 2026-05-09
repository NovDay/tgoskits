use alloc::{format, vec::Vec};

use ax_errno::LinuxResult;
use ax_fs::{FS_CONTEXT, File, FsContext};
use axfs_ng_vfs::{
    Location, MetadataUpdate, NodePermission,
    path::{Path, PathBuf},
};

const DIR_PERMISSION: NodePermission = NodePermission::from_bits_truncate(0o755);
const FILE_PERMISSION: NodePermission = NodePermission::from_bits_truncate(0o444);

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
) -> LinuxResult<()> {
    ensure_path_dirs(fs, "/run/udev/data")?;
    let mut content = format!("I:1\nE:DEVNAME=/dev/{devname}\n");
    for property in properties {
        content.push_str("E:");
        content.push_str(property);
        content.push('\n');
    }
    content.push_str("V:1\n");
    write_file(fs, &format!("/run/udev/data/c{major}:{minor}"), &content)
}

fn ensure_symlink(fs: &FsContext, target: &str, path: &str) -> LinuxResult<()> {
    match fs.resolve_no_follow(path) {
        Ok(_) => Ok(()),
        Err(_) => Ok(fs.symlink(target, path).map(|_| ())?),
    }
}

fn dev_numbers(fs: &FsContext, path: &str) -> Option<(u32, u32)> {
    let rdev = fs.resolve(path).ok()?.metadata().ok()?.rdev;
    Some((rdev.major(), rdev.minor()))
}

fn ensure_dev_char_link(
    fs: &FsContext,
    major: u32,
    minor: u32,
    device_path: &str,
) -> LinuxResult<()> {
    ensure_path_dirs(fs, "/sys/dev/char")?;
    ensure_symlink(fs, device_path, &format!("/sys/dev/char/{major}:{minor}"))
}

fn existing_input_nodes(fs: &FsContext) -> Vec<&'static str> {
    let candidates = ["event0", "event1", "event2", "event3", "mice"];
    candidates
        .into_iter()
        .filter(|name| fs.resolve(format!("/dev/input/{name}")).is_ok())
        .collect()
}

fn populate_graphics(fs: &FsContext) -> LinuxResult<()> {
    if let Some((major, minor)) = dev_numbers(fs, "/dev/fb0") {
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
        write_udev_data(fs, major, minor, "fb0", &[])?;
        ensure_symlink(
            fs,
            "../../../../class/graphics",
            "/sys/devices/virtual/graphics/fb0/subsystem",
        )?;
    }
    Ok(())
}

fn populate_input(fs: &FsContext) -> LinuxResult<()> {
    let input_nodes = existing_input_nodes(fs);
    if input_nodes.is_empty() {
        return Ok(());
    }

    ensure_path_dirs(fs, "/sys/class/input")?;
    ensure_path_dirs(fs, "/sys/devices/virtual/input")?;

    for name in input_nodes {
        let Some((major, minor)) = dev_numbers(fs, &format!("/dev/input/{name}")) else {
            continue;
        };
        let class_dir = format!("/sys/class/input/{name}");
        let device_dir = format!("/sys/devices/virtual/input/{name}");
        ensure_path_dirs(fs, &class_dir)?;
        ensure_path_dirs(fs, &device_dir)?;
        ensure_dev_char_link(
            fs,
            major,
            minor,
            &format!("../../devices/virtual/input/{name}"),
        )?;
        write_file(
            fs,
            &format!("{class_dir}/dev"),
            &format!("{major}:{minor}\n"),
        )?;
        write_file(fs, &format!("{class_dir}/name"), &format!("{name}\n"))?;
        write_uevent(
            fs,
            &format!("{class_dir}/uevent"),
            &format!("input/{name}"),
            major,
            minor,
        )?;
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
        let input_properties: &[&str] = if matches!(name, "mice" | "event0") {
            &["ID_INPUT=1", "ID_INPUT_MOUSE=1", "ID_SEAT=seat0"]
        } else if name == "event1" {
            &["ID_INPUT=1", "ID_INPUT_KEYBOARD=1", "ID_SEAT=seat0"]
        } else {
            &["ID_INPUT=1", "ID_SEAT=seat0"]
        };
        write_udev_data(fs, major, minor, &format!("input/{name}"), input_properties)?;
        ensure_symlink(
            fs,
            &format!("../../../devices/virtual/input/{name}"),
            &format!("{class_dir}/device"),
        )?;
        ensure_symlink(
            fs,
            "../../../class/input",
            &format!("{class_dir}/subsystem"),
        )?;
        ensure_symlink(
            fs,
            "../../../../class/input",
            &format!("{device_dir}/subsystem"),
        )?;
    }

    Ok(())
}

pub fn populate_sysfs() -> LinuxResult<()> {
    let fs = FS_CONTEXT.lock();
    populate_graphics(&fs)?;
    populate_input(&fs)?;
    drop(fs);

    Ok(())
}
