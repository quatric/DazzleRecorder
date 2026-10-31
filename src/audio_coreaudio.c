/*
 * Audio from a capture box that macOS already publishes as a Core Audio
 * input device. The black Dazzle (1b80:e60a) paces its USB Audio Class
 * stream correctly, so AppleUSBAudio owns the interface and libusb cannot.
 */
#include "sysaudio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_BUFS 4

struct sa_input {
    AudioQueueRef q;
    sa_input_cb cb;
    void *user;
};

/* pass 0: model UID contains ":VVVV:PPPP"; pass 1: device name matches */
static int cfstr_matches(CFStringRef s, int pass, const char *vidpid, const char *product)
{
    char buf[256];
    if (!s || !CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
        return 0;
    if (pass == 0) {
        for (char *p = buf; *p; p++)
            if (*p >= 'a' && *p <= 'z')
                *p = (char)(*p - 'a' + 'A');
        return strstr(buf, vidpid) != NULL;
    }
    return sa_name_matches(buf, product);
}

static CFStringRef dev_string(AudioObjectID id, AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 sz = sizeof(s);
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &s))
        return NULL;
    return s;
}

static int input_channels(AudioObjectID id)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) || !sz)
        return 0;
    AudioBufferList *bl = malloc(sz);
    int n = 0;
    if (bl && !AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, bl))
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
            n += (int)bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

/* USB audio devices carry "name:VVVV:PPPP" in their model UID. Match on that,
 * then fall back to the product name. */
static AudioObjectID find_device(uint16_t vid, uint16_t pid, const char *product)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz))
        return kAudioObjectUnknown;
    AudioObjectID *ids = malloc(sz);
    if (!ids || AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids)) {
        free(ids);
        return kAudioObjectUnknown;
    }
    char vidpid[16];
    snprintf(vidpid, sizeof(vidpid), ":%04X:%04X", vid, pid);
    AudioObjectID found = kAudioObjectUnknown;
    const char *env = getenv("DVC100_AUDIO_DEVICE");
    for (int pass = env && *env ? 1 : 0; pass < 2 && found == kAudioObjectUnknown; pass++) {
        for (UInt32 i = 0; i < sz / sizeof(*ids); i++) {
            if (input_channels(ids[i]) == 0)
                continue;
            CFStringRef s = dev_string(ids[i], pass ? kAudioObjectPropertyName : kAudioDevicePropertyModelUID);
            int hit = cfstr_matches(s, pass, vidpid, product);
            if (s)
                CFRelease(s);
            if (hit) {
                found = ids[i];
                break;
            }
        }
    }
    free(ids);
    return found;
}

static void input_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *ts,
                     UInt32 npackets, const AudioStreamPacketDescription *desc)
{
    (void)ts, (void)npackets, (void)desc;
    sa_input_t *c = user;
    if (b->mAudioDataByteSize)
        c->cb((const int16_t *)b->mAudioData, (int)(b->mAudioDataByteSize / 4), c->user);
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

const char *sa_backend_name(void) { return "Core Audio"; }

sa_input_t *sa_input_start(uint16_t vid, uint16_t pid, const char *product, sa_input_cb cb, void *user,
                           int *rate)
{
    AudioObjectID id = find_device(vid, pid, product);
    if (id == kAudioObjectUnknown)
        return NULL;

    Float64 sr = 48000;
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(sr);
    AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &sr);

    sa_input_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->cb = cb;
    c->user = user;

    AudioStreamBasicDescription f = {
        .mSampleRate = sr,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = 4,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 4,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 16,
    };
    CFStringRef uid = dev_string(id, kAudioDevicePropertyDeviceUID);
    if (AudioQueueNewInput(&f, input_cb, c, NULL, NULL, 0, &c->q) || !uid ||
        AudioQueueSetProperty(c->q, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid))) {
        if (uid)
            CFRelease(uid);
        if (c->q)
            AudioQueueDispose(c->q, true);
        free(c);
        return NULL;
    }
    CFRelease(uid);

    /* 10 ms buffers */
    UInt32 bytes = (UInt32)(sr / 100) * 4;
    for (int i = 0; i < NUM_BUFS; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(c->q, bytes, &b) == 0)
            AudioQueueEnqueueBuffer(c->q, b, 0, NULL);
    }
    if (AudioQueueStart(c->q, NULL)) {
        AudioQueueDispose(c->q, true);
        free(c);
        return NULL;
    }
    if (rate)
        *rate = (int)sr;
    return c;
}

void sa_input_stop(sa_input_t *c)
{
    if (!c)
        return;
    AudioQueueStop(c->q, true);
    AudioQueueDispose(c->q, true);
    free(c);
}
