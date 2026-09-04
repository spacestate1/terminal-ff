#define _POSIX_C_SOURCE 200809L

#include "sound.h"

#include <alsa/asoundlib.h>
#include <mpg123.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The clip is decoded once at startup and kept as interleaved 16-bit samples.
// A dedicated thread owns the ALSA handle, so the input callback that triggers
// a sound only has to bump a counter - it never blocks the render loop on the
// audio device.
static short*  s_samples;      // interleaved, s_frames * s_channels shorts
static size_t  s_frames;
static int     s_channels;
static long    s_rate;

static snd_pcm_t* s_device;

static pthread_t       s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_wake = PTHREAD_COND_INITIALIZER;

static unsigned long s_requested;   // bumped by sound_play()
static unsigned long s_served;      // generation the thread last started on
static bool s_thread_running;
static bool s_available;
static bool s_enabled = true;

// Returns true if any channel of this frame is above the audible threshold.
static bool frame_is_audible(size_t frame, short threshold) {
    for (int c = 0; c < s_channels; c++) {
        short sample = s_samples[frame * (size_t)s_channels + c];
        if (sample > threshold || sample < -threshold) return true;
    }
    return false;
}

// MP3s routinely carry a few hundred milliseconds of silence at the head -
// encoder delay plus whatever the clip was cut with. Tied to a keypress that
// reads as input lag, so drop the quiet run at both ends and start playback on
// the attack instead.
static void trim_silence(void) {
    const short threshold = 256;   // roughly -42 dBFS

    size_t first = 0;
    while (first < s_frames && !frame_is_audible(first, threshold)) first++;
    if (first == s_frames) return;   // entirely silent: leave it alone

    size_t last = s_frames;
    while (last > first && !frame_is_audible(last - 1, threshold)) last--;

    // Keep a few milliseconds of run-up so playback does not begin on a
    // non-zero sample and click.
    size_t preroll = (size_t)(s_rate / 300);   // ~3ms
    first = first > preroll ? first - preroll : 0;

    if (first == 0 && last == s_frames) return;

    double lead_ms = (double)first / (double)s_rate * 1000.0;
    double tail_ms = (double)(s_frames - last) / (double)s_rate * 1000.0;

    size_t kept = last - first;
    memmove(s_samples, s_samples + first * (size_t)s_channels,
            kept * (size_t)s_channels * sizeof(short));
    s_frames = kept;

    printf("sound: trimmed %.0fms of lead-in and %.0fms of tail silence\n",
           lead_ms, tail_ms);
}

// Decode the whole file to signed 16-bit PCM. Small UI clips are a few hundred
// kilobytes at most, so holding all of it beats decoding on every keypress.
static bool decode_file(const char* path) {
    if (mpg123_init() != MPG123_OK) {
        return false;
    }

    int err = MPG123_OK;
    mpg123_handle* mh = mpg123_new(NULL, &err);
    if (!mh) {
        mpg123_exit();
        return false;
    }

    if (mpg123_open(mh, path) != MPG123_OK) {
        fprintf(stderr, "sound: cannot open %s: %s\n", path, mpg123_strerror(mh));
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    long rate = 0;
    int channels = 0, encoding = 0;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    // Pin the output format so the decoder cannot switch mid-stream.
    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, MPG123_ENC_SIGNED_16);

    unsigned char* pcm = NULL;
    size_t len = 0, cap = 0;
    unsigned char chunk[16384];
    size_t got = 0;
    int status;

    while ((status = mpg123_read(mh, chunk, sizeof chunk, &got)) == MPG123_OK ||
           status == MPG123_NEW_FORMAT) {
        if (got == 0) continue;

        if (len + got > cap) {
            size_t want = (len + got) * 2;
            unsigned char* grown = realloc(pcm, want);
            if (!grown) {
                free(pcm);
                mpg123_close(mh);
                mpg123_delete(mh);
                mpg123_exit();
                return false;
            }
            pcm = grown;
            cap = want;
        }

        memcpy(pcm + len, chunk, got);
        len += got;
    }

    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    if (!pcm || len < sizeof(short) * (size_t)channels) {
        free(pcm);
        fprintf(stderr, "sound: %s decoded to nothing\n", path);
        return false;
    }

    s_samples  = (short*)pcm;
    s_channels = channels;
    s_rate     = rate;
    s_frames   = len / (sizeof(short) * (size_t)channels);

    trim_silence();
    return true;
}

static bool open_device(void) {
    if (snd_pcm_open(&s_device, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        s_device = NULL;
        return false;
    }

    // 20ms of buffering. The clip is triggered by a keypress, so queue depth
    // is felt directly as lag; a dedicated thread feeds it, so the render loop
    // cannot starve it the way a shared one would.
    int rc = snd_pcm_set_params(s_device,
                                SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                (unsigned int)s_channels,
                                (unsigned int)s_rate,
                                1,         // allow resampling
                                20000);    // latency, microseconds
    if (rc < 0) {
        fprintf(stderr, "sound: device rejected %ldHz x%d: %s\n",
                s_rate, s_channels, snd_strerror(rc));
        snd_pcm_close(s_device);
        s_device = NULL;
        return false;
    }

    return true;
}

static void* playback_thread(void* arg) {
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&s_lock);
        while (s_thread_running && s_requested == s_served) {
            pthread_cond_wait(&s_wake, &s_lock);
        }
        if (!s_thread_running) {
            pthread_mutex_unlock(&s_lock);
            break;
        }
        unsigned long generation = s_requested;
        s_served = generation;
        pthread_mutex_unlock(&s_lock);

        // Drop whatever is queued so a retrigger starts from the top rather
        // than tailing the previous play.
        snd_pcm_drop(s_device);
        snd_pcm_prepare(s_device);

        size_t frame = 0;
        while (frame < s_frames) {
            pthread_mutex_lock(&s_lock);
            bool interrupted = (s_requested != generation) || !s_thread_running;
            pthread_mutex_unlock(&s_lock);
            if (interrupted) break;

            snd_pcm_uframes_t want = s_frames - frame;
            if (want > 512) want = 512;

            snd_pcm_sframes_t wrote =
                snd_pcm_writei(s_device, s_samples + frame * (size_t)s_channels, want);

            if (wrote < 0) {
                if (snd_pcm_recover(s_device, (int)wrote, 1) < 0) break;
                continue;   // recovered: retry this chunk
            }
            frame += (size_t)wrote;
        }

        if (frame >= s_frames) {
            snd_pcm_drain(s_device);
        }
    }

    return NULL;
}

bool sound_init(const char* path) {
    if (s_available || !path) return s_available;

    if (!decode_file(path)) {
        return false;
    }

    if (!open_device()) {
        fprintf(stderr, "sound: no audio device, continuing without sound\n");
        free(s_samples);
        s_samples = NULL;
        return false;
    }

    // Get the device into a prepared state now, so the first keypress does not
    // pay for setup on top of playback.
    snd_pcm_prepare(s_device);

    s_thread_running = true;
    if (pthread_create(&s_thread, NULL, playback_thread, NULL) != 0) {
        s_thread_running = false;
        snd_pcm_close(s_device);
        s_device = NULL;
        free(s_samples);
        s_samples = NULL;
        return false;
    }

    s_available = true;
    return true;
}

void sound_play(void) {
    if (!s_available || !s_enabled) return;

    pthread_mutex_lock(&s_lock);
    s_requested++;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_lock);
}

void sound_set_enabled(bool enabled) {
    s_enabled = enabled;
}

bool sound_is_enabled(void) {
    return s_enabled;
}

bool sound_is_available(void) {
    return s_available;
}

void sound_shutdown(void) {
    if (!s_available) return;

    pthread_mutex_lock(&s_lock);
    s_thread_running = false;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_thread, NULL);

    if (s_device) {
        snd_pcm_close(s_device);
        s_device = NULL;
    }

    free(s_samples);
    s_samples = NULL;
    s_available = false;
}
