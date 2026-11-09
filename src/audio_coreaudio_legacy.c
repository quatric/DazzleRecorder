/*
 * Core Audio for the Mac OS X 10.3/10.4 build (PowerPC and Intel), which has
 * neither AudioQueue (10.5) nor AudioObject (10.4) nor AVAudioEngine:
 *
 *   input   the Dazzle's own sound device (the black Dazzle) or the Line In,
 *           read with an IOProc on the device; the HAL hands it over as Float32
 *   output  monitor playback through the default output unit, found with the
 *           Component Manager; it converts our 16-bit stereo itself
 *
 * See audio_coreaudio.c for the 10.9+ version of the input side.
 */
#include "sysaudio.h"

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- device lookup ---- */

static int list_devices(AudioDeviceID **out)
{
    UInt32 sz = 0;
    Boolean writable;
    *out = NULL;
    if (AudioHardwareGetPropertyInfo(kAudioHardwarePropertyDevices, &sz, &writable) || !sz)
        return 0;
    AudioDeviceID *ids = malloc(sz);
    if (!ids || AudioHardwareGetProperty(kAudioHardwarePropertyDevices, &sz, ids)) {
        free(ids);
        return 0;
    }
    *out = ids;
    return (int)(sz / sizeof(*ids));
}

static int input_channels(AudioDeviceID id)
{
    UInt32 sz = 0;
    Boolean writable;
    if (AudioDeviceGetPropertyInfo(id, 0, true, kAudioDevicePropertyStreamConfiguration, &sz, &writable) || !sz)
        return 0;
    AudioBufferList *bl = malloc(sz);
    int n = 0;
    if (bl && !AudioDeviceGetProperty(id, 0, true, kAudioDevicePropertyStreamConfiguration, &sz, bl))
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
            n += (int)bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

static void device_name(AudioDeviceID id, char *out, UInt32 len)
{
    UInt32 sz = len;
    out[0] = 0;
    if (AudioDeviceGetProperty(id, 0, true, kAudioDevicePropertyDeviceName, &sz, out))
        out[0] = 0;
    out[len - 1] = 0;
}

/* 10.3 has no model UID with the USB IDs in it, so the box is found by its name */
static AudioDeviceID find_device(const char *product)
{
    AudioDeviceID *ids, found = kAudioDeviceUnknown;
    int n = list_devices(&ids);
    for (int i = 0; i < n && found == kAudioDeviceUnknown; i++) {
        char name[256];
        if (input_channels(ids[i]) == 0)
            continue;
        device_name(ids[i], name, sizeof(name));
        if (sa_name_matches(name, product))
            found = ids[i];
    }
    free(ids);
    return found;
}

/* The computer's Line In: an input named "Line In", then a built-in input with a Line In data
 * source (selected here), then the default input unless that is the capture box. */
static AudioDeviceID find_line_in(void)
{
    AudioDeviceID *ids, found = kAudioDeviceUnknown, fallback = kAudioDeviceUnknown;
    int n = list_devices(&ids);
    for (int i = 0; i < n && found == kAudioDeviceUnknown; i++) {
        char name[256];
        if (input_channels(ids[i]) == 0)
            continue;
        device_name(ids[i], name, sizeof(name));
        if (sa_line_name_matches(name)) {
            found = ids[i];
            break;
        }
        if (sa_box_name(name))
            continue;
        UInt32 sz = 0;
        Boolean writable;
        if (!AudioDeviceGetPropertyInfo(ids[i], 0, true, kAudioDevicePropertyDataSources, &sz, &writable) && sz) {
            UInt32 *srcs = malloc(sz);
            if (srcs && !AudioDeviceGetProperty(ids[i], 0, true, kAudioDevicePropertyDataSources, &sz, srcs)) {
                for (UInt32 k = 0; k < sz / sizeof(UInt32); k++) {
                    if (srcs[k] == 0x6c696e65) { /* 'line' */
                        UInt32 v = srcs[k];
                        AudioDeviceSetProperty(ids[i], NULL, 0, true, kAudioDevicePropertyDataSource, sizeof(v), &v);
                        found = ids[i];
                        break;
                    }
                }
            }
            free(srcs);
        }
        if (fallback == kAudioDeviceUnknown)
            fallback = ids[i];
    }
    free(ids);
    if (found != kAudioDeviceUnknown)
        return found;
    AudioDeviceID def = kAudioDeviceUnknown;
    UInt32 sz = sizeof(def);
    if (!AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice, &sz, &def) && def != kAudioDeviceUnknown &&
        input_channels(def) > 0) {
        char name[256];
        device_name(def, name, sizeof(name));
        if (!sa_box_name(name))
            return def;
    }
    return fallback;
}

/* ---- input ---- */

#define CHUNK 1024

struct sa_input {
    AudioDeviceID dev;
    sa_input_cb cb;
    void *user;
    int16_t buf[CHUNK * 2];
};

static inline int16_t to_s16(float v)
{
    v *= 32768.0f;
    return (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)v);
}

static OSStatus input_proc(AudioDeviceID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                           const AudioTimeStamp *in_time, AudioBufferList *out, const AudioTimeStamp *out_time,
                           void *ctx)
{
    (void)dev, (void)now, (void)in_time, (void)out, (void)out_time;
    sa_input_t *c = ctx;
    if (!in || !in->mNumberBuffers || !in->mBuffers[0].mData || !in->mBuffers[0].mNumberChannels)
        return noErr;
    /* interleaved (one buffer, 1 or more channels) or one buffer per channel */
    const AudioBuffer *bufs = in->mBuffers; /* mNumberBuffers of them, though declared [1] */
    const AudioBuffer *b0 = &bufs[0];
    const AudioBuffer *b1 = in->mNumberBuffers > 1 && bufs[1].mData ? &bufs[1] : NULL;
    UInt32 ch0 = b0->mNumberChannels;
    UInt32 frames = b0->mDataByteSize / (sizeof(float) * ch0);
    const float *p0 = b0->mData, *p1 = b1 ? b1->mData : NULL;
    for (UInt32 done = 0; done < frames;) {
        UInt32 n = frames - done > CHUNK ? CHUNK : frames - done;
        for (UInt32 i = 0; i < n; i++) {
            UInt32 f = done + i;
            float l = p0[f * ch0], r = ch0 > 1 ? p0[f * ch0 + 1] : p1 ? p1[f * b1->mNumberChannels] : l;
            c->buf[2 * i] = to_s16(l);
            c->buf[2 * i + 1] = to_s16(r);
        }
        c->cb(c->buf, (int)n, c->user);
        done += n;
    }
    return noErr;
}

const char *sa_backend_name(void) { return "Core Audio"; }

sa_input_t *sa_input_start(uint16_t vid, uint16_t pid, const char *product, sa_input_cb cb, void *user, int *rate)
{
    (void)vid, (void)pid;
    sa_set_error(NULL);
    int line = sa_line_in();
    AudioDeviceID id = line ? find_line_in() : find_device(product);
    if (id == kAudioDeviceUnknown) {
        sa_set_error(line ? "Mac OS X shows no Line In sound input (check System Preferences > Sound > Input)"
                          : "Mac OS X shows no sound input for the Dazzle (check System Preferences > Sound > Input)");
        return NULL;
    }
    AudioStreamBasicDescription f;
    UInt32 sz = sizeof(f);
    memset(&f, 0, sizeof(f));
    if (AudioDeviceGetProperty(id, 0, true, kAudioDevicePropertyStreamFormat, &sz, &f) || f.mSampleRate <= 0)
        f.mSampleRate = 48000;

    sa_input_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->dev = id;
    c->cb = cb;
    c->user = user;
    if (AudioDeviceAddIOProc(id, input_proc, c)) {
        sa_set_error("Mac OS X would not open the sound input");
        free(c);
        return NULL;
    }
    if (AudioDeviceStart(id, input_proc)) {
        AudioDeviceRemoveIOProc(id, input_proc);
        sa_set_error("Mac OS X would not start the sound input");
        free(c);
        return NULL;
    }
    if (rate)
        *rate = (int)f.mSampleRate;
    return c;
}

void sa_input_stop(sa_input_t *c)
{
    if (!c)
        return;
    AudioDeviceStop(c->dev, input_proc);
    AudioDeviceRemoveIOProc(c->dev, input_proc);
    free(c);
}

/* ---- output ---- */

struct sa_output {
    AudioUnit unit;
    sa_output_cb cb;
    void *user;
};

static OSStatus render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *abl)
{
    (void)flags, (void)ts, (void)bus;
    sa_output_t *o = ctx;
    o->cb((int16_t *)abl->mBuffers[0].mData, (int)frames, o->user);
    return noErr;
}

sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user)
{
    ComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple,
                               0, 0 };
    Component comp = FindNextComponent(NULL, &d);
    if (!comp)
        return NULL;
    sa_output_t *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->cb = cb;
    o->user = user;
    if (OpenAComponent(comp, &o->unit)) {
        free(o);
        return NULL;
    }
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof(f));
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked
#if defined(__BIG_ENDIAN__)
                     | kLinearPCMFormatFlagIsBigEndian
#endif
        ;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    AURenderCallbackStruct rcb = { render, o };
    if (AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) ||
        AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rcb,
                             sizeof(rcb)) ||
        AudioUnitInitialize(o->unit) || AudioOutputUnitStart(o->unit)) {
        CloseComponent(o->unit);
        free(o);
        return NULL;
    }
    return o;
}

void sa_output_stop(sa_output_t *o)
{
    if (!o)
        return;
    AudioOutputUnitStop(o->unit);
    AudioUnitUninitialize(o->unit);
    CloseComponent(o->unit);
    free(o);
}
