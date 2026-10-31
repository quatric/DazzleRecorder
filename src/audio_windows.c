/*
 * Windows audio. The capture box's USB Audio Class interfaces are bound to
 * usbaudio.sys (only the video interface is switched to WinUSB), so its
 * sound is a WASAPI capture endpoint. Monitoring plays through waveOut.
 */
#define COBJMACROS
#include "sysaudio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* defined here so no uuid/ksuser import library is needed */
static const GUID k_CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c,
                                                 { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const GUID k_IID_IMMDeviceEnumerator = { 0xa95664d2, 0x9614, 0x4f35,
                                                { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const GUID k_IID_IAudioClient = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const GUID k_IID_IAudioCaptureClient = { 0xc8adbd64, 0xe71e, 0x48a0,
                                                { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const PROPERTYKEY k_PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

const char *sa_backend_name(void) { return "WASAPI"; }

static void to_utf8(const wchar_t *w, char *out, int len)
{
    out[0] = 0;
    if (w)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, len, NULL, NULL);
}

static int upper_contains(const char *hay, const char *needle)
{
    char h[512];
    size_t i;
    for (i = 0; hay[i] && i + 1 < sizeof(h); i++)
        h[i] = (char)(hay[i] >= 'a' && hay[i] <= 'z' ? hay[i] - 32 : hay[i]);
    h[i] = 0;
    return strstr(h, needle) != NULL;
}

/* pass 0: any string property names the USB device (paths and instance IDs
 * carry "VID_xxxx&PID_xxxx"); pass 1: the friendly name matches. */
static int endpoint_matches(IMMDevice *dev, int pass, const char *vidpid, const char *product)
{
    IPropertyStore *ps = NULL;
    int hit = 0;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)))
        return 0;
    char buf[512];
    if (pass == 0) {
        DWORD n = 0;
        IPropertyStore_GetCount(ps, &n);
        for (DWORD i = 0; i < n && !hit; i++) {
            PROPERTYKEY key;
            PROPVARIANT pv;
            if (FAILED(IPropertyStore_GetAt(ps, i, &key)))
                continue;
            PropVariantInit(&pv);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &key, &pv)) && pv.vt == VT_LPWSTR) {
                to_utf8(pv.pwszVal, buf, sizeof(buf));
                hit = upper_contains(buf, vidpid);
            }
            PropVariantClear(&pv);
        }
    } else {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &k_PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
            to_utf8(pv.pwszVal, buf, sizeof(buf));
            hit = sa_name_matches(buf, product);
        }
        PropVariantClear(&pv);
    }
    IPropertyStore_Release(ps);
    return hit;
}

static IMMDevice *find_endpoint(IMMDeviceEnumerator *en, uint16_t vid, uint16_t pid, const char *product)
{
    IMMDeviceCollection *coll = NULL;
    IMMDevice *found = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eCapture, DEVICE_STATE_ACTIVE, &coll)))
        return NULL;
    UINT count = 0;
    IMMDeviceCollection_GetCount(coll, &count);
    char vidpid[32];
    snprintf(vidpid, sizeof(vidpid), "VID_%04X&PID_%04X", vid, pid);
    const char *env = getenv("DVC100_AUDIO_DEVICE");
    for (int pass = env && *env ? 1 : 0; pass < 2 && !found; pass++) {
        for (UINT i = 0; i < count && !found; i++) {
            IMMDevice *dev = NULL;
            if (FAILED(IMMDeviceCollection_Item(coll, i, &dev)))
                continue;
            if (endpoint_matches(dev, pass, vidpid, product))
                found = dev;
            else
                IMMDevice_Release(dev);
        }
    }
    IMMDeviceCollection_Release(coll);
    return found;
}

struct sa_input {
    uint16_t vid, pid;
    char product[128];
    sa_input_cb cb;
    void *user;
    volatile int quit;
    HANDLE ready;
    int ok;
    pthread_t thread;
};

static void *capture_thread(void *arg)
{
    sa_input_t *in = arg;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL;
    int com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));

    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
    if (FAILED(CoCreateInstance(&k_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &k_IID_IMMDeviceEnumerator,
                                (void **)&en)) ||
        !(dev = find_endpoint(en, in->vid, in->pid, in->product)) ||
        FAILED(IMMDevice_Activate(dev, &k_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) ||
        FAILED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       2000000 /* 200 ms */, 0, &wf, NULL)) ||
        FAILED(IAudioClient_GetService(ac, &k_IID_IAudioCaptureClient, (void **)&cc)) ||
        FAILED(IAudioClient_Start(ac))) {
        SetEvent(in->ready);
        goto out;
    }
    in->ok = 1;
    SetEvent(in->ready);

    while (!in->quit) {
        Sleep(5);
        UINT32 avail = 0;
        while (!in->quit && SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &avail)) && avail) {
            BYTE *data;
            UINT32 frames;
            DWORD flags;
            if (FAILED(IAudioCaptureClient_GetBuffer(cc, &data, &frames, &flags, NULL, NULL)))
                break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                memset(data, 0, (size_t)frames * 4);
            in->cb((const int16_t *)(const void *)data, (int)frames, in->user);
            IAudioCaptureClient_ReleaseBuffer(cc, frames);
        }
    }
    IAudioClient_Stop(ac);
out:
    if (cc) IAudioCaptureClient_Release(cc);
    if (ac) IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (com)
        CoUninitialize();
    return NULL;
}

sa_input_t *sa_input_start(uint16_t vid, uint16_t pid, const char *product, sa_input_cb cb, void *user,
                           int *rate)
{
    sa_input_t *in = calloc(1, sizeof(*in));
    if (!in)
        return NULL;
    in->vid = vid;
    in->pid = pid;
    snprintf(in->product, sizeof(in->product), "%s", product ? product : "");
    in->cb = cb;
    in->user = user;
    in->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!in->ready || pthread_create(&in->thread, NULL, capture_thread, in)) {
        if (in->ready)
            CloseHandle(in->ready);
        free(in);
        return NULL;
    }
    WaitForSingleObject(in->ready, INFINITE);
    if (!in->ok) {
        pthread_join(in->thread, NULL);
        CloseHandle(in->ready);
        free(in);
        return NULL;
    }
    if (rate)
        *rate = 48000;
    return in;
}

void sa_input_stop(sa_input_t *in)
{
    if (!in)
        return;
    in->quit = 1;
    pthread_join(in->thread, NULL);
    CloseHandle(in->ready);
    free(in);
}

/* ---- playback ---- */

#define OUT_BUFS 6
#define OUT_FRAMES 480 /* 10 ms at 48 kHz */

struct sa_output {
    HWAVEOUT wo;
    HANDLE event;
    WAVEHDR hdr[OUT_BUFS];
    int16_t buf[OUT_BUFS][OUT_FRAMES * 2];
    int frames;
    sa_output_cb cb;
    void *user;
    volatile int quit;
    pthread_t thread;
};

static void *playback_thread(void *arg)
{
    sa_output_t *out = arg;
    while (!out->quit) {
        for (int i = 0; i < OUT_BUFS; i++) {
            WAVEHDR *h = &out->hdr[i];
            if (h->dwUser && !(h->dwFlags & WHDR_DONE))
                continue; /* still queued */
            out->cb(out->buf[i], out->frames, out->user);
            h->dwUser = 1;
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(out->wo, h, sizeof(*h));
        }
        WaitForSingleObject(out->event, 50);
    }
    return NULL;
}

sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user)
{
    sa_output_t *out = calloc(1, sizeof(*out));
    if (!out)
        return NULL;
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, (DWORD)rate, (DWORD)rate * 4, 4, 16, 0 };
    out->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!out->event ||
        waveOutOpen(&out->wo, WAVE_MAPPER, &wf, (DWORD_PTR)out->event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        if (out->event)
            CloseHandle(out->event);
        free(out);
        return NULL;
    }
    out->frames = rate / 100 < OUT_FRAMES ? rate / 100 : OUT_FRAMES;
    for (int i = 0; i < OUT_BUFS; i++) {
        out->hdr[i].lpData = (LPSTR)out->buf[i];
        out->hdr[i].dwBufferLength = (DWORD)out->frames * 4;
        waveOutPrepareHeader(out->wo, &out->hdr[i], sizeof(WAVEHDR));
    }
    out->cb = cb;
    out->user = user;
    if (pthread_create(&out->thread, NULL, playback_thread, out)) {
        sa_output_stop(out);
        return NULL;
    }
    return out;
}

void sa_output_stop(sa_output_t *out)
{
    if (!out)
        return;
    if (out->thread) {
        out->quit = 1;
        SetEvent(out->event);
        pthread_join(out->thread, NULL);
    }
    waveOutReset(out->wo);
    for (int i = 0; i < OUT_BUFS; i++)
        waveOutUnprepareHeader(out->wo, &out->hdr[i], sizeof(WAVEHDR));
    waveOutClose(out->wo);
    CloseHandle(out->event);
    free(out);
}
