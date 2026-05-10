#!/bin/sh
set -eu

apk add \
    wayland-dev \
    wayland-protocols \
    weston \
    weston-backend-headless \
    weston-backend-drm \
    libinput-dev \
    mesa-gbm \
    mesa-egl \
    mesa-dri-gallium \
    xkeyboard-config
