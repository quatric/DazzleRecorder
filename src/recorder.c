/*
 * MPEG-2 recorder: YUV4MPEG2 video on the child's stdin, raw s16le audio on
 * a second pipe (fd 3 on POSIX, a named pipe on Windows), encoded by ffmpeg
 * to MPEG-2 video + MPEG-1 Layer II audio in a DVD program stream. Audio is
 * the master clock: if the video falls behind (dropped frames, loss of
 * signal) the last frame is repeated so the two streams stay in sync for
 * the whole recording.
 */
#include "recorder.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE pipe_t;
#define NO_PIPE NULL
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
typedef int pipe_t;
#define NO_PIPE (-1)
#endif

#define VQUEUE 32
#define ARING (48000 * 4 * 2) /* 2 s of 48 kHz s16 stereo */

struct recorder {
    recorder_opts_t o;
#ifdef _WIN32
    HANDLE proc;
    HANDLE apipe_event;     /* overlapped connect/write on the audio pipe */
#else
    pid_t pid;
#endif
    pipe_t vfd, afd;
    size_t frame_bytes;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    int closing;

    uint8_t *vq[VQUEUE];
    int vhead, vcount;
    int started;            /* first video frame seen; audio before it is dropped */

    uint8_t *aring;
    size_t ahead, acount;

    uint64_t audio_samples; /* written to ffmpeg */
    uint64_t video_frames;  /* written to ffmpeg, including repeats */
    uint64_t dropped, repeated;
    int failed;

    pthread_t vthread, athread;
};

/* ---- platform: pipes, the ffmpeg child, finding ffmpeg ---- */

#ifdef _WIN32

static int write_all(recorder_t *r, HANDLE h, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    OVERLAPPED ov = { 0 };
    int overlapped = h == r->afd; /* the audio named pipe is opened for overlapped I/O */
    ov.hEvent = r->apipe_event;
    while (len) {
        DWORD n = 0, chunk = len > (1u << 20) ? (1u << 20) : (DWORD)len;
        if (overlapped) {
            if (!WriteFile(h, p, chunk, NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
                return -1;
            if (!GetOverlappedResult(h, &ov, &n, TRUE))
                return -1;
        } else if (!WriteFile(h, p, chunk, &n, NULL)) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static void close_pipe(pipe_t *h)
{
    if (*h) {
        CloseHandle(*h);
        *h = NULL;
    }
}

static int child_exited(recorder_t *r) { return WaitForSingleObject(r->proc, 0) == WAIT_OBJECT_0; }

/* ffmpeg opens the audio pipe only after it has read the video header */
static int connect_audio_pipe(recorder_t *r)
{
    OVERLAPPED ov = { 0 };
    ov.hEvent = r->apipe_event;
    if (ConnectNamedPipe(r->afd, &ov))
        return 0;
    DWORD e = GetLastError();
    if (e == ERROR_PIPE_CONNECTED)
        return 0;
    if (e != ERROR_IO_PENDING)
        return -1;
    while (WaitForSingleObject(ov.hEvent, 200) == WAIT_TIMEOUT) {
        if (child_exited(r)) {
            CancelIo(r->afd);
            return -1;
        }
    }
    DWORD n;
    return GetOverlappedResult(r->afd, &ov, &n, FALSE) ? 0 : -1;
}

static int is_file(const char *utf8)
{
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, MAX_PATH * 2))
        return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int exe_dir(char *out, size_t len)
{
    wchar_t w[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2)
        return -1;
    wchar_t *slash = wcsrchr(w, L'\\');
    if (slash)
        *slash = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)len, NULL, NULL) ? 0 : -1;
}

#define EXE_NAME "ffmpeg.exe"
#define PATH_SEP ";"
#define DIR_SEP "\\"
#define EXTRA_DIRS ""

/* Command line quoting that CommandLineToArgvW and the MSVC runtime undo. */
static void append_arg(wchar_t *cmd, size_t cap, const char *arg)
{
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, 2048);
    size_t n = wcslen(cmd);
#define PUT(c) do { if (n + 1 < cap) cmd[n++] = (c); } while (0)
    if (n)
        PUT(L' ');
    PUT(L'"');
    for (const wchar_t *p = w;; p++) {
        size_t bs = 0;
        while (*p == L'\\') {
            p++;
            bs++;
        }
        if (!*p) {
            for (size_t i = 0; i < bs * 2; i++) PUT(L'\\');
            break;
        }
        if (*p == L'"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) PUT(L'\\');
        } else {
            for (size_t i = 0; i < bs; i++) PUT(L'\\');
        }
        PUT(*p);
    }
    PUT(L'"');
#undef PUT
    cmd[n] = 0;
}

static int spawn_ffmpeg(recorder_t *r, const char *ffmpeg, const char **argv, char *audio_arg, size_t audio_len,
                        char *err, int errlen)
{
    static volatile LONG counter;
    wchar_t pipename[96];
    swprintf(pipename, 96, L"\\\\.\\pipe\\dvc100-audio-%lu-%ld", GetCurrentProcessId(),
             InterlockedIncrement(&counter));
    WideCharToMultiByte(CP_UTF8, 0, pipename, -1, audio_arg, (int)audio_len, NULL, NULL);

    r->apipe_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    r->afd = CreateNamedPipeW(pipename, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1 << 20, 0, 0, NULL);
    if (r->afd == INVALID_HANDLE_VALUE) {
        r->afd = NULL;
        snprintf(err, (size_t)errlen, "cannot create the audio pipe (error %lu)", GetLastError());
        return -1;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE vread = NULL, nul;
    if (!CreatePipe(&vread, &r->vfd, &sa, 1 << 20)) {
        snprintf(err, (size_t)errlen, "cannot create the video pipe (error %lu)", GetLastError());
        return -1;
    }
    SetHandleInformation(r->vfd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    static wchar_t cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++)
        append_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), argv[i]);
    wchar_t wexe[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, ffmpeg, -1, wexe, MAX_PATH * 2);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = vread;
    si.hStdOutput = nul;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(wexe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(vread);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ok) {
        snprintf(err, (size_t)errlen, "cannot start %s (error %lu)", ffmpeg, e);
        return -1;
    }
    CloseHandle(pi.hThread);
    r->proc = pi.hProcess;
    return 0;
}

static int wait_child(recorder_t *r)
{
    DWORD code = 1;
    WaitForSingleObject(r->proc, INFINITE);
    GetExitCodeProcess(r->proc, &code);
    CloseHandle(r->proc);
    if (r->apipe_event)
        CloseHandle(r->apipe_event);
    return code == 0 ? 0 : -1;
}

#else /* POSIX */

static int write_all(recorder_t *r, int fd, const void *buf, size_t len)
{
    (void)r;
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void close_pipe(pipe_t *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int connect_audio_pipe(recorder_t *r)
{
    (void)r;
    return 0;
}

static int is_file(const char *path) { return access(path, X_OK) == 0; }

static int exe_dir(char *out, size_t len)
{
    char buf[4096];
#ifdef __APPLE__
    uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n))
        return -1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1;
    buf[n] = 0;
#else
    return -1;
#endif
    char *slash = strrchr(buf, '/');
    if (slash)
        *slash = 0;
    snprintf(out, len, "%s", buf);
    return 0;
}

#define EXE_NAME "ffmpeg"
#define PATH_SEP ":"
#define DIR_SEP "/"
/* GUI apps get a minimal PATH, so also try the usual Homebrew/MacPorts places */
#define EXTRA_DIRS ":/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:/usr/bin"

static int spawn_ffmpeg(recorder_t *r, const char *ffmpeg, const char **argv, char *audio_arg, size_t audio_len,
                        char *err, int errlen)
{
    int vp[2], ap[2];
    snprintf(audio_arg, audio_len, "pipe:3");
    if (pipe(vp)) {
        snprintf(err, (size_t)errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    if (pipe(ap)) {
        snprintf(err, (size_t)errlen, "pipe: %s", strerror(errno));
        close(vp[0]);
        close(vp[1]);
        return -1;
    }
    fcntl(vp[1], F_SETFD, FD_CLOEXEC);
    fcntl(ap[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, vp[0], 0);
    posix_spawn_file_actions_adddup2(&fa, ap[0], 3);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    int rc = posix_spawn(&r->pid, ffmpeg, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(vp[0]);
    close(ap[0]);
    if (rc) {
        snprintf(err, (size_t)errlen, "cannot start %s: %s", ffmpeg, strerror(rc));
        close(vp[1]);
        close(ap[1]);
        return -1;
    }
    r->vfd = vp[1];
    r->afd = ap[1];
    signal(SIGPIPE, SIG_IGN); /* a dying ffmpeg must not kill the host process */
    return 0;
}

static int wait_child(recorder_t *r)
{
    int status = 0;
    while (waitpid(r->pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

#endif

/* DVC100_FFMPEG, then next to this program, then PATH */
const char *rec_find_ffmpeg(void)
{
    static char found[4096];
    const char *env = getenv("DVC100_FFMPEG");
    if (env && is_file(env))
        return env;
    char dir[4096];
    if (exe_dir(dir, sizeof(dir)) == 0) {
        snprintf(found, sizeof(found), "%s" DIR_SEP EXE_NAME, dir);
        if (is_file(found))
            return found;
    }
    const char *path = getenv("PATH");
    char buf[8192];
    snprintf(buf, sizeof(buf), "%s" EXTRA_DIRS, path ? path : "");
    for (char *save = NULL, *d = strtok_r(buf, PATH_SEP, &save); d; d = strtok_r(NULL, PATH_SEP, &save)) {
        snprintf(found, sizeof(found), "%s" DIR_SEP EXE_NAME, d);
        if (is_file(found))
            return found;
    }
    return NULL;
}

static void *video_writer(void *arg)
{
    recorder_t *r = arg;
    const int w = r->o.width, h = r->o.height;
    uint8_t *frame = malloc(r->frame_bytes), *planar = malloc(r->frame_bytes);
    const double fps = r->o.pal ? 25.0 : 30000.0 / 1001.0;
    int have_frame = 0;

    for (;;) {
        pthread_mutex_lock(&r->lock);
        while (!r->vcount && !r->closing)
            pthread_cond_wait(&r->cond, &r->lock);
        if (!r->vcount && r->closing) {
            pthread_mutex_unlock(&r->lock);
            break;
        }
        memcpy(frame, r->vq[r->vhead], r->frame_bytes);
        r->vhead = (r->vhead + 1) % VQUEUE;
        r->vcount--;
        pthread_mutex_unlock(&r->lock);

        /* YUYV -> planar 4:2:2 */
        uint8_t *Y = planar, *U = planar + w * h, *V = U + (w / 2) * h;
        for (int i = 0, n = w * h / 2; i < n; i++) {
            Y[2 * i] = frame[4 * i];
            U[i] = frame[4 * i + 1];
            Y[2 * i + 1] = frame[4 * i + 2];
            V[i] = frame[4 * i + 3];
        }
        have_frame = 1;

        /* write once, plus repeats while the audio clock is ahead by > 1 frame */
        int copies = 1;
        do {
            if (write_all(r, r->vfd, "FRAME\n", 6) || write_all(r, r->vfd, planar, r->frame_bytes)) {
                r->failed = 1;
                goto out;
            }
            pthread_mutex_lock(&r->lock);
            r->video_frames++;
            if (copies > 1)
                r->repeated++;
            double audio_t = (double)r->audio_samples / r->o.audio_rate;
            double video_t = r->video_frames / fps;
            pthread_mutex_unlock(&r->lock);
            if (audio_t - video_t < 1.5 / fps || copies > 30)
                break;
            copies++;
        } while (1);
    }
out:
    (void)have_frame;
    free(frame);
    free(planar);
    close_pipe(&r->vfd);
    return NULL;
}

static void *audio_writer(void *arg)
{
    recorder_t *r = arg;
    uint8_t buf[8192];
    if (connect_audio_pipe(r))
        r->failed = 1;
    for (;;) {
        pthread_mutex_lock(&r->lock);
        while (!r->acount && !r->closing)
            pthread_cond_wait(&r->cond, &r->lock);
        if (!r->acount && r->closing) {
            pthread_mutex_unlock(&r->lock);
            break;
        }
        size_t n = r->acount < sizeof(buf) ? r->acount : sizeof(buf);
        for (size_t i = 0; i < n; i++)
            buf[i] = r->aring[(r->ahead + i) % ARING];
        r->ahead = (r->ahead + n) % ARING;
        r->acount -= n;
        pthread_mutex_unlock(&r->lock);

        if (r->failed)
            continue; /* keep draining so the ring never fills */
        if (write_all(r, r->afd, buf, n)) {
            r->failed = 1;
            continue;
        }
        pthread_mutex_lock(&r->lock);
        r->audio_samples += n / 4;
        pthread_mutex_unlock(&r->lock);
    }
    close_pipe(&r->afd);
    return NULL;
}

recorder_t *rec_open(const char *path, const recorder_opts_t *opts, char *err, int errlen)
{
    const char *ffmpeg = rec_find_ffmpeg();
    if (!ffmpeg) {
        snprintf(err, (size_t)errlen,
#ifdef _WIN32
                 "ffmpeg.exe not found (put it next to this program or on PATH, or set DVC100_FFMPEG)"
#elif defined(__APPLE__)
                 "ffmpeg not found (brew install ffmpeg, or set DVC100_FFMPEG)"
#else
                 "ffmpeg not found (install the ffmpeg package, or set DVC100_FFMPEG)"
#endif
        );
        return NULL;
    }
    recorder_t *r = calloc(1, sizeof(*r));
    r->o = *opts;
    if (!r->o.video_kbps)
        r->o.video_kbps = 8000;
    if (!r->o.audio_kbps)
        r->o.audio_kbps = 224;
    if (!r->o.audio_rate)
        r->o.audio_rate = 48000;
    r->frame_bytes = (size_t)r->o.width * r->o.height * 2;
    r->vfd = r->afd = NO_PIPE;
    char audio_in[128];

    char vk[16], maxk[16], ak[16], rate[16], gop[8];
    snprintf(vk, sizeof(vk), "%dk", r->o.video_kbps);
    /* DVD limit is 9.8 Mbit/s for video */
    snprintf(maxk, sizeof(maxk), "%dk", r->o.video_kbps + 1000 > 9800 ? 9800 : r->o.video_kbps + 1000);
    snprintf(ak, sizeof(ak), "%dk", r->o.audio_kbps);
    snprintf(rate, sizeof(rate), "%d", r->o.audio_rate);
    snprintf(gop, sizeof(gop), "%d", r->o.pal ? 15 : 18);

    const char *argv[64];
    int n = 0;
#define A(x) argv[n++] = (x)
    A(ffmpeg); A("-hide_banner"); A("-loglevel"); A("error"); A("-nostdin"); A("-y");
    A("-thread_queue_size"); A("64");
    A("-f"); A("yuv4mpegpipe"); A("-i"); A("pipe:0");
    A("-thread_queue_size"); A("1024");
    A("-f"); A("s16le"); A("-ar"); A(rate); A("-ac"); A("2"); A("-i"); A(audio_in);
    A("-map"); A("0:v"); A("-map"); A("1:a");
    if (r->o.deinterlace) {
        A("-vf"); A("yadif=mode=send_frame:parity=tff,format=yuv420p");
    } else {
        /* field-aware 4:2:2 -> 4:2:0 so chroma doesn't mix the two fields */
        A("-vf"); A("setfield=tff,scale=interl=1,format=yuv420p");
        A("-flags"); A("+ilme+ildct"); A("-alternate_scan"); A("1");
    }
    A("-c:v"); A("mpeg2video"); A("-b:v"); A(vk); A("-maxrate"); A(maxk); A("-bufsize"); A("1835k");
    A("-g"); A(gop); A("-bf"); A("2"); A("-aspect"); A("4:3");
    A("-c:a"); A("mp2"); A("-b:a"); A(ak);
    A("-f"); A("dvd"); A(path);
    A(NULL);
#undef A

    if (spawn_ffmpeg(r, ffmpeg, argv, audio_in, sizeof(audio_in), err, errlen)) {
        close_pipe(&r->vfd);
        close_pipe(&r->afd);
#ifdef _WIN32
        if (r->apipe_event)
            CloseHandle(r->apipe_event);
#endif
        free(r);
        return NULL;
    }

    char hdr[128];
    int hl = snprintf(hdr, sizeof(hdr), "YUV4MPEG2 W%d H%d %s It %s C422\n", r->o.width, r->o.height,
                      r->o.pal ? "F25:1" : "F30000:1001", r->o.pal ? "A59:54" : "A10:11");
    write_all(r, r->vfd, hdr, (size_t)hl);

    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    for (int i = 0; i < VQUEUE; i++)
        r->vq[i] = malloc(r->frame_bytes);
    r->aring = malloc(ARING);
    pthread_create(&r->vthread, NULL, video_writer, r);
    pthread_create(&r->athread, NULL, audio_writer, r);
    return r;
}

void rec_video(recorder_t *r, const uint8_t *yuyv)
{
    pthread_mutex_lock(&r->lock);
    if (!r->closing) {
        r->started = 1;
        if (r->vcount == VQUEUE) {
            r->dropped++; /* the writer repeats a frame later to keep sync */
        } else {
            memcpy(r->vq[(r->vhead + r->vcount) % VQUEUE], yuyv, r->frame_bytes);
            r->vcount++;
            pthread_cond_broadcast(&r->cond);
        }
    }
    pthread_mutex_unlock(&r->lock);
}

void rec_audio(recorder_t *r, const int16_t *samples, int frames)
{
    size_t bytes = (size_t)frames * 4;
    const uint8_t *p = (const uint8_t *)samples;
    pthread_mutex_lock(&r->lock);
    if (r->started && !r->closing && r->acount + bytes <= ARING) {
        size_t tail = (r->ahead + r->acount) % ARING;
        for (size_t i = 0; i < bytes; i++)
            r->aring[(tail + i) % ARING] = p[i];
        r->acount += bytes;
        pthread_cond_broadcast(&r->cond);
    }
    pthread_mutex_unlock(&r->lock);
}

double rec_seconds(recorder_t *r)
{
    pthread_mutex_lock(&r->lock);
    double s = (double)r->audio_samples / r->o.audio_rate;
    pthread_mutex_unlock(&r->lock);
    return s;
}

int rec_close(recorder_t *r)
{
    if (!r)
        return -1;
    pthread_mutex_lock(&r->lock);
    r->closing = 1;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->vthread, NULL);
    pthread_join(r->athread, NULL);

    int ok = wait_child(r) == 0 && !r->failed;
    if (r->dropped || r->repeated)
        fprintf(stderr, "recorder: %llu frames dropped, %llu repeated to keep A/V sync\n",
                (unsigned long long)r->dropped, (unsigned long long)r->repeated);

    for (int i = 0; i < VQUEUE; i++)
        free(r->vq[i]);
    free(r->aring);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    free(r);
    return ok ? 0 : -1;
}
