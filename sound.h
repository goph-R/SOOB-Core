#ifndef SOUND_H
#define SOUND_H

/*
 * OpenAL audio wrapper (OpenAL 1.1 / OpenAL Soft, LGPL).
 * Works from Win98 (OpenAL Soft 1.9.563) up through modern Linux/Win10
 * (OpenAL Soft 1.25.x) with a single `-lopenal` / `-lOpenAL32` link.
 *
 * API:
 *   sndInit(&sys, sampleRate)  — open device, make context, allocate sources
 *   sndShutdown(&sys)          — tear down
 *   sndMakeBuffer(pcm, numSamples, sampleRate) — 16-bit signed mono PCM
 *   sndLoadWav(path)           — 16-bit PCM WAV (mono or stereo→mono mix)
 *   sndLoadOgg(path)           — .ogg decoded once at load via stb_vorbis
 *                                (mono or stereo→mono mix)
 *   sndLoad(path)              — dispatches by ".ogg" / ".wav" extension
 *   sndFreeBuffer(buffer)
 *   sndPlay(&sys, buffer)      — fire-and-forget on any free source
 *
 * Named registry (SoundLibrary): each entry is a *group* of up to
 * SND_MAX_VARIANTS buffers. Single sounds are just groups of one.
 * sndLibPick(&lib, "steps") returns a uniformly-random non-repeating
 * variant from the group; for a 1-variant entry it returns that buffer.
 * Per-entry lastIdx state means groups don't interfere with each other.
 *
 * Positional audio:
 *   sndUpdateListener(&sys, pos, forward, up) — call once per frame after
 *     camera math; sets the OpenAL listener's position + orientation.
 *   sndPlayAt(&sys, buf, pos) — fire-and-forget at a world position.
 *   sndPlay(&sys, buf) — head-relative (player events, UI, music).
 * sndPlay forces SOURCE_RELATIVE = TRUE and position = (0,0,0) per call,
 * so a source previously used by sndPlayAt won't leak its world position
 * into the next 2D play. Default OpenAL inverse-distance attenuation
 * (reference 1m, rolloff 1) is left as-is.
 *
 * Relies on SDL.h being included before this header. main.cpp does so.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <AL/al.h>
#include <AL/alc.h>

/* stb_vorbis declarations only — the implementation lives in vorbis.o
   (compiled separately from vendor/stb/stb_vorbis.c). Same trick music.h
   uses; both files including the header is safe via stb_vorbis.c's
   own include guard. */
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb/stb_vorbis.c"

#include "math.h"

#define SND_NUM_SOURCES 16

typedef ALuint SoundBuffer;

struct SoundSystem {
    ALCdevice  *device;
    ALCcontext *context;
    ALuint     sources[SND_NUM_SOURCES];
};

static int sndInit(SoundSystem *s, int /*sampleRate*/)
{
    s->device = alcOpenDevice(NULL);
    if (!s->device) {
        conLogf("OpenAL: alcOpenDevice failed\n");
        return 0;
    }
    s->context = alcCreateContext(s->device, NULL);
    if (!s->context) {
        conLogf("OpenAL: alcCreateContext failed\n");
        alcCloseDevice(s->device);
        return 0;
    }
    alcMakeContextCurrent(s->context);
    alGenSources(SND_NUM_SOURCES, s->sources);
    return 1;
}

static void sndShutdown(SoundSystem *s)
{
    alDeleteSources(SND_NUM_SOURCES, s->sources);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(s->context);
    alcCloseDevice(s->device);
}

static SoundBuffer sndMakeBuffer(const short *pcm, int numSamples, int sampleRate)
{
    ALuint buf = 0;
    alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, pcm,
                 numSamples * (int)sizeof(short), sampleRate);
    return buf;
}

static void sndFreeBuffer(SoundBuffer buf)
{
    if (buf) alDeleteBuffers(1, &buf);
}

/* Load a mono or stereo 16-bit PCM WAV and upload it as a mono buffer.
   Stereo is downmixed by averaging L/R. Other formats (8-bit, float,
   ADPCM) are rejected with a stderr message and the call returns 0.
   Sample rate is passed through — OpenAL handles the resampling. */
static SoundBuffer sndLoadWav(const char *path)
{
    SDL_AudioSpec spec;
    Uint8 *data = NULL;
    Uint32 len  = 0;
    if (!SDL_LoadWAV(path, &spec, &data, &len)) {
        conLogf("sndLoadWav: %s: %s\n", path, SDL_GetError());
        return 0;
    }
    if (spec.format != AUDIO_S16LSB && spec.format != AUDIO_S16SYS) {
        conLogf("sndLoadWav: %s: unsupported format 0x%x (need 16-bit PCM)\n",
                path, (unsigned)spec.format);
        SDL_FreeWAV(data);
        return 0;
    }
    const int bytesPerFrame = 2 * spec.channels;
    const int frames = (int)(len / (Uint32)bytesPerFrame);
    SoundBuffer out = 0;
    if (spec.channels == 1) {
        out = sndMakeBuffer((const short *)data, frames, spec.freq);
    } else if (spec.channels == 2) {
        short *mono = (short *)malloc((size_t)frames * sizeof(short));
        const short *s = (const short *)data;
        for (int i = 0; i < frames; i++) {
            int l = s[i * 2 + 0];
            int r = s[i * 2 + 1];
            mono[i] = (short)((l + r) / 2);
        }
        out = sndMakeBuffer(mono, frames, spec.freq);
        free(mono);
    } else {
        conLogf("sndLoadWav: %s: unsupported channel count %d\n",
                path, (int)spec.channels);
    }
    SDL_FreeWAV(data);
    return out;
}

/* Load an .ogg as a fully-decoded mono buffer (no streaming — SFX want
   zero-latency playback). Stereo is downmixed by averaging L/R. Sample
   rate is passed through and OpenAL handles any resampling. */
static SoundBuffer sndLoadOgg(const char *path)
{
    int channels = 0, sampleRate = 0;
    short *pcm = NULL;
    int frames = stb_vorbis_decode_filename(path, &channels, &sampleRate, &pcm);
    if (frames <= 0 || !pcm) {
        conLogf("sndLoadOgg: %s: decode failed\n", path);
        if (pcm) free(pcm);
        return 0;
    }
    SoundBuffer out = 0;
    if (channels == 1) {
        out = sndMakeBuffer(pcm, frames, sampleRate);
    } else if (channels == 2) {
        short *mono = (short *)malloc((size_t)frames * sizeof(short));
        for (int i = 0; i < frames; i++) {
            int l = pcm[i * 2 + 0];
            int r = pcm[i * 2 + 1];
            mono[i] = (short)((l + r) / 2);
        }
        out = sndMakeBuffer(mono, frames, sampleRate);
        free(mono);
    } else {
        conLogf("sndLoadOgg: %s: unsupported channel count %d\n", path, channels);
    }
    free(pcm);
    return out;
}

/* Dispatch to the right loader by file extension. Anything that's not
   ".ogg" / ".OGG" is treated as a WAV. Keeps script-side asset loading
   format-agnostic. */
static SoundBuffer sndLoad(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot && (strcmp(dot, ".ogg") == 0 || strcmp(dot, ".OGG") == 0)) {
        return sndLoadOgg(path);
    }
    return sndLoadWav(path);
}

/* ---- Named registry ----
 * Each entry holds up to SND_MAX_VARIANTS buffers under one name.
 * sndLibAdd appends a buffer to the named entry (creating it the first
 * time). Single sounds and randomized groups go through the same path. */
#define SND_MAX_NAMED    64
#define SND_MAX_VARIANTS 4

struct SoundEntry {
    char        name[32];
    SoundBuffer bufs[SND_MAX_VARIANTS];
    int         count;        /* 1 for a single sound, N for a group */
    int         lastIdx;      /* last picked variant; -1 = none yet */
};

struct SoundLibrary {
    SoundEntry entries[SND_MAX_NAMED];
    int        count;
};

static void sndLibInit(SoundLibrary *lib)
{
    memset(lib, 0, sizeof(*lib));
}

static SoundEntry *sndLibFindEntry(SoundLibrary *lib, const char *name)
{
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].name, name) == 0) return &lib->entries[i];
    }
    return NULL;
}

/* Append a buffer to the named entry. Creates the entry on first call.
   Skips silently if the upstream load failed (b == 0). */
static void sndLibAdd(SoundLibrary *lib, const char *name, SoundBuffer b)
{
    if (!b) return;
    SoundEntry *e = sndLibFindEntry(lib, name);
    if (!e) {
        if (lib->count >= SND_MAX_NAMED) {
            conLogf("sndLibAdd: registry full, dropping '%s'\n", name);
            return;
        }
        e = &lib->entries[lib->count++];
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->count = 0;
        e->lastIdx = -1;
    }
    if (e->count >= SND_MAX_VARIANTS) {
        conLogf("sndLibAdd: '%s' already has %d variants, dropping extra\n",
                name, SND_MAX_VARIANTS);
        return;
    }
    e->bufs[e->count++] = b;
}

/* Pick a random non-repeating variant from the named group and return
   its buffer. Returns 0 if the name isn't registered. For a 1-variant
   entry the picker short-circuits to bufs[0]. */
static SoundBuffer sndLibPick(SoundLibrary *lib, const char *name)
{
    SoundEntry *e = sndLibFindEntry(lib, name);
    if (!e || e->count == 0) return 0;
    if (e->count == 1) return e->bufs[0];

    /* Exclusion-shift: pick from count-1 candidates, then bump past the
       last index to skip it. Uniform over the count-1 valid choices. */
    int r = rand() % (e->count - 1);
    if (e->lastIdx >= 0 && r >= e->lastIdx) r++;
    e->lastIdx = r;
    return e->bufs[r];
}

static void sndLibShutdown(SoundLibrary *lib)
{
    for (int i = 0; i < lib->count; i++) {
        SoundEntry *e = &lib->entries[i];
        for (int j = 0; j < e->count; j++) sndFreeBuffer(e->bufs[j]);
    }
    lib->count = 0;
}

/* Find a non-playing source. If all sources are busy, return source[0]
   (stolen). Internal helper for sndPlay / sndPlayAt. */
static ALuint sndPickSource(SoundSystem *s)
{
    for (int i = 0; i < SND_NUM_SOURCES; i++) {
        ALint state = AL_INITIAL;
        alGetSourcei(s->sources[i], AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING && state != AL_PAUSED) return s->sources[i];
    }
    return s->sources[0];
}

/* Head-relative play (2D). For player-self events, UI, music. */
static void sndPlay(SoundSystem *s, SoundBuffer buf)
{
    if (!buf) return;
    ALuint src = sndPickSource(s);
    alSourceStop(src);
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcePlay(src);
}

/* World-positioned play (3D). For NPC sounds, environment, distant fire. */
static void sndPlayAt(SoundSystem *s, SoundBuffer buf, Vec3 pos)
{
    if (!buf) return;
    ALuint src = sndPickSource(s);
    alSourceStop(src);
    alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
    alSource3f(src, AL_POSITION, pos.x, pos.y, pos.z);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcePlay(src);
}

/* Update the listener's pose. Call once per frame after computing the
   camera. `forward` is the unit vector the camera looks along; `up` is
   the camera's up axis (Y for an FPS that doesn't roll). */
static void sndUpdateListener(SoundSystem *s, Vec3 pos, Vec3 forward, Vec3 up)
{
    (void)s;
    alListener3f(AL_POSITION, pos.x, pos.y, pos.z);
    float ori[6] = { forward.x, forward.y, forward.z, up.x, up.y, up.z };
    alListenerfv(AL_ORIENTATION, ori);
}

#endif /* SOUND_H */
