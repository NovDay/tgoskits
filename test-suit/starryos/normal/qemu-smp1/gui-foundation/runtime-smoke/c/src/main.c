#include <errno.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_text_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "FAIL: open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fprintf(stderr, "FAIL: malloc for %s\n", path);
        fclose(file);
        return NULL;
    }

    for (;;) {
        size_t n = fread(buf + len, 1, cap - len - 1, file);
        len += n;
        if (ferror(file)) {
            fprintf(stderr, "FAIL: read %s\n", path);
            free(buf);
            fclose(file);
            return NULL;
        }
        if (feof(file)) {
            break;
        }
        cap *= 2;
        char *next = realloc(buf, cap);
        if (next == NULL) {
            fprintf(stderr, "FAIL: realloc for %s\n", path);
            free(buf);
            fclose(file);
            return NULL;
        }
        buf = next;
    }

    buf[len] = '\0';
    fclose(file);
    return buf;
}

static int expect_contains(const char *text, const char *needle, const char *label) {
    if (strstr(text, needle) == NULL) {
        fprintf(stderr, "FAIL: %s missing %s\n", label, needle);
        return 1;
    }
    return 0;
}

static int test_proc_status(void) {
    char *status = read_text_file("/proc/self/status");
    if (status == NULL) {
        return 1;
    }

    int result = 0;
    const char *fields[] = {
        "Name:\t",
        "State:\t",
        "Tgid:\t",
        "Pid:\t",
        "PPid:\t",
        "Uid:\t",
        "Gid:\t",
        "FDSize:\t",
        "Groups:\t",
        "Threads:\t",
        "SigBlk:\t",
        "CapEff:\t",
        "NoNewPrivs:\t",
        "Seccomp:\t",
        "Cpus_allowed:\t",
        "Cpus_allowed_list:\t",
        "Mems_allowed:\t",
        "Mems_allowed_list:\t",
        "voluntary_ctxt_switches:\t",
        "nonvoluntary_ctxt_switches:\t",
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (expect_contains(status, fields[i], "/proc/self/status") != 0) {
            result = 1;
        }
    }

    free(status);
    return result;
}

static int test_proc_mounts(void) {
    char *mounts = read_text_file("/proc/mounts");
    if (mounts == NULL) {
        return 1;
    }

    int result = 0;
    const char *entries[] = {
        "rootfs / rootfs rw 0 0\n",
        "tmpfs /dev tmpfs rw,nosuid 0 0\n",
        "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n",
        "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n",
        "tmpfs /run tmpfs rw,nosuid,nodev 0 0\n",
        "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n",
        "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n",
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (expect_contains(mounts, entries[i], "/proc/mounts") != 0) {
            result = 1;
        }
    }

    free(mounts);
    return result;
}

static int check_dir_mode(const char *path, mode_t expected) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "FAIL: stat %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "FAIL: %s is not a directory mode=%#o\n", path,
                (unsigned)st.st_mode);
        return 1;
    }
    if ((st.st_mode & 07777) != expected) {
        fprintf(stderr, "FAIL: %s mode=%#o expected=%#o\n", path,
                (unsigned)(st.st_mode & 07777), (unsigned)expected);
        return 1;
    }
    if (st.st_uid != 0 || st.st_gid != 0) {
        fprintf(stderr, "FAIL: %s owner uid=%u gid=%u\n", path, st.st_uid, st.st_gid);
        return 1;
    }
    return 0;
}

static int test_runtime_dirs(void) {
    if (check_dir_mode("/tmp", 01777) != 0) {
        return 1;
    }
    if (check_dir_mode("/dev/shm", 01777) != 0) {
        return 1;
    }
    if (check_dir_mode("/run", 0755) != 0) {
        return 1;
    }
    if (check_dir_mode("/run/user", 0755) != 0) {
        return 1;
    }
    if (check_dir_mode("/run/user/0", 0700) != 0) {
        return 1;
    }

    if (setenv("XDG_RUNTIME_DIR", "/run/user/0", 1) != 0) {
        fprintf(stderr, "FAIL: setenv XDG_RUNTIME_DIR: %s\n", strerror(errno));
        return 1;
    }
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == NULL || strcmp(runtime_dir, "/run/user/0") != 0) {
        fprintf(stderr, "FAIL: getenv XDG_RUNTIME_DIR returned %s\n",
                runtime_dir == NULL ? "(null)" : runtime_dir);
        return 1;
    }

    return 0;
}

static int test_gui_runtime_syscalls(void) {
    if (prctl(PR_CAPBSET_READ, CAP_SYS_NICE, 0, 0, 0) != 1) {
        fprintf(stderr, "FAIL: prctl(PR_CAPBSET_READ, CAP_SYS_NICE): %s\n",
                strerror(errno));
        return 1;
    }
    errno = 0;
    if (prctl(PR_CAPBSET_READ, 1024, 0, 0, 0) != -1 || errno != EINVAL) {
        fprintf(stderr, "FAIL: prctl(PR_CAPBSET_READ invalid cap) errno=%d\n", errno);
        return 1;
    }

    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) != 0 || errno != 0) {
        fprintf(stderr, "FAIL: getpriority(PRIO_PROCESS): value/errno mismatch errno=%d\n",
                errno);
        return 1;
    }
    if (setpriority(PRIO_PROCESS, 0, 1) != 0) {
        fprintf(stderr, "FAIL: setpriority(PRIO_PROCESS): %s\n", strerror(errno));
        return 1;
    }
    if (setpriority(PRIO_PROCESS, 0, 20) != 0) {
        fprintf(stderr, "FAIL: setpriority clamped high prio: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}

int main(void) {
    if (test_proc_status() != 0) {
        return 1;
    }
    if (test_proc_mounts() != 0) {
        return 1;
    }
    if (test_runtime_dirs() != 0) {
        return 1;
    }
    if (test_gui_runtime_syscalls() != 0) {
        return 1;
    }

    printf("/proc status, mounts and runtime directory tests passed\n");
    printf("All runtime smoke tests passed!\n");
    return 0;
}
