# Weston Headless

This directory contains the Stage 3 Weston compositor probe. It is enabled as a
separate QEMU case instead of being folded into `gui-foundation`, because Weston
and Mesa pull in a heavier runtime than the foundation probes.

The probe can build and start Weston with the headless backend and
`kiosk-shell`, then connect a real Wayland client, bind `wl_compositor`/`wl_shm`,
and commit an ARGB shm surface. Its default QEMU command runs the DRM backend
path. The DRM probe runs Weston with `LIBSEAT_BACKEND=noop` and verifies
libinput udev dispatch, registry globals, `wl_shm` formats, xdg-shell toplevel
configure, shm surface commit, frame callback completion, and a `/dev/fb0` plus
CRTC `fb_id` output change after the commit.

The default `gui-foundation` gate covers the currently validated visible-output
substitute: `wayland-process-smoke` runs a controlled compositor, accepts a
client `wl_shm` surface commit, blits the shm buffer into `/dev/fb0`, and
checks framebuffer readback. Weston stays separate so the foundation gate
remains focused on smaller graphics, runtime, and protocol primitives.

By default `/usr/bin/weston-smoke` runs the headless backend probe. The enabled
QEMU case runs:

```sh
STARRY_WESTON_BACKEND=drm /usr/bin/weston-smoke
```

Useful optional knobs while narrowing the DRM path:

- `STARRY_WESTON_RENDERER=pixman` switches away from the default GL path.
- `STARRY_WESTON_REQUIRE_OUTPUT=1` also makes output/frame completion mandatory
  for non-DRM backend experiments. DRM backend runs require these checks by
  default.
- `STARRY_WESTON_DEBUG=1` enables Weston's `--debug` flag and records the
  resulting server log in `/tmp/weston.log`.
- `STARRY_WESTON_LOGGER_SCOPES=...` and
  `STARRY_WESTON_FLIGHT_REC_SCOPES=...` pass through Weston's native logging
  scope flags.
- `STARRY_WESTON_CONNECT_DELAY_MS=...` delays the client connect attempt.
- `STARRY_WESTON_CONNECT_RETRY_MS=...` changes how long the client retries
  connecting to the Weston socket.

Current known results:

- DRM + default GL renderer starts through Mesa GBM/EGL/llvmpipe, enables the
  virtual output, accepts a Wayland client, advertises globals and shm formats,
  accepts a shm surface commit, completes a frame callback, and changes both
  the framebuffer hash and CRTC `fb_id`.
- The probe intentionally does not package or launch `seatd`; full seat/session
  management remains a later GNOME/Mutter layer.
- DRM + `pixman` remains a useful alternate renderer probe.
- DRM + `noop` renderer exits early with `unsupported renderer for DRM backend`.
