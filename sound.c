#define _POSIX_C_SOURCE 200809L

#include "sound.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <mpg123.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static unsigned long s_cancelled;   // clips up to this generation are abandoned
static bool s_thread_running;
static bool s_available;
static bool s_enabled = true;

// When the device is unusable, the next attempt to reopen it is held off until
// s_next_open_attempt. snd_pcm_open() can take a while to fail when the sound
// server has gone away, and without a backoff every keypress would queue
// another slow attempt behind the last one.
//
// The gap widens with each consecutive failure. Reopening is how sound comes
// back after a device is unplugged and replugged, so it is worth retrying
// indefinitely - but on a device that is gone for good a fixed short timer is
// pure cost: measured under LeakSanitizer, every open/close cycle strands a
// PipeWire context inside the ALSA plugin layer, which nothing here can free.
static double s_next_open_attempt;
static double s_open_backoff;
#define REOPEN_BACKOFF_MIN 5.0
#define REOPEN_BACKOFF_MAX 60.0

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

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

#define MAX_CLIP_BYTES (32u * 1024u * 1024u)

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

    // Keep libmpg123's own diagnostics off the terminal's stderr. A bad or
    // missing clip is reported below in one line the user can act on; the
    // library's internal resync notes are noise on top of it.
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);

    if (mpg123_open(mh, path) != MPG123_OK) {
        fprintf(stderr, "sound: cannot open %s: %s\n", path, mpg123_strerror(mh));
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    long rate = 0;
    int channels = 0, encoding = 0;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK ||
        channels <= 0 || rate <= 0) {
        // A zero channel count would divide by zero working out the frame
        // count, and a zero rate would do the same in the playback deadline.
        fprintf(stderr, "sound: %s has an unusable format (%ldHz x%d)\n",
                path, rate, channels);
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

        // A keypress blip is a few hundred kilobytes. Anything approaching
        // this is not the file we were pointed at, so stop rather than let a
        // bad path grow the buffer without limit.
        if (len + got > MAX_CLIP_BYTES) {
            fprintf(stderr, "sound: %s is too large for a UI clip, truncating\n", path);
            break;
        }

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

// Open the device non-blocking, so writes return -EAGAIN instead of blocking
// and every wait in play_clip() can carry a timeout.
static bool open_device(void) {
    static bool complained = false;

    int rc = snd_pcm_open(&s_device, "default", SND_PCM_STREAM_PLAYBACK,
                          SND_PCM_NONBLOCK);
    if (rc < 0) {
        s_device = NULL;
        if (!complained) {
            fprintf(stderr, "sound: cannot open the audio device: %s\n",
                    snd_strerror(rc));
            complained = true;
        }
        return false;
    }
    complained = false;

    // 20ms of buffering. The clip is triggered by a keypress, so queue depth
    // is felt directly as lag; a dedicated thread feeds it, so the render loop
    // cannot starve it the way a shared one would.
    rc = snd_pcm_set_params(s_device,
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

    // set_params does not touch the blocking mode, but say so explicitly: the
    // whole write loop below depends on getting -EAGAIN rather than a block.
    snd_pcm_nonblock(s_device, 1);
    return true;
}

static void close_device(void) {
    if (s_device) {
        snd_pcm_close(s_device);
        s_device = NULL;
    }
}

#define WAIT_TIMEOUT_MS 100     // longest single wait for buffer space
#define CLIP_SLACK_SECONDS 2.0  // allowance on top of the clip's own length

// One trigger of the clip. Returns false if the device could not be brought
// back to a usable state, which tells the caller to close it and try again
// from scratch on the next trigger.
//
// Two properties this has to hold, because between them they cover every way
// the sound could stop after a few keypresses while the terminal carried on:
//
//   1. Every wait is bounded. Nothing below can hold this thread for longer
//      than WAIT_TIMEOUT_MS, so it always gets back to the interrupt check -
//      and sound_shutdown()'s join always returns. A thread parked in a
//      blocking ALSA call is silent from then on and hangs the exit.
//   2. Every failure is recoverable. Errors used to leave the device in
//      whatever state it had failed in, and since the return values of drop
//      and prepare went unchecked, one bad clip could silence the rest of the
//      session with nothing printed. Now a wedged device is closed and
//      reopened on the next keypress.
static bool play_clip(unsigned long generation) {
    double now = monotonic_seconds();

    // Reopen lazily. A device that wedged on the last clip was closed rather
    // than left broken, so this is the path back to working sound instead of
    // staying silent for the rest of the session - but back off between tries
    // so a device that is gone for good is not reopened on every keypress.
    if (!s_device) {
        if (now < s_next_open_attempt) return false;

        if (s_open_backoff < REOPEN_BACKOFF_MIN) s_open_backoff = REOPEN_BACKOFF_MIN;

        if (!open_device()) {
            s_next_open_attempt = now + s_open_backoff;
            s_open_backoff *= 2.0;
            if (s_open_backoff > REOPEN_BACKOFF_MAX) s_open_backoff = REOPEN_BACKOFF_MAX;
            return false;
        }

        s_next_open_attempt = 0.0;
        s_open_backoff = REOPEN_BACKOFF_MIN;
    }

    // Drop whatever is queued so a retrigger starts from the top rather than
    // tailing the previous play. -EBADFD just means the stream was already
    // stopped, which is the normal state here.
    int rc = snd_pcm_drop(s_device);
    if (rc < 0 && rc != -EBADFD) {
        fprintf(stderr, "sound: cannot stop the device: %s\n", snd_strerror(rc));
        return false;
    }

    rc = snd_pcm_prepare(s_device);
    if (rc < 0) {
        fprintf(stderr, "sound: cannot prepare the device: %s\n", snd_strerror(rc));
        return false;
    }

    // One deadline covers every way this loop could fail to make progress -
    // a wait that keeps reporting the device ready while the write keeps
    // returning -EAGAIN, or a write that accepts zero frames. Counting
    // individual timeouts missed both of those and left the thread spinning.
    double deadline = now + (double)s_frames / (double)s_rate + CLIP_SLACK_SECONDS;

    size_t frame = 0;

    while (frame < s_frames) {
        pthread_mutex_lock(&s_lock);
        bool interrupted = (s_requested != generation)
                        || (generation <= s_cancelled)
                        || !s_thread_running;
        pthread_mutex_unlock(&s_lock);
        if (interrupted) return true;

        if (monotonic_seconds() > deadline) {
            fprintf(stderr, "sound: giving up on a clip the device would not take\n");
            return false;
        }

        snd_pcm_uframes_t want = s_frames - frame;
        if (want > 512) want = 512;

        snd_pcm_sframes_t wrote =
            snd_pcm_writei(s_device, s_samples + frame * (size_t)s_channels, want);

        if (wrote == -EAGAIN) {
            // The buffer is full: wait for room, with a timeout so a stalled
            // device cannot park this thread. Returning to the top of the loop
            // is also what lets a retrigger or a shutdown be noticed promptly.
            int ready = snd_pcm_wait(s_device, WAIT_TIMEOUT_MS);
            if (ready < 0 && snd_pcm_recover(s_device, ready, 1) < 0) {
                fprintf(stderr, "sound: device error while waiting: %s\n",
                        snd_strerror(ready));
                return false;
            }
            continue;
        }

        if (wrote < 0) {
            if (snd_pcm_recover(s_device, (int)wrote, 1) < 0) {
                fprintf(stderr, "sound: unrecoverable write error: %s\n",
                        snd_strerror((int)wrote));
                return false;
            }
            continue;
        }

        frame += (size_t)wrote;
    }

    // Deliberately no snd_pcm_drain() here. Drain blocks until the queued tail
    // has played out, with no timeout to fall back on, and it is the longest
    // unbounded wait this thread had. It also buys nothing: no other thread
    // touches the device, so the tail plays out on its own while this one goes
    // back to waiting, and the underrun that follows is cleared by the
    // drop/prepare above the next time the clip is triggered.
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

        if (!play_clip(generation)) {
            // Unrecoverable. Close it here and let the next trigger reopen:
            // one bad clip should not cost the session its sound.
            close_device();
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

    // Start from a clean slate. init after a shutdown would otherwise inherit
    // the old counters, and a trigger that arrived just before the last thread
    // stopped would play the moment the new one starts.
    s_requested = 0;
    s_served = 0;
    s_cancelled = 0;
    s_next_open_attempt = 0.0;
    s_open_backoff = REOPEN_BACKOFF_MIN;

    // Keep signal delivery on the main thread. Handlers are process-wide and
    // any thread can take a signal; if SIGINT or SIGTERM landed here, the
    // render loop would not see its own shutdown flag until something else
    // woke it, and the audio path would collect spurious EINTR on top. The new
    // thread inherits this mask, so blocking across the create is what pins
    // delivery to the caller.
    sigset_t blocked, saved;
    sigfillset(&blocked);
    pthread_sigmask(SIG_SETMASK, &blocked, &saved);

    s_thread_running = true;
    int rc = pthread_create(&s_thread, NULL, playback_thread, NULL);

    pthread_sigmask(SIG_SETMASK, &saved, NULL);

    if (rc != 0) {
        s_thread_running = false;
        close_device();
        free(s_samples);
        s_samples = NULL;
        fprintf(stderr, "sound: cannot start the playback thread: %s\n",
                strerror(rc));
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

    if (!enabled) {
        // Muting should take effect now, not at the end of whatever is
        // playing: turning the sound off in the menu and still hearing the
        // keypress that did it reads as the setting not having worked.
        pthread_mutex_lock(&s_lock);
        s_cancelled = s_requested;
        pthread_mutex_unlock(&s_lock);
    }
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

    // Only safe to release these now the thread is joined: it reads s_samples
    // and s_device without the lock for the whole length of a clip.
    close_device();

    free(s_samples);
    s_samples = NULL;
    s_frames = 0;
    s_available = false;
}
