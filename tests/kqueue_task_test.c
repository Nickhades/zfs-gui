#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "zg-task.h"
static int sendf(int fd, const char *t)
{
    guint32 l = htonl((guint32)strlen(t));
    return write(fd, &l, 4) == 4 && write(fd, t, strlen(t)) == (ssize_t)strlen(t) ? 0 : -1;
}
static char *recvf(int fd)
{
    guint32 l;
    char *t;
    if (read(fd, &l, 4) != 4)
        return NULL;
    l = ntohl(l);
    t = calloc(1, l + 1);
    if (read(fd, t, l) != (ssize_t)l)
    {
        free(t);
        return NULL;
    }
    return t;
}
static int reqfd(int fd, int *ff)
{
    char b[1024], cb[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {b, sizeof(b)};
    struct msghdr m = {0};
    struct cmsghdr *cm;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cb;
    m.msg_controllen = sizeof(cb);
    ssize_t n = recvmsg(fd, &m, 0);
    if (n <= 0)
        return -1;
    cm = CMSG_FIRSTHDR(&m);
    if (!cm || cm->cmsg_type != SCM_RIGHTS)
        return -1;
    memcpy(ff, CMSG_DATA(cm), sizeof(int));
    return n;
}
int main(void)
{
    int cp[2], hp[2];
    pid_t p;
    int st;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    socketpair(AF_UNIX, SOCK_STREAM, 0, cp);
    socketpair(AF_UNIX, SOCK_STREAM, 0, hp);
    p = fork();
    if (p == 0)
    {
        int fd = open("/dev/null", O_RDONLY);
        close(cp[0]);
        close(hp[0]);
        ZgOpOutcome *o = zg_task_send_stream(cp[1], hp[1],
                                             "zgtest/data@s1", fd, 42);

        fprintf(stderr, "[child] ok=%d msg=%s\n", o != NULL && o->ok,
                o != NULL && o->message != NULL ? o->message : "-");
        _exit(o != NULL && o->ok ? 0 : 1);
    }
    close(cp[1]);
    close(hp[1]);
    struct timeval tv = {2, 0};
    setsockopt(cp[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(hp[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int ff = -1;
    struct stat sb;
    printf("req=%d fdstat=%d\n", reqfd(hp[0], &ff), ff >= 0 ? fstat(ff, &sb) : -1);
    if (ff >= 0)
        close(ff);
    printf("started send=%d\n", sendf(hp[0], "{\"type\":\"started\"}"));
    char *f = recvf(cp[0]);
    printf("client got=%s\n", f ? f : "NULL");
    free(f);
    printf("progress send=%d\n", sendf(hp[0], "{\"type\":\"progress\",\"bytes\":4096}"));
    f = recvf(cp[0]);
    printf("client got=%s\n", f ? f : "NULL");
    free(f);
    printf("cancel send=%d\n", sendf(cp[0], "{\"op\":\"cancel\"}"));
    f = recvf(hp[0]);
    printf("helper got=%s\n", f ? f : "NULL");
    free(f);
    printf("final send=%d\n", sendf(hp[0], "{\"type\":\"completed\",\"ok\":true}"));
    f = recvf(cp[0]);
    printf("client got=%s\n", f ? f : "NULL");
    free(f);
    close(cp[0]);
    close(hp[0]);
    waitpid(p, &st, 0);
    printf("child st=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
