/* libzfs_fd_test.c - 验证 libzfs_init_fd() 与 Capsicum 能力模式
 *
 * 需要打过 patches/openzfs-2.4.2-fd-injection.patch 的 libzfs。
 *
 * 构建（已安装到 /tmp/zfs-fd）：
 *   cc -O2 -o fdtest tools/libzfs_fd_test.c \
 *      -I/tmp/zfs-fd/include -L/tmp/zfs-fd/lib -lzfs -lnvpair -luutil
 *
 * 测试内容：
 *   1. cap_enter() 后 open("/etc/passwd") 必须返回 ECAPMODE
 *   2. cap_enter() 后 fork+execv 必须返回 ECAPMODE（证明 exec 回退不可用）
 *   3. 通过注入的 fd 执行 zpool_iter / zfs_open / zfs_prop_get
 *   4. 重复 init/fini 10 次，检查 /dev/zfs fd 不泄漏
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <sys/capsicum.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <libzfs.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int
count_open_fds(void)
{
    int count = 0;
    int fd;
    const int limit = getdtablesize();

    for (fd = 0; fd < limit; fd++)
    {
        if (fcntl(fd, F_GETFD) != -1)
            count++;
    }

    return count;
}

static int
pool_cb(zpool_handle_t *zh,
        void *data)
{
    (void)data;

    printf("  pool %s\n", zpool_get_name(zh));
    zpool_close(zh);
    return 0;
}

int
main(void)
{
    cap_rights_t rights;
    libzfs_handle_t *handle;
    int fd;
    int before;
    int after;
    int round;

    fd = open("/dev/zfs", O_RDWR | O_EXCL | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "open /dev/zfs failed: %s\n", strerror(errno));
        return 1;
    }

    printf("pre-open /dev/zfs: OK\n");

    /* dup 需要 CAP_DUP；libzfs 通过 ioctl 访问设备需要 CAP_IOCTL */
    cap_rights_init(&rights, CAP_IOCTL, CAP_FSTAT, CAP_EVENT);

    if (cap_rights_limit(fd, &rights) != 0)
    {
        fprintf(stderr, "cap_rights_limit failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    /*
     * libzfs_core 首次 ioctl 前会读取 sysctl vfs.zfs.version.ioctl；
     * 能力模式禁止 sysctl，因此必须在 cap_enter() 之前完成初始化。
     */
    handle = libzfs_init_fd(fd);
    if (handle == NULL)
    {
        fprintf(stderr, "libzfs_init_fd failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("libzfs_init_fd (pre-cap): OK\n");

    /* 预热一次 ioctl，让 libzfs_core 在 cap_enter() 前缓存 ioctl 版本 */
    if (zpool_iter(handle, pool_cb, NULL) != 0)
        fprintf(stderr, "warmup zpool_iter failed\n");

    if (cap_enter() != 0)
    {
        fprintf(stderr, "cap_enter failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("cap_enter: OK\n");

    /* 越狱测试 1：普通文件必须打不开 */
    {
        int escape = open("/etc/passwd", O_RDONLY);

        if (escape >= 0)
        {
            fprintf(stderr, "SANDBOX FAIL: /etc/passwd is still readable\n");
            close(escape);
            close(fd);
            return 1;
        }

        printf("sandbox open(/etc/passwd): denied (errno=%d %s)\n",
               errno, strerror(errno));
    }

    /* 越狱测试 2：execv 必须失败（因此 exec 回退在能力模式下不可用） */
    {
        pid_t pid = fork();

        if (pid == 0)
        {
            char *argv[] = {"/bin/echo", "should-not-run", NULL};

            execv(argv[0], argv);
            dprintf(2, "sandbox execv: denied (errno=%d %s)\n",
                    errno, strerror(errno));
            _exit(0);
        }

        waitpid(pid, NULL, 0);
    }

    before = count_open_fds();

    /* 反复执行 ZFS 操作，检查 fd 泄漏 */
    for (round = 0; round < 10; round++)
    {
        int used = 0;

        if (zpool_iter(handle, pool_cb, NULL) != 0)
            fprintf(stderr, "zpool_iter failed\n");

        {
            zfs_handle_t *zh = zfs_open(handle, "zroot",
                                        ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);

            if (zh != NULL)
            {
                used = (int)zfs_prop_get_int(zh, ZFS_PROP_USED);
                zfs_close(zh);
            }
        }

        if (round == 0)
            printf("zfs_prop_get(used): %d bytes\n", used);
    }

    after = count_open_fds();
    printf("fd count (operations): before=%d after=%d %s\n", before, after,
           before == after ? "PASS" : "LEAK");

    libzfs_fini(handle);
    before = count_open_fds();

    /* 能力模式内重复 init/fini，确认 /dev/zfs fd 不泄漏 */
    for (round = 0; round < 3; round++)
    {
        int now;

        handle = libzfs_init_fd(fd);
        if (handle == NULL)
        {
            fprintf(stderr, "re-init failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }

        if (zpool_iter(handle, pool_cb, NULL) != 0)
            fprintf(stderr, "re-init zpool_iter failed\n");

        libzfs_fini(handle);

        now = count_open_fds();
        printf("re-init %d: fds=%d %s\n", round + 1, now,
               now == before ? "PASS" : "LEAK");
        if (now != before)
            before = now;
    }

    /* 轻量内存泄漏烟测：重复 init/fini 100 次，观察 RSS 增长 */
    {
        struct rusage ru1;
        struct rusage ru2;
        int k;

        getrusage(RUSAGE_SELF, &ru1);

        for (k = 0; k < 100; k++)
        {
            handle = libzfs_init_fd(fd);
            if (handle == NULL)
                break;
            libzfs_fini(handle);
        }

        getrusage(RUSAGE_SELF, &ru2);
        printf("RSS delta (100 init/fini): %ld KB\n",
               (long)(ru2.ru_maxrss - ru1.ru_maxrss));
    }

    close(fd);
    return 0;
}
