#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
#include "zg-task.h"
#include "zg-frame.h"
#include "zg-json.h"

#define LOG(...)                                \
    do                                          \
    {                                           \
        fprintf(stderr, "[task] " __VA_ARGS__); \
        fputc('\n', stderr);                    \
    } while (0)

typedef struct
{
    int ok;
    char *msg;
} Out;

static int wframe(int fd, const char *t)
{
    guint32 l = g_htonl((guint32)strlen(t));
    guint8 b[1024];
    size_t tot = 4 + strlen(t), off = 0;
    memcpy(b, &l, 4);
    memcpy(b + 4, t, strlen(t));
    while (off < tot)
    {
        ssize_t n = send(fd, b + off, tot - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0)
        {
            off += n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN))
        {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 200) <= 0)
                return -1;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        LOG("wframe send fd=%d n=%zd errno=%d", fd, n, errno);
        return -1;
    }
    return 0;
}
static int sreq(int hf, const char *snap, int ffd, guint32 id)
{
    ZgJsonWriter *w = zg_json_writer_new();
    struct msghdr m;
    struct iovec iov;
    char cb[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;
    gchar *t;
    guint8 fr[1024];
    guint32 be;
    size_t tot;
    ssize_t n;
    zg_json_writer_begin_object(w, NULL);
    zg_json_writer_set_int(w, "v", 1);
    zg_json_writer_set_string(w, "op", "send_stream");
    zg_json_writer_set_string(w, "snapshot", snap);
    zg_json_writer_set_int(w, "req_id", (gint64)id);
    zg_json_writer_end_object(w);
    t = zg_json_writer_to_string(w, NULL);
    be = g_htonl((guint32)strlen(t));
    tot = 4 + strlen(t);
    memcpy(fr, &be, 4);
    memcpy(fr + 4, t, strlen(t));
    g_free(t);
    memset(&m, 0, sizeof(m));
    memset(cb, 0, sizeof(cb));
    iov.iov_base = fr;
    iov.iov_len = tot;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cb;
    m.msg_controllen = sizeof(cb);
    cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &ffd, sizeof(ffd));
    n = sendmsg(hf, &m, MSG_NOSIGNAL);
    LOG("sreq sendmsg n=%zd errno=%d", n, errno);
    return n > 0 ? 0 : -1;
}
static int istype(const char *f, const char *type)
{
    GError *e = NULL;
    ZgJson *r = zg_json_parse(f, -1, &e);
    int ok = r && g_strcmp0(zg_json_object_get_string(r, "type"), type) == 0;
    zg_json_free(r);
    g_clear_error(&e);
    return ok;
}

static Out run(int cf, int hf, const char *snap, int ffd, guint32 id)
{
    Out o = {0, NULL};
    ZgFrameBuffer *hb = zg_frame_buffer_new(1 << 20), *cb = zg_frame_buffer_new(1 << 16);
    int kq = kqueue();
    struct kevent ch, ev[4];
    struct timespec ts = {1, 0};
    if (kq < 0)
    {
        o.msg = g_strdup("kq");
        return o;
    }
    EV_SET(&ch, hf, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void *)1);
    kevent(kq, &ch, 1, NULL, 0, NULL);
    EV_SET(&ch, cf, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void *)2);
    kevent(kq, &ch, 1, NULL, 0, NULL);
    EV_SET(&ch, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, 1000, NULL);
    kevent(kq, &ch, 1, NULL, 0, NULL);
    if (sreq(hf, snap, ffd, id))
    {
        o.msg = g_strdup("sreq");
        return o;
    }
    int done = 0;
    while (!done)
    {
        int n = kevent(kq, NULL, 0, ev, 4, &ts);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
        {
            LOG("kevent errno=%d", errno);
            o.msg = g_strdup("kevent");
            break;
        }
        for (int i = 0; i < n; i++)
        {
            LOG("KEVENT ident=%ld filter=%d flags=%x data=%ld", (long)ev[i].ident, ev[i].filter, ev[i].flags, (long)ev[i].data);
            if (ev[i].filter == EVFILT_TIMER)
                continue;
            int fd = (int)ev[i].ident;
            ZgFrameBuffer *b = fd == hf ? hb : cb;
            if (ev[i].flags & EV_EOF)
            {
                LOG("EOF fd=%d", fd);
                if (fd == hf)
                {
                    o.msg = g_strdup("helper eof");
                    done = 1;
                    break;
                }
                else
                {
                    wframe(hf, "{\"op\":\"cancel\"}");
                    continue;
                }
            }
            ZgFrameReadResult rr = zg_frame_read(b, fd, NULL);
            LOG("READ fd=%d rr=%d buffered=%u errno=%d", fd, rr, b->data->len, errno);
            if (rr == ZG_FRAME_EOF)
            {
                o.msg = g_strdup("eof");
                done = 1;
                break;
            }
            gchar *f = NULL;
            int p;
            while ((p = zg_frame_pop(b, &f, NULL)) == 1)
            {
                LOG("POP fd=%d frame=%s", fd, f);
                if (fd == hf)
                {
                    if (!(istype(f, "started") || istype(f, "progress")))
                    {
                        int rc = wframe(cf, f);
                        LOG("FWD final to client rc=%d", rc);
                        o.ok = 1;
                        o.msg = g_strdup(f);
                        done = 1;
                        g_free(f);
                        break;
                    }
                    int rc = wframe(cf, f);
                    LOG("FWD to client rc=%d", rc);
                }
                else
                {
                    if (strstr(f, "cancel"))
                    {
                        int rc = wframe(hf, "{\"op\":\"cancel\"}");
                        LOG("CANCEL fwd rc=%d", rc);
                    }
                }
                g_free(f);
                f = NULL;
            }
            if (p < 0)
            {
                o.msg = g_strdup("bad frame");
                done = 1;
                break;
            }
            if (done)
                break;
        }
    }
    close(kq);
    zg_frame_buffer_free(hb);
    zg_frame_buffer_free(cb);
    return o;
}

ZgOpOutcome *
zg_task_send_stream(int client_fd, int helper_fd, const gchar *snapshot,
                    gint file_fd, guint32 req_id)
{
    Out o = run(client_fd, helper_fd, snapshot, file_fd, req_id);
    ZgOpOutcome *outcome = g_new0(ZgOpOutcome, 1);

    outcome->ok = o.ok;
    outcome->code = o.ok ? 0 : 1;
    outcome->message = o.msg != NULL ? o.msg : g_strdup(o.ok ? "操作完成" : "操作失败");
    outcome->stdout_text = g_strdup("");
    outcome->stderr_text = g_strdup("");
    outcome->data_json = g_strdup("");
    return outcome;
}
