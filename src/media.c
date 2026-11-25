/* media.c - PinFall video decoder: the ffmpeg CLI is spawned as a subprocess
 *           (fork + execv, argv array only - never a shell) and streams raw
 *           RGBA frames over a pipe.  H.264/HLS (Pinterest videos and GIF
 *           pins), animated GIF/WebP and local files are all handled by
 *           ffmpeg, with no libav* linkage and no whole-file buffering.
 *
 */
#define _POSIX_C_SOURCE 200809L

#include "media.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PC_VIDEO_MIN_DIM   2
#define PC_VIDEO_MAX_DIM   1920
#define PC_VIDEO_FRAME_TIMEOUT_S 10.0
#define PC_VIDEO_DEFAULT_FPS 24

struct PcVideo {
    pid_t pid;
    int   fd;        /* stdout pipe (raw RGBA) */
    FILE *errf;      /* stderr temp file (ffmpeg diagnostics) */
    int   errfd;
    int   w, h;
    char  last_error[768];
};

/* --------------------------------------------------------------- helpers */

static void vid_seterr(PcVideo *v, const char *fmt, ...)
{
    va_list ap;

    if (!v)
        return;
    va_start(ap, fmt);
    vsnprintf(v->last_error, sizeof v->last_error, fmt, ap);
    va_end(ap);
}

static void sanitize(char *s)
{
    char *p;

    for (p = s; p && *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    }
}

/* Last <=dstsz-1 bytes of ffmpeg's stderr, newlines flattened. */
static void read_err_tail(PcVideo *v, char *dst, size_t dstsz)
{
    struct stat st;
    off_t       start;
    ssize_t     n;

    if (!dst || dstsz == 0)
        return;
    dst[0] = '\0';
    if (!v || !v->errf)
        return;
    if (fstat(v->errfd, &st) != 0 || st.st_size <= 0)
        return;
    start = st.st_size > (off_t)(dstsz - 1) ? st.st_size - (off_t)(dstsz - 1)
                                            : 0;
    n = pread(v->errfd, dst, dstsz - 1, start);
    if (n < 0)
        n = 0;
    dst[n] = '\0';
    sanitize(dst);
}

static bool find_ffmpeg(char *out, size_t outsz)
{
    const char *env = getenv("PC_FFMPEG");
    const char *path;
    char        buf[1024];

    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return access(out, X_OK) == 0;
    }
    path = getenv("PATH");
    if (path) {
        const char *p = path;

        while (*p) {
            const char *end = strchr(p, ':');
            size_t      n = end ? (size_t)(end - p) : strlen(p);

            if (n > 0 && n + 8 < sizeof buf) {
                memcpy(buf, p, n);
                buf[n] = '\0';
                snprintf(buf + n, sizeof buf - n, "/%s", "ffmpeg");
                if (access(buf, X_OK) == 0) {
                    snprintf(out, outsz, "%s", buf);
                    return true;
                }
            }
            if (!end)
                break;
            p = end + 1;
        }
    }
    /* Common fallback locations. */
    {
        static const char *const fallbacks[] = {"/usr/bin/ffmpeg",
                                                "/usr/local/bin/ffmpeg", NULL};
        int i;

        for (i = 0; fallbacks[i]; i++) {
            if (access(fallbacks[i], X_OK) == 0) {
                snprintf(out, outsz, "%s", fallbacks[i]);
                return true;
            }
        }
    }
    return false;
}

bool pc_video_available(void)
{
    char path[1024];

    return find_ffmpeg(path, sizeof path);
}

static int clamp_dim(int v)
{
    if (v < PC_VIDEO_MIN_DIM)
        v = PC_VIDEO_MIN_DIM;
    if (v > PC_VIDEO_MAX_DIM)
        v = PC_VIDEO_MAX_DIM;
    if (v & 1)
        v--;
    if (v < PC_VIDEO_MIN_DIM)
        v = PC_VIDEO_MIN_DIM;
    return v;
}

static bool vid_fail(char **err, const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (err)
        *err = strdup(buf);
    return false;
}

/* ------------------------------------------------------------------- API */

PcVideo *pc_video_open(const char *url_or_path, int out_w, int out_h, int fps,
                       char **err)
{
    PcVideo *v;
    char     ffmpeg[1024];
    char     scale[64];
    char     fps_str[16];
    char     tail[512];
    int      fds[2];
    pid_t    pid;
    int      status = 0;

    if (err)
        *err = NULL;
    if (!url_or_path || !*url_or_path)
        return vid_fail(err, "video: invalid url/path"), NULL;
    if (!find_ffmpeg(ffmpeg, sizeof ffmpeg))
        return vid_fail(err,
                        "video: ffmpeg not found in PATH (install ffmpeg or "
                        "set PC_FFMPEG)"),
               NULL;
    if (fps <= 0)
        fps = PC_VIDEO_DEFAULT_FPS;
    if (fps > 60)
        fps = 60;
    out_w = clamp_dim(out_w);
    out_h = clamp_dim(out_h);
    snprintf(scale, sizeof scale, "scale=%d:%d", out_w, out_h);
    snprintf(fps_str, sizeof fps_str, "%d", fps);

    if (pipe(fds) != 0)
        return vid_fail(err, "video: pipe: %s", strerror(errno)), NULL;
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    v = calloc(1, sizeof *v);
    if (!v) {
        close(fds[0]);
        close(fds[1]);
        return vid_fail(err, "video: out of memory"), NULL;
    }
    v->errf = tmpfile();
    if (!v->errf) {
        close(fds[0]);
        close(fds[1]);
        free(v);
        return vid_fail(err, "video: cannot create stderr capture: %s",
                        strerror(errno)),
               NULL;
    }
    v->errfd = fileno(v->errf);
    v->fd    = -1;
    v->w     = out_w;
    v->h     = out_h;
    v->last_error[0] = '\0';

    {
        char *argv[26];
        int   n = 0;
        bool  is_hls = strstr(url_or_path, ".m3u8") != NULL;

        argv[n++] = ffmpeg;
        argv[n++] = "-hide_banner";
        argv[n++] = "-loglevel";
        argv[n++] = "error";
        argv[n++] = "-nostdin";
        if (is_hls) {
            /* Pinterest HLS playlists reference .cmfv segments, which ffmpeg's
             * HLS demuxer rejects unless extensions are unrestricted; the
             * protocol whitelist keeps https segments and local files working.
             * (These are HLS-demuxer options: only pass them for .m3u8 so a
             * plain mp4 input does not reject an unknown option.) */
            argv[n++] = "-allowed_extensions";
            argv[n++] = "ALL";
            argv[n++] = "-extension_picky";
            argv[n++] = "0";
            argv[n++] = "-protocol_whitelist";
            argv[n++] = "file,http,https,tcp,tls,crypto";
        }
        argv[n++] = "-i";
        argv[n++] = (char *)url_or_path;
        argv[n++] = "-an";
        argv[n++] = "-vf";
        argv[n++] = scale;
        argv[n++] = "-r";
        argv[n++] = fps_str;
        argv[n++] = "-f";
        argv[n++] = "rawvideo";
        argv[n++] = "-pix_fmt";
        argv[n++] = "rgba";
        argv[n++] = "-";
        argv[n]   = NULL;
        if (n >= (int)(sizeof argv / sizeof argv[0]) - 1) {
            fclose(v->errf);
            close(fds[0]);
            close(fds[1]);
            free(v);
            return vid_fail(err, "video: argv overflow"), NULL;
        }

        pid = fork();
        if (pid < 0) {
            fclose(v->errf);
            close(fds[0]);
            close(fds[1]);
            free(v);
            return vid_fail(err, "video: fork: %s", strerror(errno)), NULL;
        }
        if (pid == 0) {
            /* child: stdout -> pipe, stderr -> temp file */
            if (dup2(fds[1], STDOUT_FILENO) < 0 ||
                dup2(v->errfd, STDERR_FILENO) < 0)
                _exit(127);
            close(fds[0]);
            close(fds[1]);
            close(v->errfd);
            execv(ffmpeg, argv);
            _exit(127);
        }
    }
    close(fds[1]);
    v->pid = pid;
    v->fd  = fds[0];

    /* Short startup grace: catch obvious failures (missing file, bad URL,
     * missing ffmpeg) without waiting for the first frame timeout. */
    {
        struct timespec ts = {0, 120000000L}; /* 120 ms */

        nanosleep(&ts, NULL);
        if (waitpid(pid, &status, WNOHANG) == pid) {
            read_err_tail(v, tail, sizeof tail);
            close(v->fd);
            v->fd = -1;
            fclose(v->errf);
            vid_seterr(v, "video: ffmpeg exited immediately%s%s", tail[0] ? ": " : "",
                       tail);
            if (err)
                *err = strdup(v->last_error);
            free(v);
            return NULL;
        }
    }
    return v;
}

bool pc_video_read_frame(PcVideo *v, uint8_t *rgba_out)
{
    size_t          need, got = 0;
    struct timespec start;

    if (!v || !rgba_out || v->fd < 0) {
        if (v)
            vid_seterr(v, "video: decoder is not open");
        return false;
    }
    need = (size_t)v->w * (size_t)v->h * 4u;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (got < need) {
        fd_set          rf;
        struct timeval  tv;
        struct timespec now;
        double          elapsed, remain;
        int             rc;
        ssize_t         n;

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - start.tv_sec) +
                  (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        remain = PC_VIDEO_FRAME_TIMEOUT_S - elapsed;
        if (remain <= 0.0) {
            vid_seterr(v, "video: timed out after %.0fs waiting for a frame",
                       PC_VIDEO_FRAME_TIMEOUT_S);
            return false;
        }
        tv.tv_sec  = (long)remain;
        tv.tv_usec = (long)((remain - (double)tv.tv_sec) * 1e6);
        FD_ZERO(&rf);
        FD_SET(v->fd, &rf);
        rc = select(v->fd + 1, &rf, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            vid_seterr(v, "video: select: %s", strerror(errno));
            return false;
        }
        if (rc == 0) {
            vid_seterr(v, "video: timed out after %.0fs waiting for a frame",
                       PC_VIDEO_FRAME_TIMEOUT_S);
            return false;
        }
        n = read(v->fd, rgba_out + got, need - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            char tail[512];

            read_err_tail(v, tail, sizeof tail);
            if (got == 0)
                vid_seterr(v, "video: end of stream%s%s",
                           tail[0] ? " (ffmpeg: " : "", tail[0] ? tail : "");
            else
                vid_seterr(v, "video: truncated frame (%zu/%zu bytes)%s%s",
                           got, need, tail[0] ? " (ffmpeg: " : "",
                           tail[0] ? tail : "");
            if (got > 0 && tail[0]) {
                size_t l = strlen(v->last_error);

                if (l + 2 < sizeof v->last_error) {
                    v->last_error[l]     = ')';
                    v->last_error[l + 1] = '\0';
                }
            }
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
        vid_seterr(v, "video: read: %s", strerror(errno));
        return false;
    }
    return true;
}

void pc_video_close(PcVideo *v)
{
    if (!v)
        return;
    if (v->fd >= 0) {
        close(v->fd);
        v->fd = -1;
    }
    if (v->pid > 0) {
        int i;
        int status = 0;
        bool reaped = false;

        kill(v->pid, SIGTERM);
        for (i = 0; i < 100; i++) {
            pid_t r = waitpid(v->pid, &status, WNOHANG);

            if (r == v->pid || (r < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            {
                struct timespec ts = {0, 10000000L}; /* 10 ms */

                nanosleep(&ts, NULL);
            }
        }
        if (!reaped) {
            kill(v->pid, SIGKILL);
            waitpid(v->pid, &status, 0);
        }
    }
    if (v->errf)
        fclose(v->errf);
    free(v);
}

void pc_video_size(PcVideo *v, int *w, int *h)
{
    if (w)
        *w = v ? v->w : 0;
    if (h)
        *h = v ? v->h : 0;
}

const char *pc_video_error(PcVideo *v)
{
    static _Thread_local char buf[768];

    buf[0] = '\0';
    if (!v)
        return buf;
    snprintf(buf, sizeof buf, "%s", v->last_error);
    return buf;
}
