/* libzfs_fd_test.c - 验证 libzfs_init_fd() 与 Capsicum 能力模式
 *
 * 需要打过 patches/openzfs-2.4.2-fd-injection.patch 的 libzfs。
 *
 * 构建（已安装头部/库）：
 *   cc -O2 -o fdtest tools/libzfs_fd_test.c \
 *      $(pkg-config --cflags --libs libzfs)
 *
 * 运行（普通用户即可，本机 /dev/zfs 需可读；root 也可）：
 *   ./fdtest
 *
 * 预期输出：
 *   pre-open /dev/zfs: OK
 *   cap_enter: OK
 *   libzfs_init_fd: OK
 *   pool zroot
 *   ...
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <sys/capsicum.h>

#include <errno.h>
#include <fcntl.h>
#include <libzfs.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
pool_cb(zpool_handle_t *zh,
        void *data)
{
    (void)data;

    printf("pool %s\n", zpool_get_name(zh));
    zpool_close(zh);
    return 0;
}

int
main(void)
{
    cap_rights_t rights;
    libzfs_handle_t *handle;
    int fd;

    fd = open("/dev/zfs", O_RDWR | O_EXCL | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "open /dev/zfs failed: %s\n", strerror(errno));
        return 1;
    }

    printf("pre-open /dev/zfs: OK\n");

    /* dup 需要 CAP_DUP；libzfs 通过 ioctl 访问设备需要 CAP_IOCTL */
    cap_rights_init(&rights, CAP_IOCTL, CAP_DUP, CAP_FSTAT, CAP_EVENT);

    if (cap_rights_limit(fd, &rights) != 0)
    {
        fprintf(stderr, "cap_rights_limit failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (cap_enter() != 0)
    {
        fprintf(stderr, "cap_enter failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("cap_enter: OK\n");

    handle = libzfs_init_fd(fd);
    if (handle == NULL)
    {
        fprintf(stderr, "libzfs_init_fd failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("libzfs_init_fd: OK\n");

    if (zpool_iter(handle, pool_cb, NULL) != 0)
        fprintf(stderr, "zpool_iter failed\n");

    libzfs_fini(handle);
    close(fd);

    return 0;
}
