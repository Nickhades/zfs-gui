#include "zg-progress.h"

#include <string.h>
#include <unistd.h>

#include "zg-json.h"

static int progress_fd = -1;

void zg_progress_begin(int fd)
{
    progress_fd = fd;
}

void zg_progress_end(void)
{
    progress_fd = -1;
}

void zg_progress_report(guint64 bytes)
{
    ZgJsonWriter *writer;
    gchar *text;
    guint32 length;
    guchar *p;
    gsize left;

    if (progress_fd < 0)
        return;

    writer = zg_json_writer_new();
    zg_json_writer_begin_object(writer, NULL);
    zg_json_writer_set_string(writer, "type", "progress");
    zg_json_writer_set_int(writer, "bytes", (gint64)bytes);
    zg_json_writer_end_object(writer);
    text = zg_json_writer_to_string(writer, NULL);

    length = g_htonl((guint32)strlen(text));
    if (write(progress_fd, &length, sizeof(length)) == sizeof(length))
    {
        p = (guchar *)text;
        left = strlen(text);
        while (left > 0)
        {
            ssize_t n = write(progress_fd, p, left);

            if (n <= 0)
                break;
            p += n;
            left -= (gsize)n;
        }
    }

    g_free(text);
}

int zg_progress_fd(void)
{
    return progress_fd;
}
