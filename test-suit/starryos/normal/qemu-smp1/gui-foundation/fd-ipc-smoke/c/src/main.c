#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int test_proc_self_fd(void) {
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open /dev/null: %s\n", strerror(errno));
        return 1;
    }

    char link_path[64];
    snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);

    char target[256];
    ssize_t len = readlink(link_path, target, sizeof(target) - 1);
    if (len <= 0) {
        fprintf(stderr, "FAIL: readlink %s: %s\n", link_path, strerror(errno));
        close(fd);
        return 1;
    }
    target[len] = '\0';
    if (strstr(target, "/dev/null") == NULL) {
        fprintf(stderr, "FAIL: %s target=%s does not name /dev/null\n", link_path, target);
        close(fd);
        return 1;
    }

    struct stat st;
    if (fstatat(AT_FDCWD, link_path, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        fprintf(stderr, "FAIL: fstatat symlink %s: %s\n", link_path, strerror(errno));
        close(fd);
        return 1;
    }
    if (!S_ISLNK(st.st_mode)) {
        fprintf(stderr, "FAIL: %s mode=%#o is not symlink\n", link_path, st.st_mode);
        close(fd);
        return 1;
    }

    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL) {
        fprintf(stderr, "FAIL: opendir /proc/self/fd: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    bool found = false;
    char fd_name[32];
    snprintf(fd_name, sizeof(fd_name), "%d", fd);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (strcmp(entry->d_name, fd_name) == 0) {
            found = true;
            break;
        }
    }
    if (errno != 0) {
        fprintf(stderr, "FAIL: readdir /proc/self/fd: %s\n", strerror(errno));
        closedir(dir);
        close(fd);
        return 1;
    }
    closedir(dir);
    if (!found) {
        fprintf(stderr, "FAIL: /proc/self/fd did not list fd %d\n", fd);
        close(fd);
        return 1;
    }

    int status = fcntl(fd, F_GETFL);
    if (status < 0 || (status & O_NONBLOCK) == 0) {
        fprintf(stderr, "FAIL: F_GETFL status=%#x errno=%s\n", status, strerror(errno));
        close(fd);
        return 1;
    }
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) == 0) {
        fprintf(stderr, "FAIL: F_GETFD flags=%#x errno=%s\n", fd_flags, strerror(errno));
        close(fd);
        return 1;
    }

    int enable = 0;
    if (ioctl(fd, FIONBIO, &enable) != 0) {
        fprintf(stderr, "FAIL: FIONBIO disable: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    status = fcntl(fd, F_GETFL);
    if (status < 0 || (status & O_NONBLOCK) != 0) {
        fprintf(stderr, "FAIL: FIONBIO did not clear O_NONBLOCK status=%#x errno=%s\n", status,
                strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int test_scm_rights(void) {
    int socks[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socks) != 0) {
        fprintf(stderr, "FAIL: socketpair: %s\n", strerror(errno));
        return 1;
    }

    int sent_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (sent_fd < 0) {
        fprintf(stderr, "FAIL: open sent fd: %s\n", strerror(errno));
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    if (send(socks[0], "a", 1, 0) != 1) {
        fprintf(stderr, "FAIL: send prefix byte: %s\n", strerror(errno));
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    char byte = 'x';
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &sent_fd, sizeof(sent_fd));

    if (sendmsg(socks[0], &msg, 0) != 1) {
        fprintf(stderr, "FAIL: sendmsg SCM_RIGHTS: %s\n", strerror(errno));
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    char prefix_byte = 0;
    struct iovec prefix_iov = {.iov_base = &prefix_byte, .iov_len = 1};
    char prefix_control[CMSG_SPACE(sizeof(int))];
    memset(prefix_control, 0, sizeof(prefix_control));
    struct msghdr prefix_msg;
    memset(&prefix_msg, 0, sizeof(prefix_msg));
    prefix_msg.msg_iov = &prefix_iov;
    prefix_msg.msg_iovlen = 1;
    prefix_msg.msg_control = prefix_control;
    prefix_msg.msg_controllen = sizeof(prefix_control);

    if (recvmsg(socks[1], &prefix_msg, 0) != 1 || prefix_byte != 'a') {
        fprintf(stderr, "FAIL: recvmsg prefix byte=%c errno=%s\n", prefix_byte,
                strerror(errno));
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }
    if (CMSG_FIRSTHDR(&prefix_msg) != NULL) {
        fprintf(stderr, "FAIL: SCM_RIGHTS crossed stream data barrier\n");
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    if (send(socks[0], "tail", 4, 0) != 4) {
        fprintf(stderr, "FAIL: send tail bytes: %s\n", strerror(errno));
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    char recv_byte = 0;
    struct iovec recv_iov = {.iov_base = &recv_byte, .iov_len = 1};
    char recv_control[CMSG_SPACE(sizeof(int))];
    memset(recv_control, 0, sizeof(recv_control));
    struct msghdr recv_msg;
    memset(&recv_msg, 0, sizeof(recv_msg));
    recv_msg.msg_iov = &recv_iov;
    recv_msg.msg_iovlen = 1;
    recv_msg.msg_control = recv_control;
    recv_msg.msg_controllen = sizeof(recv_control);

    if (recvmsg(socks[1], &recv_msg, 0) != 1 || recv_byte != 'x') {
        fprintf(stderr, "FAIL: recvmsg SCM_RIGHTS byte=%c errno=%s\n", recv_byte,
                strerror(errno));
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    int received_fd = -1;
    for (struct cmsghdr *hdr = CMSG_FIRSTHDR(&recv_msg); hdr != NULL;
         hdr = CMSG_NXTHDR(&recv_msg, hdr)) {
        if (hdr->cmsg_level == SOL_SOCKET && hdr->cmsg_type == SCM_RIGHTS &&
            hdr->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&received_fd, CMSG_DATA(hdr), sizeof(received_fd));
            break;
        }
    }
    if (received_fd < 0) {
        fprintf(stderr, "FAIL: no received SCM_RIGHTS fd\n");
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    char tail[5] = {0};
    ssize_t tail_read = read(socks[1], tail, 4);
    if (tail_read != 4 || strcmp(tail, "tail") != 0) {
        fprintf(stderr, "FAIL: read SCM_RIGHTS tail ret=%zd tail=%s errno=%s\n", tail_read,
                tail, strerror(errno));
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        close(received_fd);
        return 1;
    }

    char probe;
    ssize_t n = read(received_fd, &probe, 1);
    if (n != 0) {
        fprintf(stderr, "FAIL: received fd is not readable /dev/null ret=%zd errno=%s\n", n,
                strerror(errno));
        close(received_fd);
        close(sent_fd);
        close(socks[0]);
        close(socks[1]);
        return 1;
    }

    close(received_fd);
    close(sent_fd);
    close(socks[0]);
    close(socks[1]);
    return 0;
}

int main(void) {
    if (test_proc_self_fd() != 0) {
        return 1;
    }
    if (test_scm_rights() != 0) {
        return 1;
    }

    printf("fd, procfs, fcntl, ioctl and SCM_RIGHTS tests passed\n");
    printf("All fd/ipc smoke tests passed!\n");
    return 0;
}
