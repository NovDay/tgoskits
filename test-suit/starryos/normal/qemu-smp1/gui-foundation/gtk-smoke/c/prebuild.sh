#!/bin/sh
set -eu

apk add gtk4.0-dev

/bin/mkdir -p \
    "${STARRY_CASE_OVERLAY_DIR}/etc" \
    "${STARRY_CASE_OVERLAY_DIR}/usr/share/X11" \
    "${STARRY_CASE_OVERLAY_DIR}/usr/share"

/bin/cp -R -L "${STARRY_STAGING_ROOT}/etc/fonts" "${STARRY_CASE_OVERLAY_DIR}/etc/"
/bin/cp -R -L "${STARRY_STAGING_ROOT}/usr/share/fontconfig" "${STARRY_CASE_OVERLAY_DIR}/usr/share/"
/bin/cp -R -L "${STARRY_STAGING_ROOT}/usr/share/X11/xkb" "${STARRY_CASE_OVERLAY_DIR}/usr/share/X11/"
/bin/cp -R -L "${STARRY_STAGING_ROOT}/usr/share/X11/xkb" \
    "${STARRY_CASE_OVERLAY_DIR}/usr/share/xkeyboard-config-2"
