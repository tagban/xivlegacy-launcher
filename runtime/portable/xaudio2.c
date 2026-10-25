/* XAudio2 2.4 (the March 2009 SDK's) for ffxivgame.exe, on SDL3 audio.
 *
 * The game creates the engine through COM (CLSID_XAudio2 2.4, or its debug class), builds a graph of
 * source voices feeding submix voices feeding one mastering voice, and streams into the source
 * voices from its own threads, refilling from the voice callbacks (OnBufferEnd and the rest).
 * Those callbacks are guest code XAudio2 calls on its own thread, so the engine here is a guest
 * thread too: every 10 ms quantum it takes the guest lock, runs the engine and voice callbacks,
 * consumes and mixes the submitted buffers (PCM, IEEE float, MS-ADPCM) at 48 kHz, and queues the
 * result on an SDL audio stream, which paces it. Holding the guest lock for the pass is also what
 * keeps voice state consistent with the game's own calls, which all arrive under it.
 *
 * Interfaces: IXAudio2 is COM; the voices and callbacks are plain C++ interfaces (no IUnknown) with
 * __stdcall methods. The layouts are 2.4's: voice sends are XAUDIO2_SEND_DESCRIPTORs, voices have
 * Set/GetOutputFilterParameters, GetState takes one argument. Effects (reverb, volume meter) are
 * objects the game can hand to a voice; their processing is not done (the dry signal plays). */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "thunk.h"

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000Eu
#define E_INVALIDARG 0x80070057u
#define XAUDIO2_E_INVALID_CALL 0x88960001u

#define OUT_RATE 48000u
#define QUANTUM 480u /* frames per pass: 10 ms */
#define MAX_CH 8u
#define MAX_VOICES 1024u
#define MAX_QUEUE 64u /* XAUDIO2_MAX_QUEUED_BUFFERS */
#define MAX_SENDS 8u
#define MAX_EFFECTS 8u
#define MAX_CALLBACKS 8u
#define LOOP_INFINITE 255u
#define END_OF_STREAM 0x40u
#define VOICE_NOSRC 0x4u

#define TAG_PCM 1u
#define TAG_ADPCM 2u
#define TAG_FLOAT 3u
#define TAG_EXTENSIBLE 0xFFFEu

static const uint8_t CLSID_XAudio2[16] = { 0x78, 0x9E, 0x21, 0x03, 0xC3, 0x5B, 0xD1, 0x44, 0xB9, 0x2E, 0xF6, 0x3D, 0x89, 0xCC, 0x65, 0x26 };
static const uint8_t CLSID_XAudio2_Debug[16] = { 0x5C, 0x53, 0x56, 0x42, 0xA4, 0x1E, 0x4B, 0x4D, 0x8A, 0xD5, 0xF9, 0xDB, 0x76, 0x2E, 0xCA, 0x9E };
static const uint8_t CLSID_AudioReverb[16] = { 0x8B, 0x77, 0xB7, 0x8B, 0x5B, 0x64, 0x75, 0x44, 0x9A, 0x73, 0x1D, 0xE3, 0x17, 0x0B, 0xD3, 0xAF };
static const uint8_t CLSID_AudioVolumeMeter[16] = { 0x95, 0x8B, 0x33, 0xC7, 0xB8, 0x52, 0x42, 0x45, 0xAA, 0x79, 0x42, 0xEB, 0x01, 0x6C, 0x8C, 0x1C };

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* --- voices -------------------------------------------------------------------------------------- */

enum { V_SOURCE = 1, V_SUBMIX, V_MASTER };

typedef struct Buffer
{
    uint32_t flags, bytes, data, play_begin, play_len, loop_begin, loop_len, loop_count, ctx;
    uint32_t frames;     /* in the data */
    uint32_t loops_done;
    int started;         /* OnBufferStart sent */
} Buffer;

typedef struct Send
{
    uint32_t dest;          /* voice index + 1 */
    int has_matrix;
    float matrix[MAX_CH * MAX_CH]; /* [dst][src], as XAudio2 lays it out */
} Send;

typedef struct Voice
{
    int kind;
    uint32_t guest, gen;
    uint32_t flags, channels, rate, stage;
    /* source format */
    uint32_t tag, bits, align, spb;
    int16_t coef[32][2];
    uint32_t ncoef;
    uint32_t callback;
    float ratio, max_ratio;
    int started;
    Buffer queue[MAX_QUEUE];
    uint32_t nqueue;
    double pos;            /* frame position in queue[0] */
    uint64_t played;       /* samples consumed since creation */
    int at_end;            /* its last buffer was END_OF_STREAM and has played */
    uint32_t flushed[MAX_QUEUE], nflushed; /* contexts owed an OnBufferEnd */
    /* ADPCM: the last block decoded */
    uint32_t adpcm_data, adpcm_block;
    int16_t* adpcm;
    /* mixing */
    float volume, chvol[MAX_CH];
    Send sends[MAX_SENDS];
    uint32_t nsends;
    int default_send; /* no send list given: to the mastering voice */
    float mix[MAX_CH][QUANTUM]; /* submix and master input */
    /* effects */
    uint32_t effects[MAX_EFFECTS];
    uint8_t effect_on[MAX_EFFECTS];
    uint8_t* effect_params[MAX_EFFECTS];
    uint32_t effect_size[MAX_EFFECTS];
    uint32_t neffects;
    uint32_t filter[3];
} Voice;

static Voice* g_voices[MAX_VOICES];
static uint32_t g_master; /* voice index + 1 */
static uint32_t g_gen;

static uint32_t g_engine, g_engine_refs, g_vtbl_engine, g_vtbl_source, g_vtbl_submix, g_vtbl_master;
static uint32_t g_callbacks[MAX_CALLBACKS];
static volatile uint32_t g_running, g_thread_started;
static uint32_t g_starved; /* voice passes cut short by an empty queue (render_source) */
static SDL_AudioStream* g_stream;

static Voice* voice(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i < MAX_VOICES && g_voices[i] && g_voices[i]->guest == p ? g_voices[i] : NULL;
}

static uint32_t voice_index(const Voice* v) { return rd32(v->guest + 4); }

static uint32_t make_vtable(const char* iface, const char* const* methods, unsigned n)
{
    uint32_t t = gheap_alloc(4u * n, 1);
    char full[96];
    for (unsigned i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::%s", iface, methods[i]);
        wr32(t + 4u * i, thunk_for("xaudio2_4.dll", full));
    }
    return t;
}

static const char* const ENGINE_METHODS[] = { "QueryInterface", "AddRef", "Release", "GetDeviceCount",
    "GetDeviceDetails", "Initialize", "RegisterForCallbacks", "UnregisterForCallbacks", "CreateSourceVoice",
    "CreateSubmixVoice", "CreateMasteringVoice", "StartEngine", "StopEngine", "CommitChanges", "GetPerformanceData",
    "SetDebugConfiguration" };
#define VOICE_BASE "GetVoiceDetails", "SetOutputVoices", "SetEffectChain", "EnableEffect", "DisableEffect",          \
    "GetEffectState", "SetEffectParameters", "GetEffectParameters", "SetFilterParameters", "GetFilterParameters",   \
    "SetOutputFilterParameters", "GetOutputFilterParameters", "SetVolume", "GetVolume", "SetChannelVolumes",        \
    "GetChannelVolumes", "SetOutputMatrix", "GetOutputMatrix", "DestroyVoice"
static const char* const SOURCE_METHODS[] = { VOICE_BASE, "Start", "Stop", "SubmitSourceBuffer", "FlushSourceBuffers",
    "Discontinuity", "ExitLoop", "GetState", "SetFrequencyRatio", "GetFrequencyRatio", "SetSourceSampleRate" };
static const char* const SUBMIX_METHODS[] = { VOICE_BASE };
#define N(a) (sizeof a / sizeof a[0])

static void vtables(void)
{
    if (g_vtbl_engine)
        return;
    g_vtbl_engine = make_vtable("IXAudio2", ENGINE_METHODS, N(ENGINE_METHODS));
    g_vtbl_source = make_vtable("IXAudio2SourceVoice", SOURCE_METHODS, N(SOURCE_METHODS));
    g_vtbl_submix = make_vtable("IXAudio2SubmixVoice", SUBMIX_METHODS, N(SUBMIX_METHODS));
    g_vtbl_master = make_vtable("IXAudio2MasteringVoice", SUBMIX_METHODS, N(SUBMIX_METHODS));
}

static Voice* voice_new(int kind)
{
    for (uint32_t i = 0; i < MAX_VOICES; ++i)
        if (!g_voices[i])
        {
            Voice* v = (Voice*)calloc(1, sizeof *v);
            v->kind = kind;
            v->gen = ++g_gen;
            v->guest = gheap_alloc(8, 1);
            wr32(v->guest, kind == V_SOURCE ? g_vtbl_source : kind == V_SUBMIX ? g_vtbl_submix : g_vtbl_master);
            wr32(v->guest + 4, i);
            v->volume = 1.0f;
            for (unsigned c = 0; c < MAX_CH; ++c)
                v->chvol[c] = 1.0f;
            v->ratio = 1.0f;
            v->adpcm_block = ~0u;
            g_voices[i] = v;
            return v;
        }
    return NULL;
}

static void voice_free(Voice* v)
{
    uint32_t i = voice_index(v);
    for (uint32_t k = 0; k < v->neffects; ++k)
        free(v->effect_params[k]);
    free(v->adpcm);
    gheap_free(v->guest);
    if (g_master == i + 1)
        g_master = 0;
    /* sends that pointed here go nowhere now */
    for (uint32_t j = 0; j < MAX_VOICES; ++j)
        if (g_voices[j])
            for (uint32_t s = 0; s < g_voices[j]->nsends; ++s)
                if (g_voices[j]->sends[s].dest == i + 1)
                    g_voices[j]->sends[s].dest = 0;
    g_voices[i] = NULL;
    free(v);
}

/* XAUDIO2_VOICE_SENDS {count, XAUDIO2_SEND_DESCRIPTOR* {flags, voice}}; NULL: the mastering voice. */
static void read_sends(Voice* v, uint32_t p)
{
    v->nsends = 0;
    v->default_send = !p;
    if (!p)
        return;
    uint32_t n = rd32(p), a = rd32(p + 4);
    for (uint32_t i = 0; i < n && v->nsends < MAX_SENDS && a; ++i)
    {
        Voice* d = voice(rd32(a + 8u * i + 4));
        if (d && d != v)
        {
            memset(&v->sends[v->nsends], 0, sizeof v->sends[0]);
            v->sends[v->nsends++].dest = voice_index(d) + 1;
        }
    }
}

/* XAUDIO2_EFFECT_CHAIN {count, XAUDIO2_EFFECT_DESCRIPTOR* {IUnknown*, InitialState, OutputChannels}} */
static void read_effects(Voice* v, uint32_t p)
{
    for (uint32_t k = 0; k < v->neffects; ++k)
        free(v->effect_params[k]), v->effect_params[k] = NULL, v->effect_size[k] = 0;
    v->neffects = 0;
    if (!p)
        return;
    uint32_t n = rd32(p), a = rd32(p + 4);
    for (uint32_t i = 0; i < n && i < MAX_EFFECTS && a; ++i)
    {
        v->effects[i] = rd32(a + 12u * i);
        v->effect_on[i] = rd32(a + 12u * i + 4) != 0;
        v->neffects++;
    }
}

static Voice* destination(const Send* s) { return s->dest ? g_voices[s->dest - 1] : NULL; }

/* --- the format and the samples --------------------------------------------------------------------- */

static int read_format(Voice* v, uint32_t w)
{
    uint32_t tag = rd16(w);
    v->channels = rd16(w + 2);
    v->rate = rd32(w + 4);
    v->align = rd16(w + 12);
    v->bits = rd16(w + 14);
    if (tag == TAG_EXTENSIBLE && rd16(w + 16) >= 22)
        tag = rd16(w + 24); /* the SubFormat GUID's first word is the format tag */
    v->tag = tag;
    if (tag == TAG_ADPCM)
    {
        v->spb = rd16(w + 18);
        v->ncoef = rd16(w + 20);
        if (v->ncoef > 32)
            v->ncoef = 32;
        for (uint32_t i = 0; i < v->ncoef; ++i)
            v->coef[i][0] = (int16_t)rd16(w + 22 + 4 * i), v->coef[i][1] = (int16_t)rd16(w + 24 + 4 * i);
        if (!v->spb || !v->align)
            return 0;
        v->adpcm = (int16_t*)malloc(sizeof(int16_t) * v->spb * v->channels);
    }
    static int logged[4];
    unsigned li = tag == TAG_PCM ? 0 : tag == TAG_FLOAT ? 1 : tag == TAG_ADPCM ? 2 : 3;
    if (!logged[li]++)
        rt_log("[xaudio2] source format: tag %u, %u ch, %u Hz, %u bits, align %u\n", tag, v->channels, v->rate, v->bits,
            v->align);
    if (!v->channels || v->channels > MAX_CH || !v->rate)
        return 0;
    return tag == TAG_PCM ? (v->bits == 8 || v->bits == 16 || v->bits == 24 || v->bits == 32)
         : tag == TAG_FLOAT ? v->bits == 32
         : tag == TAG_ADPCM;
}

static uint32_t buffer_frames(const Voice* v, uint32_t bytes)
{
    if (v->tag == TAG_ADPCM)
        return bytes / v->align * v->spb;
    return v->align ? bytes / v->align : 0;
}

static void adpcm_decode(Voice* v, uint32_t data, uint32_t block)
{
    static const int adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
    if (v->adpcm_data == data && v->adpcm_block == block)
        return;
    v->adpcm_data = data, v->adpcm_block = block;
    const uint8_t* p = GUEST_PTR(data + block * v->align);
    uint32_t ch = v->channels, n = v->spb;
    int c1[2], c2[2], delta[2], s1[2], s2[2];
    for (uint32_t c = 0; c < ch && c < 2; ++c)
    {
        uint32_t pi = p[c] < v->ncoef ? p[c] : 0;
        c1[c] = v->coef[pi][0], c2[c] = v->coef[pi][1];
        delta[c] = (int16_t)(p[ch + 2 * c] | p[ch + 2 * c + 1] << 8);
        s1[c] = (int16_t)(p[3 * ch + 2 * c] | p[3 * ch + 2 * c + 1] << 8);
        s2[c] = (int16_t)(p[5 * ch + 2 * c] | p[5 * ch + 2 * c + 1] << 8);
    }
    int16_t* out = v->adpcm;
    for (uint32_t c = 0; c < ch; ++c)
        out[c] = (int16_t)s2[c], out[ch + c] = (int16_t)s1[c];
    const uint8_t* q = p + 7 * ch;
    uint32_t total = n * ch, at = 2 * ch;
    for (uint32_t i = 0; at < total; ++i)
    {
        uint8_t b = q[i];
        for (int half = 0; half < 2 && at < total; ++half)
        {
            int nib = half ? b & 0xF : b >> 4;
            int c = (int)(at % ch);
            int sn = nib >= 8 ? nib - 16 : nib;
            int pred = ((s1[c] * c1[c]) + (s2[c] * c2[c])) / 256 + sn * delta[c];
            pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
            s2[c] = s1[c], s1[c] = pred;
            delta[c] = adapt[nib] * delta[c] / 256;
            if (delta[c] < 16)
                delta[c] = 16;
            out[at++] = (int16_t)pred;
        }
    }
}

static float sample_at(Voice* v, const Buffer* b, uint32_t frame, uint32_t c)
{
    if (frame >= b->frames)
        return 0.0f;
    if (v->tag == TAG_ADPCM)
    {
        adpcm_decode(v, b->data, frame / v->spb);
        return v->adpcm[(frame % v->spb) * v->channels + c] / 32768.0f;
    }
    uint32_t a = b->data + frame * v->align + c * (v->bits / 8);
    switch (v->tag == TAG_FLOAT ? 0 : v->bits)
    {
    case 0: return u2f(rd32(a));
    case 8: return ((int)*GUEST_PTR(a) - 128) / 128.0f;
    case 16: return (int16_t)rd16(a) / 32768.0f;
    case 24: return (float)((int32_t)(rd32(a) << 8) >> 8) / 8388608.0f;
    default: return (int32_t)rd32(a) / 2147483648.0f;
    }
}

/* --- callbacks into the game ---------------------------------------------------------------------- */

static void callback(uint32_t cb, unsigned method, unsigned nargs, uint32_t a1, uint32_t a2)
{
    if (!cb)
        return;
    uint32_t fn = rd32(rd32(cb) + 4u * method);
    if (!fn)
        return;
    uint32_t args[3] = { cb, a1, a2 };
    guest_call(fn, nargs, args);
}

enum { CB_PASS_START, CB_PASS_END, CB_STREAM_END, CB_BUFFER_START, CB_BUFFER_END, CB_LOOP_END, CB_ERROR };

/* still the same voice after a callback (which may have destroyed it) */
static int alive(uint32_t i, uint32_t gen) { return g_voices[i] && g_voices[i]->gen == gen; }

/* --- the pass -------------------------------------------------------------------------------------- */

static uint32_t frames_queued(const Voice* v)
{
    uint64_t n = 0;
    for (uint32_t i = 0; i < v->nqueue; ++i)
    {
        const Buffer* b = &v->queue[i];
        uint32_t end = b->play_len ? b->play_begin + b->play_len : b->frames;
        n += end > b->play_begin ? end - b->play_begin : 0;
    }
    if (v->nqueue)
        n = n > (uint64_t)v->pos - v->queue[0].play_begin ? n - ((uint64_t)v->pos - v->queue[0].play_begin) : 0;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static uint32_t bytes_for(const Voice* v, uint32_t frames)
{
    if (v->tag == TAG_ADPCM)
        return (frames + v->spb - 1) / v->spb * v->align;
    return frames * v->align;
}

/* The current buffer's end: its loop end while loops remain, else its play end. */
static uint32_t buffer_end(const Buffer* b, int* looping)
{
    uint32_t play_end = b->play_len ? b->play_begin + b->play_len : b->frames;
    if (play_end > b->frames)
        play_end = b->frames;
    *looping = b->loop_count && (b->loop_count == LOOP_INFINITE || b->loops_done < b->loop_count);
    if (*looping)
    {
        uint32_t le = b->loop_len ? b->loop_begin + b->loop_len : play_end;
        return le > play_end ? play_end : le;
    }
    return play_end;
}

/* Renders one quantum of a source voice into out[channel][QUANTUM]; runs its buffer callbacks. */
static void render_source(uint32_t vi, float out[MAX_CH][QUANTUM])
{
    Voice* v = g_voices[vi];
    uint32_t gen = v->gen, cb = v->callback;
    double step = (double)v->rate * v->ratio / OUT_RATE;
    for (uint32_t c = 0; c < v->channels; ++c)
        memset(out[c], 0, sizeof out[c]);

    /* contexts flushed since the last pass */
    while (v->nflushed)
    {
        uint32_t ctx = v->flushed[0];
        memmove(v->flushed, v->flushed + 1, sizeof v->flushed[0] * --v->nflushed);
        callback(cb, CB_BUFFER_END, 2, ctx, 0);
        if (!alive(vi, gen))
            return;
    }
    if (!v->started)
        return;

    uint32_t need = (uint32_t)(QUANTUM * step) + 2, have = frames_queued(v);
    callback(cb, CB_PASS_START, 2, have >= need ? 0 : bytes_for(v, need - have), 0);
    if (!alive(vi, gen))
        return;

    uint32_t f = 0;
    while (f < QUANTUM)
    {
        if (!v->nqueue || !v->started)
            break;
        Buffer* b = &v->queue[0];
        if (!b->started)
        {
            b->started = 1;
            if (v->pos < b->play_begin)
                v->pos = b->play_begin;
            callback(cb, CB_BUFFER_START, 2, b->ctx, 0);
            if (!alive(vi, gen))
                return;
            continue; /* the callback may have changed the queue */
        }
        int looping;
        uint32_t end = buffer_end(b, &looping);
        if (v->pos >= end)
        {
            if (looping)
            {
                uint32_t lb = b->loop_begin < end ? b->loop_begin : 0;
                v->pos -= (double)(end - lb);
                if (v->pos < lb)
                    v->pos = lb;
                b->loops_done++;
                callback(cb, CB_LOOP_END, 2, b->ctx, 0);
                if (!alive(vi, gen))
                    return;
                continue;
            }
            double over = v->pos - end;
            uint32_t ctx = b->ctx, eos = b->flags & END_OF_STREAM;
            memmove(&v->queue[0], &v->queue[1], sizeof v->queue[0] * (v->nqueue - 1));
            v->nqueue--;
            v->pos = (v->nqueue ? v->queue[0].play_begin : 0) + over;
            callback(cb, CB_BUFFER_END, 2, ctx, 0);
            if (!alive(vi, gen))
                return;
            v->at_end = eos != 0;
            if (eos)
            {
                callback(cb, CB_STREAM_END, 1, 0, 0);
                if (!alive(vi, gen))
                    return;
            }
            continue;
        }
        /* frames until this buffer's end */
        for (; f < QUANTUM && v->pos < end; ++f)
        {
            uint32_t i0 = (uint32_t)v->pos;
            float t = (float)(v->pos - i0);
            uint32_t i1 = i0 + 1 < end ? i0 + 1 : i0;
            for (uint32_t c = 0; c < v->channels; ++c)
            {
                float a = sample_at(v, b, i0, c), bb = sample_at(v, b, i1, c);
                out[c][f] = a + (bb - a) * t;
            }
            double before = v->pos;
            v->pos += step;
            v->played += (uint64_t)v->pos - (uint64_t)before;
        }
    }
    /* a stream that ran out of buffers mid-pass, not at its end: the game fed it late */
    if (f < QUANTUM && v->started && v->played && !v->at_end)
        g_starved++;
    callback(cb, CB_PASS_END, 1, 0, 0);
}

/* Mixes a voice's output (channels x QUANTUM, at its volume) into each of its destinations. */
static void send_out(Voice* v, float in[MAX_CH][QUANTUM])
{
    Send fallback = { g_master, 0, { 0 } };
    uint32_t n = v->default_send ? (g_master ? 1 : 0) : v->nsends;
    for (uint32_t s = 0; s < n; ++s)
    {
        Send* sd = v->default_send ? &fallback : &v->sends[s];
        Voice* d = destination(sd);
        if (!d || d == v)
            continue;
        uint32_t sc = v->channels, dc = d->channels;
        float m[MAX_CH * MAX_CH];
        if (sd->has_matrix)
            memcpy(m, sd->matrix, sizeof m);
        else
        {
            memset(m, 0, sizeof m);
            if (sc == 1)
                for (uint32_t o = 0; o < dc && o < 2; ++o)
                    m[o * sc] = dc == 1 ? 1.0f : 0.7071f;
            else
                for (uint32_t o = 0; o < dc && o < sc; ++o)
                    m[o * sc + o] = 1.0f;
        }
        for (uint32_t o = 0; o < dc; ++o)
            for (uint32_t c = 0; c < sc; ++c)
            {
                float k = m[o * sc + c] * v->volume * v->chvol[c];
                if (k == 0.0f)
                    continue;
                for (uint32_t f = 0; f < QUANTUM; ++f)
                    d->mix[o][f] += in[c][f] * k;
            }
    }
}

/* One engine pass: QUANTUM stereo frames into `out` (interleaved float). */
static void pass(float* out)
{
    memset(out, 0, sizeof(float) * 2 * QUANTUM);
    for (uint32_t k = 0; k < MAX_CALLBACKS; ++k)
        callback(g_callbacks[k], 0, 1, 0, 0); /* OnProcessingPassStart */
    for (uint32_t i = 0; i < MAX_VOICES; ++i)
        if (g_voices[i] && g_voices[i]->kind != V_SOURCE)
            for (uint32_t c = 0; c < MAX_CH; ++c)
                memset(g_voices[i]->mix[c], 0, sizeof g_voices[i]->mix[c]);

    static float buf[MAX_CH][QUANTUM];
    for (uint32_t i = 0; i < MAX_VOICES; ++i)
        if (g_voices[i] && g_voices[i]->kind == V_SOURCE)
        {
            uint32_t gen = g_voices[i]->gen;
            render_source(i, buf);
            if (alive(i, gen))
                send_out(g_voices[i], buf);
        }
    /* submix voices, lowest processing stage first */
    for (uint32_t stage = 0; stage < 16; ++stage)
        for (uint32_t i = 0; i < MAX_VOICES; ++i)
            if (g_voices[i] && g_voices[i]->kind == V_SUBMIX && (g_voices[i]->stage == stage || (stage == 15 && g_voices[i]->stage > 15)))
                send_out(g_voices[i], g_voices[i]->mix);

    Voice* m = g_master ? g_voices[g_master - 1] : NULL;
    if (m)
    {
        /* to stereo: FL FR FC LFE BL BR SL SR */
        static const float L[MAX_CH] = { 1, 0, 0.7071f, 0, 0.7071f, 0, 0.7071f, 0 };
        static const float R[MAX_CH] = { 0, 1, 0.7071f, 0, 0, 0.7071f, 0, 0.7071f };
        for (uint32_t f = 0; f < QUANTUM; ++f)
        {
            float l = 0, r = 0;
            if (m->channels == 1)
                l = r = m->mix[0][f];
            else
                for (uint32_t c = 0; c < m->channels; ++c)
                    l += m->mix[c][f] * L[c] * m->chvol[c], r += m->mix[c][f] * R[c] * m->chvol[c];
            l *= m->volume, r *= m->volume;
            out[2 * f] = l > 1 ? 1 : l < -1 ? -1 : l;
            out[2 * f + 1] = r > 1 ? 1 : r < -1 ? -1 : r;
        }
    }
    for (uint32_t k = 0; k < MAX_CALLBACKS; ++k)
        callback(g_callbacks[k], 1, 1, 0, 0); /* OnProcessingPassEnd */
}

static void engine_thread(void* arg)
{
    (void)arg;
    gt_lock();
    gt_self();
    gt_unlock();
    /* The device pulls in 10 ms steps rather than its own default (larger on some Windows drivers,
     * where it emptied the queue between our batches). */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
    if (!getenv("XIVLEGACY_NOAUDIO") && SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, OUT_RATE };
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (g_stream)
            SDL_ResumeAudioStreamDevice(g_stream);
    }
    if (!g_stream)
        rt_log("[xaudio2] no audio device: the engine runs silent, on the clock\n");
    static float out[2 * QUANTUM];
    uint64_t next = rt_monotonic_ns();
    const int pass_bytes = (int)sizeof out;
    for (;;)
    {
        /* Passes go in batches while the device has `ahead` passes queued (8 at first: 50-80 ms),
         * taking the guest lock when it comes free (the render thread's frame Sleeps): asking the
         * holder to yield at every pass, 100 times a second, cost the world a third of its frame
         * rate. Below half of it it asks. When the device runs dry anyway (a device that pulls
         * large chunks, a slow frame), `ahead` grows by a pass, up to 300 ms. */
        static int ahead = 8;
        int urgent = 0, want = 1;
        if (g_stream)
        {
            int q;
            while ((q = SDL_GetAudioStreamQueued(g_stream)) > (ahead - 3) * pass_bytes)
                plat_sleep_ms(2);
            if (q == 0 && ahead < 30)
            {
                ahead++;
                if (ahead % 4 == 0)
                    rt_log("[xaudio2] the device ran dry: keeping %d ms queued now\n", ahead * 10);
            }
            urgent = q <= ahead / 2 * pass_bytes;
            want = ahead - q / pass_bytes;
        }
        else
        {
            uint64_t now = rt_monotonic_ns();
            if (now < next)
                plat_sleep_ms((uint32_t)((next - now) / 1000000u) + 1);
            next += 10000000u;
        }
        if (urgent)
            gt_lock_urgent();
        else
            gt_lock();
        for (int n = 0; n < want; ++n)
        {
            if (g_running)
                pass(out);
            else
                memset(out, 0, sizeof out);
            if (g_stream)
            {
                /* the device ran dry: the pass came late (counted, reported now and then) */
                static uint32_t passes, dry, reported;
                if (++passes > 100 && SDL_GetAudioStreamQueued(g_stream) == 0)
                    dry++;
                static uint32_t starved_reported;
                if ((dry != reported || g_starved != starved_reported || passes == 3000) && passes % 1000 == 0)
                    rt_log("[xaudio2] %u of %u passes late (the device ran dry), %u voice passes short of data\n", dry,
                        passes, g_starved),
                        reported = dry, starved_reported = g_starved;
                SDL_PutAudioStreamData(g_stream, out, pass_bytes);
            }
        }
        gt_unlock();
    }
}

/* --- IXAudio2 ------------------------------------------------------------------------------------- */

static void sh_X_QueryInterface(Guest* g)
{
    g_engine_refs++;
    wr32(ARG(2), ARG(0));
    RET(S_OK, 3);
}

static void sh_X_AddRef(Guest* g) { RET(++g_engine_refs, 1); }

static void sh_X_Release(Guest* g)
{
    if (g_engine_refs)
        g_engine_refs--;
    if (!g_engine_refs)
        g_running = 0;
    RET(g_engine_refs, 1);
}

static void sh_X_GetDeviceCount(Guest* g) { wr32(ARG(1), 1); RET(S_OK, 2); }

static void wstr(uint32_t p, const char* s)
{
    for (; *s; ++s, p += 2)
        wr16(p, (uint16_t)*s);
    wr16(p, 0);
}

/* GetDeviceDetails(index, XAUDIO2_DEVICE_DETAILS*): stereo float at 48 kHz, the default device */
static void sh_X_GetDeviceDetails(Guest* g)
{
    uint32_t p = ARG(2);
    if (ARG(1) != 0)
        RET(E_INVALIDARG, 3);
    memset(GUEST_PTR(p), 0, 1068);
    wstr(p, "{XIVLEGACY-AUDIO}");
    wstr(p + 512, "Default Audio Device");
    wr32(p + 1024, 0xF); /* GlobalDefaultDevice */
    uint32_t w = p + 1028;
    wr16(w, TAG_EXTENSIBLE);
    wr16(w + 2, 2);
    wr32(w + 4, OUT_RATE);
    wr32(w + 8, OUT_RATE * 8);
    wr16(w + 12, 8);
    wr16(w + 14, 32);
    wr16(w + 16, 22);
    wr16(w + 18, 32);
    wr32(w + 20, 3); /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
    static const uint8_t KSDATAFORMAT_SUBTYPE_IEEE_FLOAT[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
    memcpy(GUEST_PTR(w + 24), KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16);
    RET(S_OK, 3);
}

static void start_thread(void)
{
    g_running = 1;
    if (!g_thread_started)
    {
        g_thread_started = 1;
        plat_thread_start(engine_thread, NULL);
    }
}

static void sh_X_Initialize(Guest* g)
{
    rt_log("[xaudio2] Initialize(flags %#x, processor %#x)\n", ARG(1), ARG(2));
    start_thread();
    RET(S_OK, 3);
}

static void sh_X_RegisterForCallbacks(Guest* g)
{
    for (uint32_t k = 0; k < MAX_CALLBACKS; ++k)
        if (!g_callbacks[k] || g_callbacks[k] == ARG(1))
        {
            g_callbacks[k] = ARG(1);
            RET(S_OK, 2);
        }
    RET(E_OUTOFMEMORY, 2);
}

static void sh_X_UnregisterForCallbacks(Guest* g)
{
    for (uint32_t k = 0; k < MAX_CALLBACKS; ++k)
        if (g_callbacks[k] == ARG(1))
            g_callbacks[k] = 0;
    RET(0, 2);
}

/* CreateSourceVoice(IXAudio2SourceVoice**, const WAVEFORMATEX*, flags, max_ratio, callback, sends, effects) */
static void sh_X_CreateSourceVoice(Guest* g)
{
    uint32_t out = ARG(1);
    wr32(out, 0);
    Voice* v = voice_new(V_SOURCE);
    if (!v)
        RET(E_OUTOFMEMORY, 8);
    if (!read_format(v, ARG(2)))
    {
        rt_log("[xaudio2] CreateSourceVoice: format tag %u not supported\n", rd16(ARG(2)));
        voice_free(v);
        RET(E_INVALIDARG, 8);
    }
    v->flags = ARG(3);
    v->max_ratio = u2f(ARG(4));
    v->callback = ARG(5);
    read_sends(v, ARG(6));
    read_effects(v, ARG(7));
    wr32(out, v->guest);
    RET(S_OK, 8);
}

/* CreateSubmixVoice(IXAudio2SubmixVoice**, channels, rate, flags, stage, sends, effects) */
static void sh_X_CreateSubmixVoice(Guest* g)
{
    uint32_t out = ARG(1);
    wr32(out, 0);
    if (!ARG(2) || ARG(2) > MAX_CH)
        RET(E_INVALIDARG, 8);
    Voice* v = voice_new(V_SUBMIX);
    if (!v)
        RET(E_OUTOFMEMORY, 8);
    v->channels = ARG(2), v->rate = ARG(3), v->flags = ARG(4), v->stage = ARG(5);
    read_sends(v, ARG(6));
    read_effects(v, ARG(7));
    wr32(out, v->guest);
    RET(S_OK, 8);
}

/* CreateMasteringVoice(IXAudio2MasteringVoice**, channels, rate, flags, device, effects) */
static void sh_X_CreateMasteringVoice(Guest* g)
{
    uint32_t out = ARG(1);
    wr32(out, 0);
    if (g_master)
        RET(XAUDIO2_E_INVALID_CALL, 7);
    Voice* v = voice_new(V_MASTER);
    if (!v)
        RET(E_OUTOFMEMORY, 7);
    v->channels = ARG(2) && ARG(2) <= MAX_CH ? ARG(2) : 2;
    v->rate = ARG(3) ? ARG(3) : OUT_RATE;
    v->flags = ARG(4);
    read_effects(v, ARG(6));
    g_master = voice_index(v) + 1;
    rt_log("[xaudio2] mastering voice: %u ch, %u Hz\n", v->channels, v->rate);
    wr32(out, v->guest);
    RET(S_OK, 7);
}

static void sh_X_StartEngine(Guest* g) { start_thread(); RET(S_OK, 1); }
static void sh_X_StopEngine(Guest* g) { g_running = 0; RET(0, 1); }
static void sh_X_CommitChanges(Guest* g) { RET(S_OK, 2); }

static void sh_X_GetPerformanceData(Guest* g)
{
    uint32_t p = ARG(1), active = 0, total = 0;
    for (uint32_t i = 0; i < MAX_VOICES; ++i)
        if (g_voices[i] && g_voices[i]->kind == V_SOURCE)
            total++, active += g_voices[i]->started;
    memset(GUEST_PTR(p), 0, 56);
    wr32(p + 28, 3 * QUANTUM); /* CurrentLatencyInSamples */
    wr32(p + 36, active);
    wr32(p + 40, total);
    RET(0, 2);
}

static void sh_X_SetDebugConfiguration(Guest* g) { RET(0, 3); }

/* --- IXAudio2Voice ------------------------------------------------------------------------------- */

#define THIS_VOICE(n)                                                                                          \
    Voice* v = voice(ARG(0));                                                                                  \
    if (!v)                                                                                                    \
        RET(XAUDIO2_E_INVALID_CALL, n)

/* XAUDIO2_VOICE_DETAILS {CreationFlags, InputChannels, InputSampleRate} */
static void sh_V_GetVoiceDetails(Guest* g)
{
    THIS_VOICE(2);
    wr32(ARG(1), v->flags);
    wr32(ARG(1) + 4, v->channels);
    wr32(ARG(1) + 8, v->rate);
    RET(0, 2);
}

static void sh_V_SetOutputVoices(Guest* g) { THIS_VOICE(2); read_sends(v, ARG(1)); RET(S_OK, 2); }
static void sh_V_SetEffectChain(Guest* g) { THIS_VOICE(2); read_effects(v, ARG(1)); RET(S_OK, 2); }

static void sh_V_EnableEffect(Guest* g)
{
    THIS_VOICE(3);
    if (ARG(1) >= v->neffects)
        RET(E_INVALIDARG, 3);
    v->effect_on[ARG(1)] = 1;
    RET(S_OK, 3);
}

static void sh_V_DisableEffect(Guest* g)
{
    THIS_VOICE(3);
    if (ARG(1) >= v->neffects)
        RET(E_INVALIDARG, 3);
    v->effect_on[ARG(1)] = 0;
    RET(S_OK, 3);
}

static void sh_V_GetEffectState(Guest* g)
{
    THIS_VOICE(3);
    wr32(ARG(2), ARG(1) < v->neffects ? v->effect_on[ARG(1)] : 0);
    RET(0, 3);
}

/* SetEffectParameters(index, const void*, size, opset): kept, for GetEffectParameters */
static void sh_V_SetEffectParameters(Guest* g)
{
    THIS_VOICE(5);
    uint32_t i = ARG(1), n = ARG(3);
    if (i >= v->neffects || n > 4096)
        RET(E_INVALIDARG, 5);
    free(v->effect_params[i]);
    v->effect_params[i] = (uint8_t*)malloc(n ? n : 1);
    memcpy(v->effect_params[i], GUEST_PTR(ARG(2)), n);
    v->effect_size[i] = n;
    RET(S_OK, 5);
}

static void sh_V_GetEffectParameters(Guest* g)
{
    THIS_VOICE(4);
    uint32_t i = ARG(1), n = ARG(3);
    if (i >= v->neffects)
        RET(E_INVALIDARG, 4);
    memset(GUEST_PTR(ARG(2)), 0, n);
    if (v->effect_params[i])
        memcpy(GUEST_PTR(ARG(2)), v->effect_params[i], n < v->effect_size[i] ? n : v->effect_size[i]);
    RET(S_OK, 4);
}

/* XAUDIO2_FILTER_PARAMETERS {Type, Frequency, OneOverQ}: kept, not applied */
static void sh_V_SetFilterParameters(Guest* g)
{
    THIS_VOICE(3);
    memcpy(v->filter, GUEST_PTR(ARG(1)), 12);
    RET(S_OK, 3);
}

static void sh_V_GetFilterParameters(Guest* g)
{
    THIS_VOICE(2);
    if (!v->filter[1] && !v->filter[2])
    {
        v->filter[0] = 0, v->filter[1] = f2u(1.0f), v->filter[2] = f2u(1.0f); /* the defaults: an open filter */
    }
    memcpy(GUEST_PTR(ARG(1)), v->filter, 12);
    RET(0, 2);
}

static void sh_V_SetOutputFilterParameters(Guest* g) { RET(S_OK, 4); }

static void sh_V_GetOutputFilterParameters(Guest* g)
{
    wr32(ARG(2), 0);
    wr32(ARG(2) + 4, f2u(1.0f));
    wr32(ARG(2) + 8, f2u(1.0f));
    RET(0, 3);
}

static void sh_V_SetVolume(Guest* g) { THIS_VOICE(3); v->volume = u2f(ARG(1)); RET(S_OK, 3); }
static void sh_V_GetVolume(Guest* g) { THIS_VOICE(2); wr32(ARG(1), f2u(v->volume)); RET(0, 2); }

static void sh_V_SetChannelVolumes(Guest* g)
{
    THIS_VOICE(4);
    for (uint32_t c = 0; c < ARG(1) && c < MAX_CH; ++c)
        v->chvol[c] = u2f(rd32(ARG(2) + 4 * c));
    RET(S_OK, 4);
}

static void sh_V_GetChannelVolumes(Guest* g)
{
    THIS_VOICE(3);
    for (uint32_t c = 0; c < ARG(1) && c < MAX_CH; ++c)
        wr32(ARG(2) + 4 * c, f2u(v->chvol[c]));
    RET(0, 3);
}

static Send* send_to(Voice* v, uint32_t dest)
{
    Voice* d = dest ? voice(dest) : NULL;
    if (v->default_send && (!dest || (d && voice_index(d) + 1 == g_master)))
    {
        /* the implicit send to the mastering voice becomes an explicit one */
        v->default_send = 0;
        memset(&v->sends[0], 0, sizeof v->sends[0]);
        v->sends[0].dest = g_master;
        v->nsends = g_master ? 1 : 0;
    }
    for (uint32_t s = 0; s < v->nsends; ++s)
        if (!d || v->sends[s].dest == voice_index(d) + 1)
            return &v->sends[s];
    return NULL;
}

/* SetOutputMatrix(dest, src_channels, dst_channels, const float*, opset) */
static void sh_V_SetOutputMatrix(Guest* g)
{
    THIS_VOICE(6);
    Send* s = send_to(v, ARG(1));
    uint32_t sc = ARG(2), dc = ARG(3);
    if (!s || sc > MAX_CH || dc > MAX_CH)
        RET(E_INVALIDARG, 6);
    memset(s->matrix, 0, sizeof s->matrix);
    for (uint32_t o = 0; o < dc; ++o)
        for (uint32_t c = 0; c < sc; ++c)
            s->matrix[o * v->channels + c] = c < v->channels ? u2f(rd32(ARG(4) + 4 * (o * sc + c))) : 0;
    s->has_matrix = 1;
    RET(S_OK, 6);
}

static void sh_V_GetOutputMatrix(Guest* g)
{
    THIS_VOICE(5);
    Send* s = send_to(v, ARG(1));
    uint32_t sc = ARG(2), dc = ARG(3);
    for (uint32_t o = 0; o < dc && o < MAX_CH; ++o)
        for (uint32_t c = 0; c < sc && c < MAX_CH; ++c)
            wr32(ARG(4) + 4 * (o * sc + c), f2u(s && s->has_matrix ? s->matrix[o * v->channels + c] : (o == c ? 1.0f : 0.0f)));
    RET(0, 5);
}

static void sh_V_DestroyVoice(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v)
        voice_free(v);
    RET(0, 1);
}

/* --- IXAudio2SourceVoice ------------------------------------------------------------------------- */

static void sh_S_Start(Guest* g) { THIS_VOICE(3); v->started = 1; RET(S_OK, 3); }

/* Stop(flags, opset): XAUDIO2_PLAY_TAILS (0x20) makes no difference without effects */
static void sh_S_Stop(Guest* g) { THIS_VOICE(3); v->started = 0; RET(S_OK, 3); }

/* SubmitSourceBuffer(const XAUDIO2_BUFFER*, const XAUDIO2_BUFFER_WMA*) */
static void sh_S_SubmitSourceBuffer(Guest* g)
{
    THIS_VOICE(3);
    uint32_t p = ARG(1);
    if (!p || v->nqueue >= MAX_QUEUE)
        RET(XAUDIO2_E_INVALID_CALL, 3);
    Buffer* b = &v->queue[v->nqueue];
    memset(b, 0, sizeof *b);
    b->flags = rd32(p), b->bytes = rd32(p + 4), b->data = rd32(p + 8);
    b->play_begin = rd32(p + 12), b->play_len = rd32(p + 16);
    b->loop_begin = rd32(p + 20), b->loop_len = rd32(p + 24), b->loop_count = rd32(p + 28);
    b->ctx = rd32(p + 32);
    b->frames = buffer_frames(v, b->bytes);
    if (!b->data || !b->frames)
        RET(E_INVALIDARG, 3);
    if (v->nqueue == 0)
        v->pos = b->play_begin;
    v->nqueue++;
    v->at_end = 0;
    RET(S_OK, 3);
}

/* Pending buffers go; their OnBufferEnd comes on the engine's next pass, as in XAudio2. A started
 * voice keeps the buffer it is playing. */
static void sh_S_FlushSourceBuffers(Guest* g)
{
    THIS_VOICE(1);
    uint32_t keep = v->started && v->nqueue ? 1 : 0;
    for (uint32_t i = keep; i < v->nqueue && v->nflushed < MAX_QUEUE; ++i)
        v->flushed[v->nflushed++] = v->queue[i].ctx;
    v->nqueue = keep;
    if (!keep)
        v->pos = 0;
    RET(S_OK, 1);
}

static void sh_S_Discontinuity(Guest* g)
{
    THIS_VOICE(1);
    if (v->nqueue)
        v->queue[v->nqueue - 1].flags |= END_OF_STREAM;
    RET(S_OK, 1);
}

static void sh_S_ExitLoop(Guest* g)
{
    THIS_VOICE(2);
    if (v->nqueue)
        v->queue[0].loop_count = v->queue[0].loops_done; /* no further loops */
    RET(S_OK, 2);
}

/* GetState(XAUDIO2_VOICE_STATE*) {pCurrentBufferContext, BuffersQueued, SamplesPlayed} */
static void sh_S_GetState(Guest* g)
{
    THIS_VOICE(2);
    uint32_t p = ARG(1);
    wr32(p, v->nqueue ? v->queue[0].ctx : 0);
    wr32(p + 4, v->nqueue);
    wr32(p + 8, (uint32_t)v->played);
    wr32(p + 12, (uint32_t)(v->played >> 32));
    RET(0, 2);
}

static void sh_S_SetFrequencyRatio(Guest* g)
{
    THIS_VOICE(3);
    float r = u2f(ARG(1));
    if (r < 1.0f / 1024)
        r = 1.0f / 1024;
    if (v->max_ratio > 0 && r > v->max_ratio)
        r = v->max_ratio;
    v->ratio = r;
    RET(S_OK, 3);
}

static void sh_S_GetFrequencyRatio(Guest* g) { THIS_VOICE(2); wr32(ARG(1), f2u(v->ratio)); RET(0, 2); }
static void sh_S_SetSourceSampleRate(Guest* g) { THIS_VOICE(2); v->rate = ARG(1); RET(S_OK, 2); }

/* --- effects: IUnknown + IXAPO + IXAPOParameters on one object {vtable, refs} ------------------------- */

static const char* const EFFECT_METHODS[] = { "QueryInterface", "AddRef", "Release", "GetRegistrationProperties",
    "IsInputFormatSupported", "IsOutputFormatSupported", "Initialize", "Reset", "LockForProcess", "UnlockForProcess",
    "Process", "CalcInputFrames", "CalcOutputFrames" };

static uint32_t effect_create(void)
{
    static uint32_t vtbl;
    if (!vtbl)
        vtbl = make_vtable("XAPO", EFFECT_METHODS, N(EFFECT_METHODS));
    uint32_t o = gheap_alloc(8, 1);
    wr32(o, vtbl);
    wr32(o + 4, 1);
    return o;
}

static void sh_E_QueryInterface(Guest* g)
{
    wr32(ARG(0) + 4, rd32(ARG(0) + 4) + 1);
    wr32(ARG(2), ARG(0));
    RET(S_OK, 3);
}

static void sh_E_AddRef(Guest* g) { uint32_t r = rd32(ARG(0) + 4) + 1; wr32(ARG(0) + 4, r); RET(r, 1); }

/* never freed: a voice may still list it */
static void sh_E_Release(Guest* g)
{
    uint32_t r = rd32(ARG(0) + 4);
    if (r > 1)
        wr32(ARG(0) + 4, --r);
    RET(r, 1);
}

static uint32_t engine_create(void)
{
    vtables();
    if (!g_engine)
    {
        g_engine = gheap_alloc(8, 1);
        wr32(g_engine, g_vtbl_engine);
        rt_log("[xaudio2] engine created (XAudio2 2.4)\n");
    }
    g_engine_refs++;
    return g_engine;
}

#define X(name, fn) { "xaudio2_4.dll", name, fn }
#define V(iface)                                                                                               \
    X(iface "::GetVoiceDetails", sh_V_GetVoiceDetails), X(iface "::SetOutputVoices", sh_V_SetOutputVoices),     \
    X(iface "::SetEffectChain", sh_V_SetEffectChain), X(iface "::EnableEffect", sh_V_EnableEffect),             \
    X(iface "::DisableEffect", sh_V_DisableEffect), X(iface "::GetEffectState", sh_V_GetEffectState),           \
    X(iface "::SetEffectParameters", sh_V_SetEffectParameters),                                                 \
    X(iface "::GetEffectParameters", sh_V_GetEffectParameters),                                                 \
    X(iface "::SetFilterParameters", sh_V_SetFilterParameters),                                                 \
    X(iface "::GetFilterParameters", sh_V_GetFilterParameters),                                                 \
    X(iface "::SetOutputFilterParameters", sh_V_SetOutputFilterParameters),                                     \
    X(iface "::GetOutputFilterParameters", sh_V_GetOutputFilterParameters),                                     \
    X(iface "::SetVolume", sh_V_SetVolume), X(iface "::GetVolume", sh_V_GetVolume),                             \
    X(iface "::SetChannelVolumes", sh_V_SetChannelVolumes), X(iface "::GetChannelVolumes", sh_V_GetChannelVolumes), \
    X(iface "::SetOutputMatrix", sh_V_SetOutputMatrix), X(iface "::GetOutputMatrix", sh_V_GetOutputMatrix),     \
    X(iface "::DestroyVoice", sh_V_DestroyVoice)

static const ShimDef XAUDIO2[] = {
    X("IXAudio2::QueryInterface", sh_X_QueryInterface),
    X("IXAudio2::AddRef", sh_X_AddRef),
    X("IXAudio2::Release", sh_X_Release),
    X("IXAudio2::GetDeviceCount", sh_X_GetDeviceCount),
    X("IXAudio2::GetDeviceDetails", sh_X_GetDeviceDetails),
    X("IXAudio2::Initialize", sh_X_Initialize),
    X("IXAudio2::RegisterForCallbacks", sh_X_RegisterForCallbacks),
    X("IXAudio2::UnregisterForCallbacks", sh_X_UnregisterForCallbacks),
    X("IXAudio2::CreateSourceVoice", sh_X_CreateSourceVoice),
    X("IXAudio2::CreateSubmixVoice", sh_X_CreateSubmixVoice),
    X("IXAudio2::CreateMasteringVoice", sh_X_CreateMasteringVoice),
    X("IXAudio2::StartEngine", sh_X_StartEngine),
    X("IXAudio2::StopEngine", sh_X_StopEngine),
    X("IXAudio2::CommitChanges", sh_X_CommitChanges),
    X("IXAudio2::GetPerformanceData", sh_X_GetPerformanceData),
    X("IXAudio2::SetDebugConfiguration", sh_X_SetDebugConfiguration),
    V("IXAudio2SourceVoice"),
    V("IXAudio2SubmixVoice"),
    V("IXAudio2MasteringVoice"),
    X("IXAudio2SourceVoice::Start", sh_S_Start),
    X("IXAudio2SourceVoice::Stop", sh_S_Stop),
    X("IXAudio2SourceVoice::SubmitSourceBuffer", sh_S_SubmitSourceBuffer),
    X("IXAudio2SourceVoice::FlushSourceBuffers", sh_S_FlushSourceBuffers),
    X("IXAudio2SourceVoice::Discontinuity", sh_S_Discontinuity),
    X("IXAudio2SourceVoice::ExitLoop", sh_S_ExitLoop),
    X("IXAudio2SourceVoice::GetState", sh_S_GetState),
    X("IXAudio2SourceVoice::SetFrequencyRatio", sh_S_SetFrequencyRatio),
    X("IXAudio2SourceVoice::GetFrequencyRatio", sh_S_GetFrequencyRatio),
    X("IXAudio2SourceVoice::SetSourceSampleRate", sh_S_SetSourceSampleRate),
    X("XAPO::QueryInterface", sh_E_QueryInterface),
    X("XAPO::AddRef", sh_E_AddRef),
    X("XAPO::Release", sh_E_Release),
    { NULL, NULL, NULL },
};

void xaudio2_init(void)
{
    thunk_register(XAUDIO2);
    ole_register_host_class(CLSID_XAudio2, engine_create);
    ole_register_host_class(CLSID_XAudio2_Debug, engine_create);
    ole_register_host_class(CLSID_AudioReverb, effect_create);
    ole_register_host_class(CLSID_AudioVolumeMeter, effect_create);
}
