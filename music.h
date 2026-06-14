#ifndef MUSIC_H
#define MUSIC_H

/*
 * Streaming Ogg Vorbis music for OpenAL.
 *
 * Sits next to sound.h: 2D head-relative playback, separate sources from
 * the sound effect pool, decoded incrementally so a 5MB .ogg never lives
 * in memory all at once. Built on stb_vorbis (vendor/stb/stb_vorbis.c)
 * compiled as its own TU into raw/obj/vorbis.o; this header pulls in just
 * the prototypes via STB_VORBIS_HEADER_ONLY.
 *
 * Algorithm — per active track (MusicTrack):
 *   - One AL source, MUSIC_NUM_BUFFERS small AL buffers in a ring.
 *   - musicPlay opens the .ogg with stb_vorbis_open_filename, primes the
 *     ring (decode + queue all buffers), then alSourcePlay.
 *   - musicUpdate is the per-frame pump: each call unqueues every buffer
 *     OpenAL has finished consuming, decodes the next ~MUSIC_BUFFER_FRAMES
 *     samples per channel into it, and re-queues. On short read (EOF) we
 *     either stb_vorbis_seek_start (loop=1) or mark eos and let the queue
 *     drain (loop=0). Underruns from a slow disk are recovered with a
 *     defensive alSourcePlay if the source dropped to AL_STOPPED while
 *     buffers were still queued.
 *
 * Crossfade — MUSIC_NUM_TRACKS = 2:
 *   musicPlay with a non-zero fade ramps the *new* track's gain up from 0
 *   while flagging every other active track to fade to 0 and self-destruct
 *   when it hits silence. Two tracks is enough to overlap an outgoing
 *   ambient with an incoming combat loop; a third concurrent fade would
 *   evict the lowest-gain slot.
 *
 * Lifecycle:
 *   - musicInit / musicShutdown bracket the whole subsystem; sources and
 *     buffers are generated once up front and reused across tracks.
 *   - The MusicSystem outlives any one Game session — title music in the
 *     menu, ambient/combat in-game, transitions are just musicPlay calls.
 *
 * MusicLibrary is a name->path map populated from assets.lua (no preload;
 * decoding only starts when musicPlay opens the file). Resolution falls
 * through to a raw path if the name isn't registered, mirroring the
 * AssetRegistry convention.
 *
 * Relies on conLogf being declared before this header (main.cpp does so).
 * Sound.h is the canonical OpenAL include site; this header re-includes
 * <AL/al.h> with its own header guards so include order between sound.h
 * and music.h doesn't matter.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <AL/al.h>
#include <AL/alc.h>

#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb/stb_vorbis.c"

#define MUSIC_NUM_BUFFERS    6        /* ~1.1s lookahead @ 44.1kHz stereo */
#define MUSIC_BUFFER_FRAMES  8192     /* ~0.18s @ 44.1kHz; 32KB stereo s16 */
#define MUSIC_NUM_TRACKS     2        /* enough for one crossfade in flight */
#define MUSIC_PATH_MAX       128
#define MUSIC_NAME_MAX       32
#define MUSIC_LOOP_RETRY     4        /* re-seek attempts per fill before giving
                                         up THIS pump; a looping track never
                                         latches eos — it just retries next frame */

struct MusicTrack {
    stb_vorbis *vorbis;       /* NULL = slot free */
    ALuint     source;
    ALuint     buffers[MUSIC_NUM_BUFFERS];
    int        channels;      /* 1 or 2 — stb_vorbis coerces to this */
    int        sampleRate;
    ALenum     format;        /* AL_FORMAT_MONO16 or AL_FORMAT_STEREO16 */
    int        loop;
    float      gain;          /* current applied gain, 0..1 */
    float      targetGain;    /* gain converges toward this */
    float      fadeRate;      /* gain change per second */
    int        stopOnFadeOut; /* 1 = free the slot when gain hits 0 */
    int        eos;           /* file ended, drain queue then stop */
    int        warned;        /* 1 once a loop re-seek failure has been logged,
                                 so a broken looping file can't spam the log */
    char       name[MUSIC_NAME_MAX];   /* registered logical name, "" if raw path */
    char       path[MUSIC_PATH_MAX];   /* file path, for log + same-track checks */
};

struct MusicSystem {
    MusicTrack tracks[MUSIC_NUM_TRACKS];
    float      masterGain;
};

/* ---- Named registry (name -> path) ---- */
#define MUSIC_LIB_MAX 32

struct MusicLibraryEntry {
    char name[MUSIC_NAME_MAX];
    char path[MUSIC_PATH_MAX];
};

struct MusicLibrary {
    MusicLibraryEntry entries[MUSIC_LIB_MAX];
    int               count;
};

/* ---- Internal helpers ---- */

/* Decode one buffer's worth of samples from track->vorbis and upload it
   to `buf`. Returns 1 if data was uploaded, 0 if there's nothing left
   (caller should NOT requeue). Handles loop / eos transitions. */
static int musicFillBuffer(MusicTrack *t, ALuint buf)
{
    /* Stack-local decode scratch — reused across all calls in a frame.
       MUSIC_BUFFER_FRAMES * 2 channels * sizeof(short) = 32KB. */
    static short decodeBuf[MUSIC_BUFFER_FRAMES * 2];
    int  shorts = MUSIC_BUFFER_FRAMES * t->channels;
    int  framesGot = stb_vorbis_get_samples_short_interleaved(
                         t->vorbis, t->channels, decodeBuf, shorts);

    /* Short read (or zero) means end-of-stream — or, since stb_vorbis reports
       a mid-stream decode glitch the same way, an occasional hiccup. If
       looping, seek back to 0 and fill the remainder of this same buffer so
       the join is glitch-free. Bounded: a corrupt file could otherwise hand
       back nothing forever, and we check seek_start so a failed seek doesn't
       silently spin. */
    if (framesGot < MUSIC_BUFFER_FRAMES && t->loop) {
        int attempts = 0;
        while (framesGot < MUSIC_BUFFER_FRAMES && attempts++ < MUSIC_LOOP_RETRY) {
            if (!stb_vorbis_seek_start(t->vorbis)) {
                if (!t->warned) {
                    conLogf("music: loop re-seek failed on '%s'; will retry "
                            "each pump (track stays alive)\n", t->path);
                    t->warned = 1;
                }
                break;
            }
            int remShorts = (MUSIC_BUFFER_FRAMES - framesGot) * t->channels;
            short *tail = decodeBuf + framesGot * t->channels;
            int more = stb_vorbis_get_samples_short_interleaved(
                           t->vorbis, t->channels, tail, remShorts);
            if (more <= 0) break;
            framesGot += more;
        }
    }

    if (framesGot <= 0) {
        /* Nothing this round. A looping track must never give up: leave the
           slot alive and try again next pump — the source may underrun in the
           meantime, but musicUpdate's AL_STOPPED recovery restarts it the
           moment we manage to requeue. Only a non-looping track ends here. */
        if (!t->loop) t->eos = 1;
        return 0;
    }

    if (framesGot < MUSIC_BUFFER_FRAMES && !t->loop) {
        /* Partial final buffer — upload what we got, mark eos so the pump
           stops trying to feed this track once OpenAL drains. (A looping
           track that came up short still uploads what it has and keeps
           streaming; it never latches eos.) */
        t->eos = 1;
    }

    int bytes = framesGot * t->channels * (int)sizeof(short);
    alBufferData(buf, t->format, decodeBuf, bytes, t->sampleRate);
    return 1;
}

/* Detach buffers, close the decoder, mark slot free. Source itself is
   left intact (regenerated state on next musicPlay). */
static void musicTrackFree(MusicTrack *t)
{
    if (!t->vorbis) return;
    alSourceStop(t->source);
    /* AL_BUFFER = 0 detaches every queued + current buffer in one call. */
    alSourcei(t->source, AL_BUFFER, 0);
    stb_vorbis_close(t->vorbis);
    t->vorbis = NULL;
    t->eos = 0;
    t->warned = 0;
    t->gain = 0.0f;
    t->targetGain = 0.0f;
    t->stopOnFadeOut = 0;
    t->name[0] = '\0';
    t->path[0] = '\0';
}

/* Find a slot to use for a new track. Prefers an empty slot; otherwise
   evicts the slot with the lowest current gain (the one already farthest
   into a fade-out). */
static MusicTrack *musicPickSlot(MusicSystem *m)
{
    for (int i = 0; i < MUSIC_NUM_TRACKS; i++) {
        if (!m->tracks[i].vorbis) return &m->tracks[i];
    }
    int   bestIdx = 0;
    float bestGain = m->tracks[0].gain;
    for (int i = 1; i < MUSIC_NUM_TRACKS; i++) {
        if (m->tracks[i].gain < bestGain) {
            bestGain = m->tracks[i].gain;
            bestIdx = i;
        }
    }
    return &m->tracks[bestIdx];
}

/* ---- Lifecycle ---- */

static int musicInit(MusicSystem *m)
{
    memset(m, 0, sizeof(*m));
    m->masterGain = 1.0f;
    for (int i = 0; i < MUSIC_NUM_TRACKS; i++) {
        MusicTrack *t = &m->tracks[i];
        alGenSources(1, &t->source);
        alGenBuffers(MUSIC_NUM_BUFFERS, t->buffers);
        /* 2D head-relative — music shouldn't pan with the camera. */
        alSourcei(t->source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(t->source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        /* OpenAL's AL_LOOPING doesn't apply to queued buffers — we loop
           by seeking the decoder. Leave it FALSE. */
        alSourcei(t->source, AL_LOOPING, AL_FALSE);
        alSourcef(t->source, AL_GAIN, 0.0f);
    }
    return 1;
}

static void musicShutdown(MusicSystem *m)
{
    for (int i = 0; i < MUSIC_NUM_TRACKS; i++) {
        MusicTrack *t = &m->tracks[i];
        musicTrackFree(t);
        alDeleteSources(1, &t->source);
        alDeleteBuffers(MUSIC_NUM_BUFFERS, t->buffers);
    }
    memset(m, 0, sizeof(*m));
}

/* ---- Library ---- */

static void musicLibInit(MusicLibrary *lib)
{
    memset(lib, 0, sizeof(*lib));
}

static void musicLibAdd(MusicLibrary *lib, const char *name, const char *path)
{
    if (!name || !path) return;
    if (lib->count >= MUSIC_LIB_MAX) {
        conLogf("musicLibAdd: registry full, dropping '%s'\n", name);
        return;
    }
    MusicLibraryEntry *e = &lib->entries[lib->count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
}

/* Look up a registered name. Returns NULL if not found — caller can fall
   through to treating the input as a raw path. */
static const char *musicLibFind(MusicLibrary *lib, const char *name)
{
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].name, name) == 0) return lib->entries[i].path;
    }
    return NULL;
}

/* ---- Playback control ---- */

/* Start a track. `name` is looked up in `lib`; if it's not registered, it
   falls through as a raw path. fadeSec=0 hard-cuts; >0 ramps gain in over
   that many seconds while every other active track ramps out. loop=1
   restarts from sample 0 on EOF, loop=0 stops naturally.

   `lib` may be NULL if the caller already has a path it knows is good. */
static void musicPlay(MusicSystem *m, MusicLibrary *lib, const char *name,
                      float fadeSec, int loop)
{
    if (!name) return;

    const char *path = lib ? musicLibFind(lib, name) : NULL;
    if (!path) path = name;  /* fall through to raw path */

    /* If the same path is already playing (and not fading out), do nothing
       — repeated `musicPlay("ambient")` calls shouldn't restart the loop. */
    for (int i = 0; i < MUSIC_NUM_TRACKS; i++) {
        MusicTrack *t = &m->tracks[i];
        if (t->vorbis && !t->stopOnFadeOut && strcmp(t->path, path) == 0) {
            return;
        }
    }

    /* Fade out everything else that's currently audible. */
    for (int i = 0; i < MUSIC_NUM_TRACKS; i++) {
        MusicTrack *t = &m->tracks[i];
        if (!t->vorbis) continue;
        t->targetGain    = 0.0f;
        t->stopOnFadeOut = 1;
        t->fadeRate      = (fadeSec > 0.0f) ? (1.0f / fadeSec) : 1000.0f;
    }

    MusicTrack *t = musicPickSlot(m);
    musicTrackFree(t);  /* in case we evicted a still-open track */

    int err = 0;
    t->vorbis = stb_vorbis_open_filename(path, &err, NULL);
    if (!t->vorbis) {
        conLogf("musicPlay: stb_vorbis_open_filename('%s') failed, err=%d\n",
                path, err);
        return;
    }
    stb_vorbis_info info = stb_vorbis_get_info(t->vorbis);
    /* Coerce >2 channels down to stereo via stb_vorbis's own coercion
       rules; OpenAL only takes mono16 / stereo16 in fixed-function. */
    t->channels   = (info.channels >= 2) ? 2 : 1;
    t->sampleRate = (int)info.sample_rate;
    t->format     = (t->channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    t->loop       = loop;
    t->eos        = 0;

    if (fadeSec > 0.0f) {
        t->gain       = 0.0f;
        t->targetGain = 1.0f;
        t->fadeRate   = 1.0f / fadeSec;
    } else {
        t->gain       = 1.0f;
        t->targetGain = 1.0f;
        t->fadeRate   = 1000.0f;
    }
    t->stopOnFadeOut = 0;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    strncpy(t->path, path, sizeof(t->path) - 1);
    t->path[sizeof(t->path) - 1] = '\0';

    /* Prime the ring: decode + queue every buffer up front so the source
       has a full lookahead before alSourcePlay starts pulling. */
    int queued = 0;
    for (int i = 0; i < MUSIC_NUM_BUFFERS; i++) {
        if (!musicFillBuffer(t, t->buffers[i])) break;
        alSourceQueueBuffers(t->source, 1, &t->buffers[i]);
        queued++;
    }
    if (queued == 0) {
        conLogf("musicPlay: '%s' produced no audio (empty file?)\n", path);
        musicTrackFree(t);
        return;
    }

    alSourcef(t->source, AL_GAIN, t->gain * m->masterGain);
    alSourcePlay(t->source);
    conLogf("musicPlay: %s (%d ch, %d Hz, fade %.2fs, loop=%d)\n",
            path, t->channels, t->sampleRate, fadeSec, loop);
}

/* Fade everything to silence and free. fadeSec=0 cuts immediately. */
static void musicStop(MusicSystem *m, float fadeSec)
{
    for (int i = 0; i < MUSIC_NUM_TRACKS; i++) {
        MusicTrack *t = &m->tracks[i];
        if (!t->vorbis) continue;
        if (fadeSec <= 0.0f) {
            musicTrackFree(t);
        } else {
            t->targetGain    = 0.0f;
            t->fadeRate      = 1.0f / fadeSec;
            t->stopOnFadeOut = 1;
        }
    }
}

static void musicSetVolume(MusicSystem *m, float g)
{
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    m->masterGain = g;
}

/* Per-frame pump. Must be called every frame from the main loop, in BOTH
   menu and game modes — music streaming is independent of game state. */
static void musicUpdate(MusicSystem *m, float dt)
{
    for (int i = 0; i < MUSIC_NUM_TRACKS; i++) {
        MusicTrack *t = &m->tracks[i];
        if (!t->vorbis) continue;

        /* Step the gain envelope toward target. */
        float step = t->fadeRate * dt;
        if (t->gain < t->targetGain) {
            t->gain += step;
            if (t->gain > t->targetGain) t->gain = t->targetGain;
        } else if (t->gain > t->targetGain) {
            t->gain -= step;
            if (t->gain < t->targetGain) t->gain = t->targetGain;
        }
        alSourcef(t->source, AL_GAIN, t->gain * m->masterGain);

        /* Fade-out completion → release the slot. Doing this BEFORE the
           refill loop avoids wasting a decode on a track about to die. */
        if (t->stopOnFadeOut && t->gain <= 0.0001f) {
            musicTrackFree(t);
            continue;
        }

        /* Refill every buffer OpenAL has finished consuming. */
        ALint processed = 0;
        alGetSourcei(t->source, AL_BUFFERS_PROCESSED, &processed);
        while (processed > 0) {
            ALuint buf = 0;
            alSourceUnqueueBuffers(t->source, 1, &buf);
            processed--;
            if (t->eos) continue;  /* file is done; let queue drain */
            if (musicFillBuffer(t, buf)) {
                alSourceQueueBuffers(t->source, 1, &buf);
            }
        }

        /* End-of-stream: once OpenAL has played every queued buffer we
           tear down. AL_BUFFERS_QUEUED counts processed + pending, so 0
           means truly drained. */
        if (t->eos) {
            ALint queued = 0;
            alGetSourcei(t->source, AL_BUFFERS_QUEUED, &queued);
            if (queued == 0) {
                musicTrackFree(t);
                continue;
            }
        }

        /* Underrun recovery. If the disk stalled long enough that OpenAL
           ran out of audio and stopped the source, kick it back into play
           now that we've refilled the ring. */
        ALint state = AL_INITIAL;
        alGetSourcei(t->source, AL_SOURCE_STATE, &state);
        if (state == AL_STOPPED && !t->eos) {
            alSourcePlay(t->source);
        }
    }
}

#endif /* MUSIC_H */
