/*
 * The operating system's audio devices: the capture box's own input
 * (when the OS audio driver owns its USB audio interface) and the default
 * output for monitoring.
 *
 *   macOS    Core Audio   (audio_coreaudio.c, input only; the app plays through AVAudioEngine)
 *   Windows  WASAPI in, waveOut out (audio_windows.c)
 *   Linux    ALSA, loaded at run time with dlopen (audio_alsa.c)
 */
#ifndef DVC100_SYSAUDIO_H
#define DVC100_SYSAUDIO_H

#include <stdint.h>

typedef struct sa_input sa_input_t;
typedef void (*sa_input_cb)(const int16_t *samples, int frames, void *user);

/* Finds the input device of the USB box vid:pid (matching its USB product
 * name, or one named "Dazzle"/"Roxio"; DVC100_AUDIO_DEVICE overrides with a
 * name substring) and streams interleaved 16-bit stereo to cb from its own
 * thread. Returns NULL if there is no such device. */
sa_input_t *sa_input_start(uint16_t vid, uint16_t pid, const char *product, sa_input_cb cb, void *user,
                           int *rate);
void sa_input_stop(sa_input_t *in);
const char *sa_backend_name(void);

/* Playback to the default output device. cb fills `frames` interleaved
 * 16-bit stereo frames from the audio thread. */
typedef struct sa_output sa_output_t;
typedef void (*sa_output_cb)(int16_t *out, int frames, void *user);
sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user);
void sa_output_stop(sa_output_t *out);

/* Case-insensitive substring test used by the device matchers. */
int sa_name_matches(const char *name, const char *product);

#endif
