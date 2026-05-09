#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    uint8_t id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

static uint32_t pack_rgb(const struct fb_var_screeninfo *var, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t pixel = 0;
    pixel |= ((uint32_t)r >> (8 - var->red.length)) << var->red.offset;
    pixel |= ((uint32_t)g >> (8 - var->green.length)) << var->green.offset;
    pixel |= ((uint32_t)b >> (8 - var->blue.length)) << var->blue.offset;
    if (var->transp.length > 0) {
        pixel |= ((1u << var->transp.length) - 1u) << var->transp.offset;
    }
    return pixel;
}

static void put_pixel(uint8_t *fb, const struct fb_var_screeninfo *var,
                      const struct fb_fix_screeninfo *fix, uint32_t x, uint32_t y,
                      uint32_t pixel) {
    size_t offset = (size_t)y * fix->line_length + (size_t)x * (var->bits_per_pixel / 8);
    memcpy(fb + offset, &pixel, var->bits_per_pixel / 8);
}

static uint32_t get_pixel(const uint8_t *fb, const struct fb_var_screeninfo *var,
                          const struct fb_fix_screeninfo *fix, uint32_t x, uint32_t y) {
    uint32_t pixel = 0;
    size_t offset = (size_t)y * fix->line_length + (size_t)x * (var->bits_per_pixel / 8);
    memcpy(&pixel, fb + offset, var->bits_per_pixel / 8);
    return pixel;
}

static int test_offset_io(int fd, uint8_t *fb, const struct fb_var_screeninfo *var,
                          const struct fb_fix_screeninfo *fix) {
    if (var->xres < 4 || var->yres < 4) {
        fprintf(stderr, "FAIL: framebuffer too small for offset I/O test\n");
        return 1;
    }

    const size_t bytes_per_pixel = var->bits_per_pixel / 8;
    const uint32_t origin_pixel = pack_rgb(var, 0x10, 0x20, 0x30);
    const uint32_t target_pixel = pack_rgb(var, 0xf0, 0xe0, 0x30);
    const uint32_t write_pixel = pack_rgb(var, 0x40, 0xc0, 0xf0);
    const uint32_t target_x = 3;
    const uint32_t target_y = 2;
    const off_t target_offset =
        (off_t)((size_t)target_y * fix->line_length + (size_t)target_x * bytes_per_pixel);

    put_pixel(fb, var, fix, 0, 0, origin_pixel);
    put_pixel(fb, var, fix, target_x, target_y, target_pixel);

    ssize_t n = pwrite(fd, &write_pixel, bytes_per_pixel, target_offset);
    if (n != (ssize_t)bytes_per_pixel) {
        fprintf(stderr, "FAIL: pwrite /dev/fb0 at offset %ld: ret=%zd errno=%s\n",
                (long)target_offset, n, strerror(errno));
        return 1;
    }

    uint32_t origin_after = get_pixel(fb, var, fix, 0, 0);
    uint32_t target_after = get_pixel(fb, var, fix, target_x, target_y);
    if (origin_after != origin_pixel || target_after != write_pixel) {
        fprintf(stderr,
                "FAIL: framebuffer offset write mismatch origin=%#x target=%#x expected=%#x/%#x\n",
                origin_after, target_after, origin_pixel, write_pixel);
        return 1;
    }

    uint32_t read_pixel = 0;
    n = pread(fd, &read_pixel, bytes_per_pixel, target_offset);
    if (n != (ssize_t)bytes_per_pixel) {
        fprintf(stderr, "FAIL: pread /dev/fb0 at offset %ld: ret=%zd errno=%s\n",
                (long)target_offset, n, strerror(errno));
        return 1;
    }
    if (read_pixel != write_pixel) {
        fprintf(stderr, "FAIL: framebuffer offset read mismatch got=%#x expected=%#x\n",
                read_pixel, write_pixel);
        return 1;
    }

    return 0;
}

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open /dev/fb0: %s\n", strerror(errno));
        return 1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
        fprintf(stderr, "FAIL: FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        fprintf(stderr, "FAIL: FBIOGET_FSCREENINFO: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (var.xres == 0 || var.yres == 0 || fix.line_length == 0 || fix.smem_len == 0) {
        fprintf(stderr, "FAIL: invalid framebuffer geometry %ux%u line=%u size=%u\n",
                var.xres, var.yres, fix.line_length, fix.smem_len);
        close(fd);
        return 1;
    }
    if (var.bits_per_pixel != 32 && var.bits_per_pixel != 24) {
        fprintf(stderr, "FAIL: unsupported bpp %u\n", var.bits_per_pixel);
        close(fd);
        return 1;
    }

    uint8_t *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "FAIL: mmap /dev/fb0: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint32_t red = pack_rgb(&var, 0xff, 0x20, 0x20);
    uint32_t green = pack_rgb(&var, 0x20, 0xff, 0x20);
    uint32_t blue = pack_rgb(&var, 0x20, 0x60, 0xff);

    for (uint32_t y = 0; y < var.yres; y++) {
        for (uint32_t x = 0; x < var.xres; x++) {
            uint32_t pixel = x < var.xres / 3 ? red : (x < (var.xres * 2) / 3 ? green : blue);
            put_pixel(fb, &var, &fix, x, y, pixel);
        }
    }

    uint32_t left = get_pixel(fb, &var, &fix, 0, 0);
    uint32_t middle = get_pixel(fb, &var, &fix, var.xres / 2, var.yres / 2);
    uint32_t right = get_pixel(fb, &var, &fix, var.xres - 1, var.yres - 1);
    if (left != red || middle != green || right != blue) {
        fprintf(stderr, "FAIL: framebuffer readback mismatch left=%#x middle=%#x right=%#x\n",
                left, middle, right);
        munmap(fb, fix.smem_len);
        close(fd);
        return 1;
    }
    if (test_offset_io(fd, fb, &var, &fix) != 0) {
        munmap(fb, fix.smem_len);
        close(fd);
        return 1;
    }

    printf("Framebuffer demo drew %ux%u@%u color bars\n", var.xres, var.yres,
           var.bits_per_pixel);
    printf("Framebuffer offset read/write tests passed\n");
    printf("All framebuffer demo tests passed!\n");

    munmap(fb, fix.smem_len);
    close(fd);
    return 0;
}
