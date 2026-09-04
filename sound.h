#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>

// Decode a sound file into memory once and start the playback thread.
// Returns false if the file or the audio device is unavailable - the caller is
// expected to carry on without sound rather than treat it as fatal.
bool sound_init(const char* path);

// Trigger playback. Safe to call from an input callback: it never blocks and
// never touches the audio device itself. Triggering while the clip is already
// playing restarts it from the beginning. Does nothing while muted.
void sound_play(void);

// Mute / unmute. Muting is independent of whether audio came up at all, so the
// setting survives a failed sound_init() and takes effect if audio returns.
void sound_set_enabled(bool enabled);
bool sound_is_enabled(void);

// True if the device opened and a clip is loaded, i.e. sound_play() can make
// noise when unmuted.
bool sound_is_available(void);

// Stop the playback thread and release the device and decoded samples.
void sound_shutdown(void);

#endif // SOUND_H
