/* Host-side DSP/audio worker implementation. Included into the generated C runtime.
 * Guest-thread ownership ends at the successful input queue push: all source
 * descriptors, ADPCM coefficients, and bytes are copied into owned snapshots.
 * Music (source 1 on this title) is decoded on core 3. The SFX worker on core 4
 * services the other sources, owns final mixing/device submission, and returns
 * an ordered completion for guest-thread DSP memory/event publication.
 */
#define DSP_RATE 32728u
#define DSP_FRAME_SAMPLES 160u
#define DSP_FRAME_NS 4888791ull
#define DSP_REGION0 0x1FF50000u
#define DSP_REGION1 0x1FF70000u
#define DSP_OFF_STATUS 0x820u
#define DSP_OFF_FINAL 0x5224u
#define DSP_OFF_SRC_STATUS 0xA80u
#define DSP_OFF_APP_DSP_CONFIG 0xE20u
#define DSP_OFF_DSP_CONFIG 0x2860u
#define DSP_OFF_SRC_CONFIG 0x800u
#define DSP_OFF_COEFFS 0x2924u
#define DSP_OFF_FRAME_COUNTER 0x7FFEu
#define DSP_NUM_SOURCES 24u
#define DSP_MAX_DECODED 8000000u
#define DSP_QUEUE_CAPACITY 8u
#define DSP_COMPLETION_CAPACITY 16u
#define DSP_LATENCY_SAMPLES 65536u
#define DSP_MUSIC_SOURCE 1u

static int dsp_audio_on;
static uint64_t dsp_next_frame_ns;
static uint64_t dsp_frames_out;
static uint64_t dsp_sources_started, dsp_sources_finished;
static float dsp_master_volume, dsp_aux_volume[2];
static int guest_memory_range(uint32_t, uint64_t, uint32_t *);

/* Source words and buffer payloads are copied only on the guest thread. */
typedef struct {
    uint32_t addr, length, channels, format, looping, buffer_id;
    uint32_t play_position, adpcm_dirty;
    uint16_t ps, yn[2];
    uint8_t *payload;
    uint32_t payload_bytes;
    int16_t coeff[16];
} DspPending;
typedef struct {
    int enabled;
    float rate;
    float gain[3][4];
    uint16_t sync_count;
    /* Source format/channels are persistent per-source registers (Azahar
     * Source::ParseConfig state.format/state.mono_or_stereo): updated only
     * when dirty bit 0 (format) / bit 1 (channels) or bit 30 (embedded
     * buffer) fires, and inherited by every later queued buffer. Reading
     * the raw per-region +180 field per enqueue picked up the other
     * double-buffer region's uninitialized value (0 = PCM8) and played
     * ADPCM stream buffers as noise. */
    uint8_t format, channels;
    DspPending *pending;
    unsigned pending_count;
    unsigned pending_capacity;
    int reset, clear_pending;
    int enabled_dirty, rate_dirty, gain_dirty, sync_dirty;
} DspGuestSource;
static DspGuestSource dsp_guest_src[DSP_NUM_SOURCES];

/* DSP shared-memory access is exclusively guest-thread code. */
static uint16_t dsp_r16(uint32_t a) {
    return (uint16_t)((uint32_t)ram[a] | ((uint32_t)ram[a + 1u] << 8));
}
static uint32_t dsp_r32(uint32_t a) {
    return (uint32_t)ram[a] | ((uint32_t)ram[a + 1u] << 8) |
           ((uint32_t)ram[a + 2u] << 16) | ((uint32_t)ram[a + 3u] << 24);
}
static uint32_t dsp_r32dsp(uint32_t a) {
    uint32_t v = dsp_r32(a);
    return (v << 16) | (v >> 16);
}
static float dsp_rfloat(uint32_t a) {
    uint32_t b = dsp_r32(a);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}
static void dsp_w16(uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)v;
    ram[a + 1u] = (uint8_t)(v >> 8);
}
static void dsp_w32(uint32_t a, uint32_t v) {
    ram[a] = (uint8_t)v;
    ram[a + 1u] = (uint8_t)(v >> 8);
    ram[a + 2u] = (uint8_t)(v >> 16);
    ram[a + 3u] = (uint8_t)(v >> 24);
}
static void dsp_w32dsp(uint32_t a, uint32_t v) { dsp_w32(a, (v << 16) | (v >> 16)); }

static uint32_t dsp_audio_guest_region = DSP_REGION1;
static uint32_t dsp_current_region(void) {
    uint16_t c0 = dsp_r16(DSP_REGION0 + DSP_OFF_FRAME_COUNTER);
    uint16_t c1 = dsp_r16(DSP_REGION1 + DSP_OFF_FRAME_COUNTER);
    uint32_t before = dsp_audio_guest_region;
    if (c0 != c1) {
        if (c0 == 0xFFFFu && c1 != 0xFFFEu) dsp_audio_guest_region = DSP_REGION1;
        else if (c1 == 0xFFFFu && c0 != 0xFFFEu) dsp_audio_guest_region = DSP_REGION0;
        else dsp_audio_guest_region = (c0 > c1) ? DSP_REGION0 : DSP_REGION1;
    } else {
        uint16_t s0 = dsp_r16(DSP_REGION0 + DSP_OFF_DSP_CONFIG);
        uint16_t s1 = dsp_r16(DSP_REGION1 + DSP_OFF_DSP_CONFIG);
        if (s0 != s1)
            dsp_audio_guest_region = ((int16_t)(s0 - s1) > 0) ? DSP_REGION0 : DSP_REGION1;
    }
    if (getenv("DQ8_TRACE_AUDIO") && dsp_audio_guest_region != before)
        fprintf(stderr, "dsp region flip: %s -> %s (c0=%04X c1=%04X)\n",
                before == DSP_REGION0 ? "0" : "1",
                dsp_audio_guest_region == DSP_REGION0 ? "0" : "1", c0, c1);
    return dsp_audio_guest_region;
}

static void dsp_pending_release(DspPending *b) {
    if (b->payload) free(b->payload);
    memset(b, 0, sizeof(*b));
}
static void dsp_guest_pending_clear(DspGuestSource *s) {
    unsigned k;
    for (k = 0; k < s->pending_count; ++k) dsp_pending_release(&s->pending[k]);
    s->pending_count = 0;
}
static void dsp_free_guest_sources(void) {
    unsigned i;
    for (i = 0; i < DSP_NUM_SOURCES; ++i) {
        dsp_guest_pending_clear(&dsp_guest_src[i]);
        free(dsp_guest_src[i].pending);
        dsp_guest_src[i].pending = NULL; dsp_guest_src[i].pending_capacity = 0;
    }
}

static const char *dsp_config_reject_reason(uint32_t dirty, uint32_t cfg) {
    if (dirty & 0x8000FFE0u) return "reserved dirty bits";
    if (dirty & ((1u << 25) | (1u << 26) | (1u << 27))) {
        unsigned k;
        for (k = 0; k < 12; ++k) {
            float g = dsp_rfloat(cfg + 4u + k * 4u);
            /* This firmware's real movie stream uses 1.4173; retain finite,
               bounded validation while accepting that source gain. */
            if (!isfinite(g) || g < -8.0f || g > 8.0f) return "gain outside finite +/-8";
        }
    }
    if (dirty & (1u << 18)) {
        float r = dsp_rfloat(cfg + 52u);
        if (!(r > 0.0f) || r > 4.0f) return "rate multiplier out of range";
    }
    return NULL;
}
static const char *dsp_buffer_reject_reason(const DspPending *b) {
    uint32_t mapped;
    uint64_t bytes;
    if ((b->addr & 3u) != 0) return "unaligned address";
    if (b->addr < 0x14000000u) return "address below LINEAR";
    if ((int32_t)b->length <= 0 || b->length > DSP_MAX_DECODED) return "length out of range";
    if (b->format > 2u) return "format > ADPCM";
    if (b->channels == 0u || b->channels > 2u) return "bad channel count";
    bytes = (b->format == 2u) ? ((uint64_t)b->length + 13u) / 14u * 8u
           : (uint64_t)b->length * b->channels * (b->format == 0u ? 1u : 2u);
    if (!guest_memory_range(b->addr & 0xFFFFFFFCu, bytes, &mapped)) return "outside guest RAM";
    return NULL;
}
static int dsp_copy_pending(unsigned i, uint32_t region, DspPending *b) {
    uint32_t mapped, other;
    unsigned k;
    b->payload_bytes = (b->format == 2u) ? ((b->length + 13u) / 14u) * 8u
                       : b->length * b->channels * (b->format == 0u ? 1u : 2u);
    if (!guest_memory_range(b->addr & 0xFFFFFFFCu, b->payload_bytes, &mapped)) return 0;
    b->payload = (uint8_t *)malloc(b->payload_bytes);
    if (!b->payload) {
        fprintf(stderr, "dsp audio: cannot snapshot src=%u buffer=%u (%u bytes)\n",
                i, b->buffer_id, b->payload_bytes);
        return 0;
    }
    memcpy(b->payload, ram + mapped, b->payload_bytes);
    if (b->format == 2u) {
        uint32_t coeff_off = region + DSP_OFF_COEFFS + i * 32u;
        other = (region == DSP_REGION0) ? DSP_REGION1 : DSP_REGION0;
        for (k = 0; k < 16u; ++k) b->coeff[k] = (int16_t)dsp_r16(coeff_off + k * 2u);
        if (b->coeff[0] == 0 && b->coeff[1] == 0)
            for (k = 0; k < 16u; ++k)
                b->coeff[k] = (int16_t)dsp_r16(other + DSP_OFF_COEFFS + i * 32u + k * 2u);
    }
    return 1;
}
static int dsp_enqueue(unsigned i, uint32_t region, const DspPending *input) {
    DspGuestSource *s = &dsp_guest_src[i];
    DspPending copy = *input;
    copy.payload = NULL;
    {
        const char *why = dsp_buffer_reject_reason(&copy);
        if (why) {
            if (getenv("DQ8_TRACE_AUDIO"))
                fprintf(stderr, "dsp enqueue REJECTED: src=%u addr=%08X len=%u fmt=%u (%s)\n",
                        i, copy.addr, copy.length, copy.format, why);
            return 1; /* invalid middleware slots are ignored as before */
        }
    }
    if (s->pending_count == s->pending_capacity) {
        unsigned new_capacity = s->pending_capacity ? s->pending_capacity * 2u : 8u;
        DspPending *new_pending = (DspPending *)realloc(s->pending, new_capacity * sizeof(DspPending));
        if (!new_pending) {
            fprintf(stderr, "dsp audio: cannot grow pending snapshots for src=%u\n", i);
            return 0;
        }
        s->pending = new_pending; s->pending_capacity = new_capacity;
    }
    if (!dsp_copy_pending(i, region, &copy)) return 0;
    s->pending[s->pending_count++] = copy;
    return 1;
}
static int dsp_parse_source_config(unsigned i, uint32_t cfg) {
    DspGuestSource *s = &dsp_guest_src[i];
    uint32_t dirty = dsp_r32(cfg);
    unsigned region_index = cfg < DSP_REGION1 ? 0u : 1u;
    unsigned old_pending_count = s->pending_count;
    static uint32_t last_rej[DSP_NUM_SOURCES][2];
    if (!dirty) return 1;
    {
        const char *why = dsp_config_reject_reason(dirty, cfg);
        if (why) {
            if (getenv("DQ8_TRACE_AUDIO") && last_rej[i][region_index] != dirty)
                fprintf(stderr, "dsp config REJECTED: src=%u region=%u dirty=%08X (%s)\n",
                        i, region_index, dirty, why);
            last_rej[i][region_index] = dirty;
            return 1;
        }
        last_rej[i][region_index] = 0;
    }
    if (dirty & (1u << 29)) {
        dsp_guest_pending_clear(s);
        s->enabled = 0; s->reset = 1; s->clear_pending = 0;
        s->enabled_dirty = 1; s->gain_dirty = 1;
        for (unsigned k = 0; k < 3u; ++k)
            for (unsigned j = 0; j < 4u; ++j) s->gain[k][j] = 0.0f;
    }
    if (dirty & (1u << 4)) {
        dsp_guest_pending_clear(s);
        s->clear_pending = 1;
    }
    if (dirty & (1u << 16)) { s->enabled = ram[cfg + 160u] != 0; s->enabled_dirty = 1; }
    if (dirty & (1u << 18)) {
        float r = dsp_rfloat(cfg + 52u);
        s->rate = (r > 0.0f) ? r : 1.0f; s->rate_dirty = 1;
    }
    if (dirty & (1u << 28)) { s->sync_count = dsp_r16(cfg + 162u); s->sync_dirty = 1; }
    /* Azahar ParseConfig: format/channels are persistent source registers
     * refreshed on their own dirty bits or on an embedded-buffer update. */
    if (dirty & ((1u << 0) | (1u << 30)))
        s->format = (uint8_t)((dsp_r16(cfg + 180u) >> 2) & 3u);
    if (dirty & ((1u << 1) | (1u << 30)))
        s->channels = ((dsp_r16(cfg + 180u) & 3u) == 2u) ? 2u : 1u;
    if (dirty & ((1u << 25) | (1u << 26) | (1u << 27))) {
        unsigned k, j;
        for (k = 0; k < 3u; ++k)
            for (j = 0; j < 4u; ++j)
                s->gain[k][j] = dsp_rfloat(cfg + 4u + (k * 4u + j) * 4u);
        s->gain_dirty = 1;
    }
    if (dirty & (1u << 30)) {
        DspPending b;
        uint16_t flags2 = dsp_r16(cfg + 188u);
        memset(&b, 0, sizeof(b));
        b.addr = dsp_r32dsp(cfg + 172u); b.length = dsp_r32dsp(cfg + 176u);
        b.channels = s->channels ? s->channels : 1u;
        b.format = s->format; b.looping = (flags2 >> 1) & 1u;
        b.buffer_id = dsp_r16(cfg + 190u); b.play_position = dsp_r32dsp(cfg + 164u);
        b.adpcm_dirty = flags2 & 1u; b.ps = dsp_r16(cfg + 182u);
        b.yn[0] = dsp_r16(cfg + 184u); b.yn[1] = dsp_r16(cfg + 186u);
        if (getenv("DQ8_TRACE_AUDIO"))
            fprintf(stderr, "dsp guest buffer: src=%u region=%u id=%u addr=%08X len=%u fmt=%u ch=%u loop=%u dirty=%08X adpcm_dirty=%u yn=%d,%d\n",
                    i, region_index, b.buffer_id, b.addr, b.length, b.format, b.channels, b.looping, dirty,
                    b.adpcm_dirty ? 1u : 0u, (int)b.yn[0], (int)b.yn[1]);
        if (!dsp_enqueue(i, region_index ? DSP_REGION1 : DSP_REGION0, &b)) goto snapshot_failed;
    }
    if (dirty & (1u << 19)) {
        uint16_t bd = dsp_r16(cfg + 74u), flags1 = dsp_r16(cfg + 180u);
        if (getenv("DQ8_TRACE_AUDIO"))
            fprintf(stderr, "dsp guest queue: src=%u region=%u dirty=%08X bd=%04X flags1=%04X\n",
                    i, region_index, dirty, bd, flags1);
        for (unsigned k = 0; k < 4u; ++k) if (bd & (1u << k)) {
            uint32_t base = cfg + 76u + k * 20u;
            DspPending b;
            memset(&b, 0, sizeof(b));
            b.addr = dsp_r32dsp(base); b.length = dsp_r32dsp(base + 4u);
            b.channels = s->channels ? s->channels : 1u;
            b.format = s->format; b.looping = ram[base + 15u];
            b.buffer_id = dsp_r16(base + 16u); b.adpcm_dirty = ram[base + 14u];
            b.ps = dsp_r16(base + 8u); b.yn[0] = dsp_r16(base + 10u); b.yn[1] = dsp_r16(base + 12u);
            if (getenv("DQ8_TRACE_AUDIO"))
                fprintf(stderr, "dsp guest qbuf: src=%u region=%u slot=%u id=%u addr=%08X len=%u fmt=%u loop=%u adpcm_dirty=%u yn=%d,%d\n",
                        i, region_index, k, b.buffer_id, b.addr, b.length, b.format, b.looping,
                        b.adpcm_dirty ? 1u : 0u, (int)b.yn[0], (int)b.yn[1]);
            if (!dsp_enqueue(i, region_index ? DSP_REGION1 : DSP_REGION0, &b)) goto snapshot_failed;
        }
        dsp_w16(cfg + 74u, 0);
    }
    dsp_w32(cfg, 0);
    return 1;
snapshot_failed:
    while (s->pending_count > old_pending_count)
        dsp_pending_release(&s->pending[--s->pending_count]);
    return 0;
}

/* Immutable job snapshot. A payload pointer is a borrowed guest-owned token
 * while queued only in the producer's pending frame; successful SPSC push
 * transfers the whole job and its payloads to its sole worker consumer. */
typedef struct {
    int enabled, reset, clear_pending;
    int enabled_dirty, rate_dirty, gain_dirty, sync_dirty;
    float rate, gain[3][4];
    uint16_t sync_count;
    unsigned pending_count;
    DspPending *pending;
} DspWorkerInput;
typedef struct {
    uint8_t enabled, buffer_update;
    uint16_t sync_count, current_buffer_id, last_buffer_id;
    uint32_t pos;
} DspSourceResult;
typedef struct {
    uint64_t sequence, submitted_ns, deadline_ns;
    uint32_t read_region, write_region;
    int notify_guest;
    float master, aux[2];
    DspWorkerInput source[DSP_NUM_SOURCES];
    DspSourceResult result[DSP_NUM_SOURCES];
    int32_t mix_l[DSP_FRAME_SAMPLES], mix_r[DSP_FRAME_SAMPLES];
    int16_t final_pcm[DSP_FRAME_SAMPLES * 2u];
    uint64_t sources_started, sources_finished;
    uint64_t feed_to_sink_ns;
} DspAudioJob;

static uintptr_t dsp_music_input_slots[DSP_QUEUE_CAPACITY];
static uintptr_t dsp_sfx_input_slots[DSP_QUEUE_CAPACITY];
static uintptr_t dsp_music_to_mixer_slots[DSP_QUEUE_CAPACITY];
static uintptr_t dsp_completion_slots[DSP_COMPLETION_CAPACITY];
static Dq8SpscQueue dsp_music_input, dsp_sfx_input, dsp_music_to_mixer, dsp_completions;
static pthread_t dsp_music_thread, dsp_sfx_thread;
static int dsp_workers_started, dsp_music_affinity_ok, dsp_sfx_affinity_ok;
static Dq8WorkerScaffold dsp_audio_worker_gate;
static DspAudioJob *dsp_pending_music_job, *dsp_pending_sfx_job;
static void dsp_audio_transfer_shadow(int music);
static uint64_t dsp_audio_sequence;
static uint64_t dsp_audio_sink_empty, dsp_audio_content_gaps, dsp_audio_sink_checks;
static uint64_t dsp_audio_deadline_misses;
static uint64_t dsp_audio_latency[DSP_LATENCY_SAMPLES];
static uint32_t dsp_audio_latency_count;

/* WAV/SDL sink has exactly one owner after worker startup: core-4 SFX/mixer. */
static FILE *dsp_wav_file;
static uint64_t dsp_wav_frames;
static char dsp_wav_path[512];
#ifdef DQ8_ENABLE_SDL
static unsigned dsp_sdl_dev;
#endif
static int dsp_out_init_done;
static void dsp_wav_update_header(void) {
    uint8_t h[44];
    uint32_t bytes = (uint32_t)(dsp_wav_frames * DSP_FRAME_SAMPLES * 4u), riff = 36u + bytes, i;
    memcpy(h, "RIFF", 4);
    for (i = 0; i < 4; ++i) h[4u + i] = (uint8_t)(riff >> (8 * i));
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = (uint8_t)DSP_RATE; h[25] = (uint8_t)(DSP_RATE >> 8); h[26] = 0; h[27] = 0;
    { uint32_t br = DSP_RATE * 4u;
      h[28] = (uint8_t)br; h[29] = (uint8_t)(br >> 8); h[30] = (uint8_t)(br >> 16); h[31] = (uint8_t)(br >> 24);
      h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0; }
    memcpy(h + 36, "data", 4);
    for (i = 0; i < 4; ++i) h[40u + i] = (uint8_t)(bytes >> (8 * i));
    if (fseek(dsp_wav_file, 0, SEEK_SET) == 0) { fwrite(h, 1, sizeof(h), dsp_wav_file); fseek(dsp_wav_file, 0, SEEK_END); }
}
static void dsp_audio_out_finish(void) {
    if (dsp_wav_file) { dsp_wav_update_header(); fclose(dsp_wav_file); dsp_wav_file = NULL; }
}
static void dsp_audio_out_init(void) {
    const char *wav;
    if (dsp_out_init_done) return;
    dsp_out_init_done = 1;
    wav = getenv("DQ8_AUDIO_DUMP");
    if (wav && wav[0]) {
        snprintf(dsp_wav_path, sizeof dsp_wav_path, "%s", wav);
        dsp_wav_file = fopen(dsp_wav_path, "wb");
        if (dsp_wav_file) {
            uint8_t zero[44] = {0}; fwrite(zero, 1, sizeof(zero), dsp_wav_file);
            atexit(dsp_audio_out_finish);
            fprintf(stderr, "host audio: recording %u Hz stereo WAV to %s\n", DSP_RATE, dsp_wav_path);
        } else fprintf(stderr, "host audio: cannot open %s for writing\n", dsp_wav_path);
    }
#ifdef DQ8_ENABLE_SDL
    {
        const char *nosdl = getenv("DQ8_AUDIO_SDL");
        SDL_AudioSpec want, have;
        if (!(nosdl && nosdl[0] == '0')) {
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "host audio: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            else {
                memset(&want, 0, sizeof(want)); want.freq = (int)DSP_RATE; want.format = AUDIO_S16SYS;
                want.channels = 2; want.samples = 480;
                dsp_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
                if (!dsp_sdl_dev) fprintf(stderr, "host audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
                else { SDL_PauseAudioDevice(dsp_sdl_dev, 0); fprintf(stderr, "host audio: SDL2 output %d Hz %u ch\n", have.freq, have.channels); }
            }
        }
    }
#endif
}
static uint64_t dsp_audio_sink_drops;
static uint64_t dsp_audio_clip_samples;
static int dsp_audio_sdl_has_queued_audio, dsp_audio_previous_frame_active;
static int dsp_audio_pcm_active(const int16_t *pcm) {
    for (unsigned i = 0; i < DSP_FRAME_SAMPLES * 2u; ++i)
        if (pcm[i] != 0) return 1;
    return 0;
}
/* Return queued device audio ahead of this frame in nanoseconds. This is the
 * host-device estimate added to feed-to-queue processing time for feed-to-play. */
static uint64_t dsp_audio_out_push(const int16_t *pcm) {
    uint64_t queue_ahead_ns = 0;
    int frame_active = dsp_audio_pcm_active(pcm);
    if (dsp_wav_file) {
        if (fwrite(pcm, 4u, DSP_FRAME_SAMPLES, dsp_wav_file) != DSP_FRAME_SAMPLES)
            ++dsp_audio_sink_drops;
        dsp_wav_frames++;
        if ((dsp_wav_frames & 0xFFu) == 0u) dsp_wav_update_header();
    }
#ifdef DQ8_ENABLE_SDL
    if (dsp_sdl_dev) {
        uint32_t queued = SDL_GetQueuedAudioSize(dsp_sdl_dev);
        queue_ahead_ns = ((uint64_t)queued * 1000000000ull) / (DSP_RATE * 4u);
        /* A late refill is not an underrun. Track an empty SDL queue separately;
         * count a content gap only between adjacent non-silent output frames. */
        if (dsp_audio_sdl_has_queued_audio) {
            ++dsp_audio_sink_checks;
            if (queued == 0u) {
                ++dsp_audio_sink_empty;
                if (dsp_audio_previous_frame_active && frame_active) ++dsp_audio_content_gaps;
            }
        }
        if (queued <= DSP_RATE * 4u) {
            if (SDL_QueueAudio(dsp_sdl_dev, pcm, DSP_FRAME_SAMPLES * 4u) != 0) ++dsp_audio_sink_drops;
            else {
                dsp_audio_sdl_has_queued_audio = 1;
                dsp_audio_previous_frame_active = frame_active;
            }
        } else ++dsp_audio_sink_drops;
    }
#endif
    return queue_ahead_ns;
}

static void dsp_sleep_brief(void) {
    struct timespec ts = {0, 100000};
    nanosleep(&ts, NULL);
}
/* Empty-queue poll interval of the two audio workers (kanban t_83f501d5).
 * dsp_sleep_brief() is deliberately left untouched for the producer /
 * backpressure / shutdown paths so completion handoff, queue-full pushes and
 * the sentinel/shutdown handshake keep their existing timing contract. This
 * knob only lengthens the *idle* wait, i.e. the voluntary sleep a worker takes
 * when its input queue is empty. Default = 100000 ns = today's behaviour, so
 * DQ8_AUDIO_POLL_US unset is a no-op; only an explicit value changes the sleep.
 * Read once at worker start (dsp_empty_poll_configure) so it is stable for the
 * run. Reversible: unset the env var and the binary behaves as before. */
static struct timespec dsp_empty_poll_ts = {0, 100000};
static void dsp_sleep_empty(void) {
    nanosleep(&dsp_empty_poll_ts, NULL);
}
static void dsp_empty_poll_configure(void) {
    const char *v = getenv("DQ8_AUDIO_POLL_US");
    long us;
    if (!v || !*v) { fprintf(stderr, "dsp audio: empty-queue poll=100 us (default)\n"); return; }
    us = strtol(v, NULL, 10);
    if (us < 1) us = 1;
    dsp_empty_poll_ts.tv_sec = us / 1000000L;
    dsp_empty_poll_ts.tv_nsec = (us % 1000000L) * 1000L;
    fprintf(stderr, "dsp audio: empty-queue poll=%ld us (DQ8_AUDIO_POLL_US)\n", us);
}
/* ---- Event-driven worker wakeup (B3e, reports/PERF_AUDIO_EVENT_WAKEUP_DESIGN.md) ----
 * One mutex guards every queue state mutation and predicate check; each
 * mutation signals the affected condvar while still holding the mutex (Mesa
 * predicate loops; signal-under-lock). The wrappers below are the only code
 * allowed to touch these four queues in event mode; in poll mode (default)
 * every wrapper degrades to the raw SPSC call plus the existing nanosleep
 * loops, so the default path is behaviourally identical to before (P1
 * DQ8_AUDIO_POLL_US semantics unchanged). The mutex is never held across job
 * processing, allocation, SDL calls, guest_notify_handle or nanosleep;
 * critical sections are single O(1) try_push/try_pop calls, and one mutex
 * means no lock-ordering deadlock is possible by construction. */
static pthread_mutex_t dsp_wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dsp_cv_music_in_ne = PTHREAD_COND_INITIALIZER; /* W1 */
static pthread_cond_t dsp_cv_sfx_in_ne = PTHREAD_COND_INITIALIZER;   /* W3 */
static pthread_cond_t dsp_cv_mixer_ne = PTHREAD_COND_INITIALIZER;    /* W4 */
static pthread_cond_t dsp_cv_mixer_nf = PTHREAD_COND_INITIALIZER;    /* W2 */
static pthread_cond_t dsp_cv_comp_nf = PTHREAD_COND_INITIALIZER;     /* W5 */
static int dsp_wake_event_mode;      /* DQ8_AUDIO_WAKE_MODE=event */
static long dsp_wake_watchdog_us;    /* 0 = pure event blocking */
/* Wait on a condvar under dsp_wake_mu. With the watchdog configured this is
 * pthread_cond_timedwait: a missed signal degrades to exactly P1's N-us
 * polling, absorbed by the predicate loop. Event mode only; mutex held. */
static void dsp_cond_wait_wdog(pthread_cond_t *cv) {
    struct timespec ts;
    if (dsp_wake_watchdog_us <= 0) { pthread_cond_wait(cv, &dsp_wake_mu); return; }
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += dsp_wake_watchdog_us / 1000000L;
    ts.tv_nsec += (dsp_wake_watchdog_us % 1000000L) * 1000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(cv, &dsp_wake_mu, &ts);
}
static void dsp_wake_configure(void) {
    const char *v = getenv("DQ8_AUDIO_WAKE_MODE");
    if (v && v[0] && strcmp(v, "poll") != 0) {
        if (strcmp(v, "event") == 0) dsp_wake_event_mode = 1;
        else fprintf(stderr, "dsp audio: unknown DQ8_AUDIO_WAKE_MODE=%s (poll assumed)\n", v);
    }
    if (dsp_wake_event_mode) {
        const char *p = getenv("DQ8_AUDIO_POLL_US");
        if (p && *p) {
            long us = strtol(p, NULL, 10);
            if (us < 1) us = 1;
            dsp_wake_watchdog_us = us;
            fprintf(stderr, "dsp audio: wake mode=event watchdog=%ld us\n", us);
        } else fprintf(stderr, "dsp audio: wake mode=event watchdog=off\n");
    }
}
/* The eight queue wrappers (design section 2.2). Blocking wrappers block
 * only in event mode; poll mode keeps the pre-B3e spin/sleep loops. */
static int dsp_push_music_in_try(uintptr_t v) {   /* main submit + shutdown: no block */
    int ok;
    if (!dsp_wake_event_mode) return dq8_spsc_try_push(&dsp_music_input, v);
    pthread_mutex_lock(&dsp_wake_mu);
    ok = dq8_spsc_try_push(&dsp_music_input, v);
    if (ok) pthread_cond_signal(&dsp_cv_music_in_ne);
    pthread_mutex_unlock(&dsp_wake_mu);
    return ok;
}
static int dsp_push_sfx_in_try(uintptr_t v) {
    int ok;
    if (!dsp_wake_event_mode) return dq8_spsc_try_push(&dsp_sfx_input, v);
    pthread_mutex_lock(&dsp_wake_mu);
    ok = dq8_spsc_try_push(&dsp_sfx_input, v);
    if (ok) pthread_cond_signal(&dsp_cv_sfx_in_ne);
    pthread_mutex_unlock(&dsp_wake_mu);
    return ok;
}
static int dsp_pop_music_in(uintptr_t *token) {   /* W1: blocks in event mode */
    if (!dsp_wake_event_mode) return dq8_spsc_try_pop(&dsp_music_input, token);
    pthread_mutex_lock(&dsp_wake_mu);
    while (!dq8_spsc_try_pop(&dsp_music_input, token)) dsp_cond_wait_wdog(&dsp_cv_music_in_ne);
    pthread_mutex_unlock(&dsp_wake_mu);
    return 1;
}
static int dsp_pop_sfx_in(uintptr_t *token) {     /* W3: blocks in event mode */
    if (!dsp_wake_event_mode) return dq8_spsc_try_pop(&dsp_sfx_input, token);
    pthread_mutex_lock(&dsp_wake_mu);
    while (!dq8_spsc_try_pop(&dsp_sfx_input, token)) dsp_cond_wait_wdog(&dsp_cv_sfx_in_ne);
    pthread_mutex_unlock(&dsp_wake_mu);
    return 1;
}
static void dsp_push_mixer(uintptr_t v) {         /* W2: blocks for a slot in event mode */
    if (!dsp_wake_event_mode) {
        while (!dq8_spsc_try_push(&dsp_music_to_mixer, v)) dsp_sleep_brief();
        return;
    }
    pthread_mutex_lock(&dsp_wake_mu);
    while (!dq8_spsc_try_push(&dsp_music_to_mixer, v)) dsp_cond_wait_wdog(&dsp_cv_mixer_nf);
    pthread_cond_signal(&dsp_cv_mixer_ne);
    pthread_mutex_unlock(&dsp_wake_mu);
}
static int dsp_pop_mixer(uintptr_t *token) {      /* W4: blocks for a token in event mode */
    if (!dsp_wake_event_mode) return dq8_spsc_try_pop(&dsp_music_to_mixer, token);
    pthread_mutex_lock(&dsp_wake_mu);
    while (!dq8_spsc_try_pop(&dsp_music_to_mixer, token)) dsp_cond_wait_wdog(&dsp_cv_mixer_ne);
    pthread_cond_signal(&dsp_cv_mixer_nf);
    pthread_mutex_unlock(&dsp_wake_mu);
    return 1;
}
static void dsp_push_completions(uintptr_t v) {   /* W5: blocks for a slot in event mode */
    if (!dsp_wake_event_mode) {
        while (!dq8_spsc_try_push(&dsp_completions, v)) dsp_sleep_brief();
        return;
    }
    pthread_mutex_lock(&dsp_wake_mu);
    while (!dq8_spsc_try_push(&dsp_completions, v)) dsp_cond_wait_wdog(&dsp_cv_comp_nf);
    pthread_mutex_unlock(&dsp_wake_mu);
}
static int dsp_pop_completions_try(uintptr_t *token) { /* main commit/drain: no block */
    int ok;
    if (!dsp_wake_event_mode) return dq8_spsc_try_pop(&dsp_completions, token);
    pthread_mutex_lock(&dsp_wake_mu);
    ok = dq8_spsc_try_pop(&dsp_completions, token);
    if (ok) pthread_cond_signal(&dsp_cv_comp_nf);
    pthread_mutex_unlock(&dsp_wake_mu);
    return ok;
}
static void dsp_set_worker_affinity(unsigned core, const char *name, int *ok) {
#if defined(__linux__)
    cpu_set_t set;
    int rc;
    CPU_ZERO(&set); CPU_SET(core, &set);
    rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    *ok = rc == 0;
    if (rc) fprintf(stderr, "dsp %s worker: affinity core %u failed (%s)\n", name, core, strerror(rc));
    else fprintf(stderr, "dsp %s worker: pinned to host core %u\n", name, core);
#else
    (void)core; *ok = 0;
    fprintf(stderr, "dsp %s worker: host affinity API unavailable\n", name);
#endif
}
#ifdef DQ8_AUDIO_TESTING
static void dsp_audio_test_delay(void) {
    const char *value = getenv("DQ8_AUDIO_TEST_DELAY_NS");
    uint64_t ns = value ? strtoull(value, NULL, 10) : 0u;
    if (ns > 0u && ns <= 1000000000ull) {
        struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
}
#else
static void dsp_audio_test_delay(void) {}
#endif

/* Worker-owned source state; never points into guest RAM. */
typedef struct {
    int enabled;
    float rate, gain[3][4];
    uint16_t sync_count;
    int16_t *decoded;
    uint8_t *encoded;
    uint32_t sample_count, channels, format, looping, pos;
    float frac;
    uint16_t current_buffer_id, last_buffer_id;
    int buffer_update;
    DspPending *pending;
    unsigned pending_count;
    unsigned pending_capacity;
    /* ADPCM predictor history carried across buffers (Azahar ADPCMState):
     * reloaded from a buffer descriptor only when its adpcm_dirty bit is
     * set, otherwise it continues from the previous decode. */
    int16_t adpcm_yn1, adpcm_yn2;
    /* Snapshot of the current looping ADPCM buffer so the loop wrap can be
     * re-decoded with carried history instead of reusing stale PCM. */
    DspPending loop_buf;
    int loop_buf_valid;
} DspWorkerSource;
static uint64_t dsp_adpcm_hist_reloads, dsp_adpcm_hist_carries, dsp_adpcm_loop_redecodes;
static DspWorkerSource dsp_music_state[DSP_NUM_SOURCES];
static DspWorkerSource dsp_sfx_state[DSP_NUM_SOURCES];
static void dsp_worker_source_free_current(DspWorkerSource *s) {
    free(s->decoded); free(s->encoded); s->decoded = NULL; s->encoded = NULL; s->sample_count = 0;
    if (s->loop_buf_valid) { dsp_pending_release(&s->loop_buf); s->loop_buf_valid = 0; }
}
static void dsp_worker_source_free_pending(DspWorkerSource *s) {
    for (unsigned k = 0; k < s->pending_count; ++k) dsp_pending_release(&s->pending[k]);
    free(s->pending); s->pending = NULL; s->pending_capacity = 0;
    s->pending_count = 0;
}
static void dsp_worker_states_free(DspWorkerSource *states) {
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        dsp_worker_source_free_current(&states[i]); dsp_worker_source_free_pending(&states[i]);
    }
}
static int16_t *dsp_decode_adpcm(const DspPending *b, int16_t *yn1_io, int16_t *yn2_io) {
    static const int nibble_signed[16] = {0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1};
    uint32_t frames = (b->length + 13u) / 14u, fi, i, total = frames * 14u;
    int16_t *out;
    int yn1 = *yn1_io, yn2 = *yn2_io;
    if (total > DSP_MAX_DECODED) return NULL;
    out = (int16_t *)malloc(total * sizeof(int16_t));
    if (!out) return NULL;
    for (fi = 0; fi < frames; ++fi) {
        uint32_t base = fi * 8u;
        int header = b->payload[base];
        int fr_scale = 1 << (header & 0xF), fr_idx = (header >> 4) & 0x7;
        for (i = 0; i < 14u && fi * 14u + i < b->length; ++i) {
            uint8_t byte = b->payload[base + 1u + i / 2u];
            int nib = nibble_signed[(i & 1u) ? (byte & 0xFu) : (byte >> 4)];
            int xn = nib * fr_scale;
            int val = ((xn << 11) + 0x400 + b->coeff[fr_idx * 2] * yn1 + b->coeff[fr_idx * 2 + 1] * yn2) >> 11;
            if (val > 32767) val = 32767; if (val < -32768) val = -32768;
            yn2 = yn1; yn1 = val; out[fi * 14u + i] = (int16_t)val;
        }
    }
    /* Persist the predictor history for the next decode, mirroring Azahar's
     * ADPCMState carry across buffers (codec.cpp writes state back). */
    *yn1_io = (int16_t)yn1; *yn2_io = (int16_t)yn2;
    return out;
}
static void dsp_worker_apply_input(DspWorkerSource *s, DspWorkerInput *in, DspAudioJob *job) {
    if (in->reset) {
        dsp_worker_source_free_current(s); dsp_worker_source_free_pending(s);
        s->enabled = 0; s->buffer_update = 0; s->pos = 0; s->frac = 0.0f;
        s->adpcm_yn1 = 0; s->adpcm_yn2 = 0;
        memset(s->gain, 0, sizeof(s->gain));
    } else if (in->clear_pending) dsp_worker_source_free_pending(s);
    if (in->enabled_dirty) s->enabled = in->enabled;
    if (in->rate_dirty) s->rate = in->rate;
    if (in->sync_dirty) s->sync_count = in->sync_count;
    if (in->gain_dirty) memcpy(s->gain, in->gain, sizeof(s->gain));
    if (s->pending_count + in->pending_count > s->pending_capacity) {
        unsigned needed = s->pending_count + in->pending_count;
        unsigned new_capacity = s->pending_capacity ? s->pending_capacity : 8u;
        while (new_capacity < needed) new_capacity *= 2u;
        DspPending *new_pending = (DspPending *)realloc(s->pending, new_capacity * sizeof(DspPending));
        if (!new_pending) {
            fprintf(stderr, "dsp audio worker: cannot grow source queue for frame %llu\n",
                    (unsigned long long)job->sequence);
            abort(); /* no buffer may be silently dropped or returned as consumed */
        }
        s->pending = new_pending; s->pending_capacity = new_capacity;
    }
    for (unsigned k = 0; k < in->pending_count; ++k) {
        s->pending[s->pending_count++] = in->pending[k];
        in->pending[k].payload = NULL; /* ownership moved into worker state */
    }
    free(in->pending); in->pending = NULL; in->pending_count = 0;
}
static int dsp_worker_dequeue(unsigned source, DspWorkerSource *s, DspAudioJob *job) {
    DspPending b;
    if (!s->pending_count) return 0;
    b = s->pending[0];
    memmove(&s->pending[0], &s->pending[1], (s->pending_count - 1u) * sizeof(DspPending));
    s->pending_count--;
    dsp_worker_source_free_current(s);
    s->channels = b.channels; s->format = b.format; s->looping = b.looping;
    s->current_buffer_id = (uint16_t)b.buffer_id; s->last_buffer_id = 0;
    s->buffer_update = 1; s->pos = b.play_position;
    /* The resampler phase carries across buffer boundaries (Azahar's
     * AudioInterp keeps its fractional position between dequeues); only a
     * source reset zeroes it. */
    if ((int32_t)b.length <= 0 || b.length > DSP_MAX_DECODED) { dsp_pending_release(&b); return 1; }
    if (b.format == 2u) {
        if (b.adpcm_dirty) {
            s->adpcm_yn1 = b.yn[0]; s->adpcm_yn2 = b.yn[1];
            ++dsp_adpcm_hist_reloads;
        } else ++dsp_adpcm_hist_carries;
        s->decoded = dsp_decode_adpcm(&b, &s->adpcm_yn1, &s->adpcm_yn2);
        if (b.looping) { s->loop_buf = b; s->loop_buf_valid = 1; b.payload = NULL; }
        else { free(b.payload); b.payload = NULL; }
        if (!s->decoded) {
            fprintf(stderr, "dsp audio worker: ADPCM decode allocation failed for src=%u buffer=%u\n",
                    source, b.buffer_id);
            abort();
        }
    } else {
        s->encoded = b.payload; b.payload = NULL;
    }
    s->sample_count = b.length;
    if (s->pos > s->sample_count) s->pos = 0;
    job->sources_started++;
    if (getenv("DQ8_TRACE_AUDIO"))
        fprintf(stderr, "dsp worker dequeue: src=%u id=%u len=%u fmt=%u ch=%u loop=%u adpcm_dirty=%u yn=%d,%d\n",
                source, b.buffer_id, b.length, b.format, b.channels, b.looping,
                b.adpcm_dirty ? 1u : 0u, (int)s->adpcm_yn1, (int)s->adpcm_yn2);
    return 1;
}
static int16_t dsp_worker_fetch(const DspWorkerSource *s, uint32_t frame, unsigned ch) {
    uint32_t idx = frame * s->channels + ch;
    if (frame >= s->sample_count) return 0;
    if (s->decoded) return s->decoded[idx];
    if (s->format == 0u) return (int16_t)((uint16_t)s->encoded[idx] << 8);
    return (int16_t)((uint16_t)s->encoded[idx * 2u] | ((uint16_t)s->encoded[idx * 2u + 1u] << 8));
}
static void dsp_process_source(unsigned i, DspWorkerSource *s, DspWorkerInput *in, DspAudioJob *job) {
    float gl, gr;
    dsp_worker_apply_input(s, in, job);
    if (s->enabled) {
        gl = job->master * (s->gain[0][0] + s->gain[0][2]) +
             job->aux[0] * (s->gain[1][0] + s->gain[1][2]) + job->aux[1] * (s->gain[2][0] + s->gain[2][2]);
        gr = job->master * (s->gain[0][1] + s->gain[0][3]) +
             job->aux[0] * (s->gain[1][1] + s->gain[1][3]) + job->aux[1] * (s->gain[2][1] + s->gain[2][3]);
        for (unsigned n = 0; n < DSP_FRAME_SAMPLES; ++n) {
            float p; uint32_t i0; int32_t vl, vr;
            while (!s->sample_count && s->enabled)
                if (!dsp_worker_dequeue(i, s, job)) {
                    s->enabled = 0; s->buffer_update = 1; s->last_buffer_id = s->current_buffer_id;
                    s->current_buffer_id = 0;
                }
            if (!s->enabled) break;
            p = (float)s->pos + s->frac; i0 = (uint32_t)p;
            if (i0 + 1u < s->sample_count) {
                float f = p - (float)i0;
                int16_t sl0 = dsp_worker_fetch(s, i0, 0), sl1 = dsp_worker_fetch(s, i0 + 1u, 0);
                vl = (int32_t)(sl0 + (sl1 - sl0) * f);
                if (s->channels > 1u) {
                    int16_t sr0 = dsp_worker_fetch(s, i0, 1), sr1 = dsp_worker_fetch(s, i0 + 1u, 1);
                    vr = (int32_t)(sr0 + (sr1 - sr0) * f);
                } else vr = vl;
            } else {
                vl = dsp_worker_fetch(s, i0, 0); vr = (s->channels > 1u) ? dsp_worker_fetch(s, i0, 1) : vl;
            }
            job->mix_l[n] += (int32_t)((double)vl * (double)gl);
            job->mix_r[n] += (int32_t)((double)vr * (double)gr);
            s->frac += s->rate;
            while (s->frac >= 1.0f) {
                s->frac -= 1.0f; s->pos++;
                if (s->pos >= s->sample_count) {
                    /* Count each played buffer/loop boundary, even when the
                     * source continues. Azahar publishes SourceStatus before
                     * signaling its per-frame audio-pipe interrupt, so these
                     * events are visible to the guest refill callback. */
                    job->sources_finished++;
                    if (s->looping) {
                        s->pos = 0;
                        /* Azahar re-dequeues a looping buffer on wrap: the
                         * ADPCM decode continues from the carried predictor
                         * history unless the descriptor's adpcm_dirty bit
                         * reloads it. Re-decode instead of reusing the first
                         * pass PCM so the seam matches the reference. */
                        if (s->format == 2u && s->loop_buf_valid) {
                            int16_t *red;
                            if (s->loop_buf.adpcm_dirty) {
                                s->adpcm_yn1 = s->loop_buf.yn[0];
                                s->adpcm_yn2 = s->loop_buf.yn[1];
                                ++dsp_adpcm_hist_reloads;
                            } else ++dsp_adpcm_hist_carries;
                            red = dsp_decode_adpcm(&s->loop_buf, &s->adpcm_yn1, &s->adpcm_yn2);
                            if (red) {
                                free(s->decoded); s->decoded = red;
                                ++dsp_adpcm_loop_redecodes;
                            }
                        }
                    } else {
                        dsp_worker_source_free_current(s);
                        if (!dsp_worker_dequeue(i, s, job)) {
                            s->enabled = 0; s->buffer_update = 1; s->last_buffer_id = s->current_buffer_id;
                            s->current_buffer_id = 0;
                        }
                        break;
                    }
                }
            }
        }
    }
    job->result[i].enabled = (uint8_t)s->enabled;
    job->result[i].buffer_update = (uint8_t)s->buffer_update;
    job->result[i].sync_count = s->sync_count; job->result[i].pos = s->pos;
    job->result[i].current_buffer_id = s->current_buffer_id;
    job->result[i].last_buffer_id = s->last_buffer_id;
    s->buffer_update = 0;
}
static void dsp_process_music(DspAudioJob *job) {
    memset(job->mix_l, 0, sizeof(job->mix_l)); memset(job->mix_r, 0, sizeof(job->mix_r));
    dsp_process_source(DSP_MUSIC_SOURCE, &dsp_music_state[DSP_MUSIC_SOURCE],
                       &job->source[DSP_MUSIC_SOURCE], job);
}
static void dsp_process_sfx(DspAudioJob *job, const DspAudioJob *music) {
    memset(job->mix_l, 0, sizeof(job->mix_l)); memset(job->mix_r, 0, sizeof(job->mix_r));
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) if (i != DSP_MUSIC_SOURCE)
        dsp_process_source(i, &dsp_sfx_state[i], &job->source[i], job);
    memcpy(&job->result[DSP_MUSIC_SOURCE], &music->result[DSP_MUSIC_SOURCE], sizeof(DspSourceResult));
    job->sources_started += music->sources_started; job->sources_finished += music->sources_finished;
    for (unsigned n = 0; n < DSP_FRAME_SAMPLES; ++n) {
        int32_t l = job->mix_l[n] + music->mix_l[n], r = job->mix_r[n] + music->mix_r[n];
        if (l > 32767) { l = 32767; ++dsp_audio_clip_samples; }
        else if (l < -32768) { l = -32768; ++dsp_audio_clip_samples; }
        if (r > 32767) { r = 32767; ++dsp_audio_clip_samples; }
        else if (r < -32768) { r = -32768; ++dsp_audio_clip_samples; }
        job->final_pcm[n * 2u] = (int16_t)l; job->final_pcm[n * 2u + 1u] = (int16_t)r;
    }
}
static void dsp_job_free(DspAudioJob *job) {
    if (!job) return;
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        DspWorkerInput *in = &job->source[i];
        free(in->pending); /* borrowed snapshots do not own payload tokens */
    }
    free(job);
}
static int dsp_latency_compare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void dsp_audio_record_latency(uint64_t value) {
    if (dsp_audio_latency_count < DSP_LATENCY_SAMPLES)
        dsp_audio_latency[dsp_audio_latency_count++] = value;
}
static void *dsp_music_worker_main(void *unused) {
    (void)unused; dsp_set_worker_affinity(3u, "music", &dsp_music_affinity_ok);
    for (;;) {
        uintptr_t token; DspAudioJob *job;
        if (!dsp_pop_music_in(&token)) { dsp_sleep_empty(); continue; }
        if (!token) break;
        dsp_audio_test_delay();
        job = (DspAudioJob *)token; dsp_process_music(job);
        dsp_push_mixer((uintptr_t)job);
    }
    dsp_worker_states_free(dsp_music_state);
    return NULL;
}
static void *dsp_sfx_worker_main(void *unused) {
    (void)unused; dsp_set_worker_affinity(4u, "sfx/mixer", &dsp_sfx_affinity_ok);
    for (;;) {
        uintptr_t token; DspAudioJob *job, *music = NULL;
        if (!dsp_pop_sfx_in(&token)) { dsp_sleep_empty(); continue; }
        if (!token) break;
        dsp_audio_test_delay();
        job = (DspAudioJob *)token;
        while (!music) {
            uintptr_t music_token;
            if (dsp_pop_mixer(&music_token)) music = (DspAudioJob *)music_token;
            else dsp_sleep_empty();
        }
        if (music->sequence != job->sequence) {
            fprintf(stderr, "dsp audio worker sequence mismatch: music=%llu sfx=%llu\n",
                    (unsigned long long)music->sequence, (unsigned long long)job->sequence);
            exit(30);
        }
        dsp_process_sfx(job, music);
        dsp_job_free(music);
        {
            uint64_t queued_ahead_ns = dsp_audio_out_push(job->final_pcm);
            uint64_t now = host_monotonic_ns();
            job->feed_to_sink_ns = now - job->submitted_ns + queued_ahead_ns;
            if ((int64_t)(now - job->deadline_ns) > 0) ++dsp_audio_deadline_misses;
            dsp_audio_record_latency(job->feed_to_sink_ns);
        }
        dsp_push_completions((uintptr_t)job);
    }
    dsp_worker_states_free(dsp_sfx_state);
    dsp_push_completions(0u);
    return NULL;
}
/* Design section 3.2: drain completions from inside every shutdown
 * push-retry loop. A full dsp_completions queue otherwise deadlocks the
 * sentinel handshake (pre-existing exit-hang hazard, an infinite spin
 * before B3e, an infinite cond_wait after it). Pops go through the wrapper,
 * so each freed slot signals comp_nf and releases a W5 waiter. Returns 1
 * once the SFX worker's exit sentinel (token 0) has been observed, so the
 * post-push drain loop stays correct even if an interleaved drain consumed
 * it. Deliberately does NOT drain the mixer queue: the sfx worker may be
 * parked in W4 waiting for exactly the music token such a drain would free;
 * mixer drain stays post-pthread_join as before. */
static int dsp_shutdown_drain_completions(int *completed) {
    uintptr_t token;
    while (dsp_pop_completions_try(&token)) {
        if (!token) { *completed = 1; continue; }
        DspAudioJob *job = (DspAudioJob *)token;
        ++dsp_frames_out;
        dsp_sources_started += job->sources_started; dsp_sources_finished += job->sources_finished;
        dsp_job_free(job);
    }
    return *completed;
}
static void dsp_audio_workers_shutdown(void) {
    int completed = 0;
    if (!dsp_workers_started) { dsp_free_guest_sources(); return; }
    if (dsp_pending_music_job) {
        while (!dsp_push_music_in_try((uintptr_t)dsp_pending_music_job)) {
            dsp_shutdown_drain_completions(&completed);
            dsp_sleep_brief();
        }
        dsp_audio_transfer_shadow(1); dsp_pending_music_job = NULL;
    }
    if (dsp_pending_sfx_job) {
        while (!dsp_push_sfx_in_try((uintptr_t)dsp_pending_sfx_job)) {
            dsp_shutdown_drain_completions(&completed);
            dsp_sleep_brief();
        }
        dsp_audio_transfer_shadow(0); dsp_pending_sfx_job = NULL;
    }
    while (!dsp_push_music_in_try(0u)) {
        dsp_shutdown_drain_completions(&completed);
        dsp_sleep_brief();
    }
    while (!dsp_push_sfx_in_try(0u)) {
        dsp_shutdown_drain_completions(&completed);
        dsp_sleep_brief();
    }
    while (!completed) {
        if (!dsp_shutdown_drain_completions(&completed)) dsp_sleep_brief();
    }
    pthread_join(dsp_music_thread, NULL); pthread_join(dsp_sfx_thread, NULL);
    /* Post-join: single-threaded, workers are gone — raw try_pop is the only
     * correct form here (the blocking wrapper would hang on an empty queue). */
    {
        uintptr_t token;
        while (dq8_spsc_try_pop(&dsp_music_to_mixer, &token)) dsp_job_free((DspAudioJob *)token);
    }
    dsp_job_free(dsp_pending_music_job); dsp_job_free(dsp_pending_sfx_job);
    dsp_pending_music_job = dsp_pending_sfx_job = NULL;
    dsp_free_guest_sources();
#ifdef DQ8_ENABLE_SDL
    if (dsp_sdl_dev) { SDL_ClearQueuedAudio(dsp_sdl_dev); SDL_CloseAudioDevice(dsp_sdl_dev); dsp_sdl_dev = 0; }
#endif
    dq8_worker_scaffold_deactivate(&dsp_audio_worker_gate);
    dsp_workers_started = 0;
    if (dsp_audio_latency_count) {
        uint64_t sorted[DSP_LATENCY_SAMPLES];
        memcpy(sorted, dsp_audio_latency, dsp_audio_latency_count * sizeof(uint64_t));
        qsort(sorted, dsp_audio_latency_count, sizeof(uint64_t), dsp_latency_compare);
        uint32_t p95 = (dsp_audio_latency_count * 95u + 99u) / 100u;
        if (p95) --p95;
        fprintf(stderr, "dsp audio metrics: frames=%llu source_buffers_started=%llu source_finish_events=%llu feed_to_play_p95_est_ns=%llu deadline_misses=%llu sink_empty=%llu active_audio_gaps=%llu sink_checks=%llu sink_drops=%llu clip_samples=%llu adpcm_hist_reloads=%llu adpcm_hist_carries=%llu adpcm_loop_redecodes=%llu music_core=%s sfx_core=%s\n",
                (unsigned long long)dsp_frames_out,
                (unsigned long long)dsp_sources_started,
                (unsigned long long)dsp_sources_finished,
                (unsigned long long)sorted[p95],
                (unsigned long long)dsp_audio_deadline_misses,
                (unsigned long long)dsp_audio_sink_empty,
                (unsigned long long)dsp_audio_content_gaps,
                (unsigned long long)dsp_audio_sink_checks,
                (unsigned long long)dsp_audio_sink_drops,
                (unsigned long long)dsp_audio_clip_samples,
                (unsigned long long)dsp_adpcm_hist_reloads,
                (unsigned long long)dsp_adpcm_hist_carries,
                (unsigned long long)dsp_adpcm_loop_redecodes,
                dsp_music_affinity_ok ? "3" : "unavailable",
                dsp_sfx_affinity_ok ? "4" : "unavailable");
    }
}
static void dsp_audio_workers_start(void) {
    if (dsp_workers_started) return;
    if (!dq8_spsc_init(&dsp_music_input, dsp_music_input_slots, DSP_QUEUE_CAPACITY) ||
        !dq8_spsc_init(&dsp_sfx_input, dsp_sfx_input_slots, DSP_QUEUE_CAPACITY) ||
        !dq8_spsc_init(&dsp_music_to_mixer, dsp_music_to_mixer_slots, DSP_QUEUE_CAPACITY) ||
        !dq8_spsc_init(&dsp_completions, dsp_completion_slots, DSP_COMPLETION_CAPACITY)) {
        fputs("dsp audio: SPSC queue initialization failed\n", stderr); exit(29);
    }
    dq8_worker_scaffold_init(&dsp_audio_worker_gate);
    dsp_empty_poll_configure();
    dsp_wake_configure();
    if (pthread_create(&dsp_music_thread, NULL, dsp_music_worker_main, NULL) != 0) {
        fputs("dsp audio: cannot create music worker\n", stderr); exit(29);
    }
    if (pthread_create(&dsp_sfx_thread, NULL, dsp_sfx_worker_main, NULL) != 0) {
        while (!dsp_push_music_in_try(0u)) dsp_sleep_brief();
        pthread_join(dsp_music_thread, NULL);
        fputs("dsp audio: cannot create SFX worker\n", stderr); exit(29);
    }
    dsp_workers_started = 1;
    if (!dq8_worker_scaffold_activate(&dsp_audio_worker_gate)) {
        fputs("dsp audio: worker gate activation failed\n", stderr); exit(29);
    }
    atexit(dsp_audio_workers_shutdown);
}

static int dsp_audio_parse_frame(uint32_t *read_out, uint32_t *write_out) {
    uint32_t read = dsp_current_region(), write = (read == DSP_REGION0) ? DSP_REGION1 : DSP_REGION0;
    uint32_t dcfg = read + DSP_OFF_APP_DSP_CONFIG, ddirty = dsp_r32(dcfg);
    if (ddirty) {
        if (ddirty & (1u << 16)) dsp_master_volume = dsp_rfloat(dcfg + 4u);
        if (ddirty & (1u << 24)) dsp_aux_volume[0] = dsp_rfloat(dcfg + 8u);
        if (ddirty & (1u << 25)) dsp_aux_volume[1] = dsp_rfloat(dcfg + 12u);
        dsp_w32(dcfg, 0);
    }
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        /* Only the current read region's configs are live: the guest fully
         * prepares a region (including the source format field at +180)
         * before flipping its frame counter. Scanning both regions eagerly
         * read half-written configs (flags1=0 -> PCM8) and played ADPCM
         * stream buffers as noise. Azahar parses ReadRegion() only
         * (hle.cpp GenerateCurrentFrame -> Source::Tick(read.config[i])). */
        if (!dsp_parse_source_config(i, read + DSP_OFF_SRC_CONFIG + i * 192u)) return 0;
    }
    *read_out = read; *write_out = write;
    return 1;
}
static DspAudioJob *dsp_audio_make_job(uint64_t sequence, uint64_t deadline, uint32_t read, uint32_t write, int music) {
    DspAudioJob *job = (DspAudioJob *)calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->sequence = sequence; job->submitted_ns = host_monotonic_ns(); job->deadline_ns = deadline + DSP_FRAME_NS;
    job->read_region = read; job->write_region = write; job->notify_guest = 1;
    job->master = dsp_master_volume; job->aux[0] = dsp_aux_volume[0]; job->aux[1] = dsp_aux_volume[1];
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        if ((i == DSP_MUSIC_SOURCE) != music) continue;
        DspGuestSource *src = &dsp_guest_src[i]; DspWorkerInput *dst = &job->source[i];
        dst->enabled = src->enabled; dst->rate = src->rate; dst->sync_count = src->sync_count;
        dst->reset = src->reset; dst->clear_pending = src->clear_pending;
        dst->enabled_dirty = src->enabled_dirty; dst->rate_dirty = src->rate_dirty;
        dst->gain_dirty = src->gain_dirty; dst->sync_dirty = src->sync_dirty;
        memcpy(dst->gain, src->gain, sizeof(dst->gain));
        dst->pending_count = src->pending_count;
        if (dst->pending_count) {
            dst->pending = (DspPending *)malloc(dst->pending_count * sizeof(DspPending));
            if (!dst->pending) { dsp_job_free(job); return NULL; }
            memcpy(dst->pending, src->pending, dst->pending_count * sizeof(DspPending));
        }
    }
    return job;
}
static void dsp_audio_transfer_shadow(int music) {
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        if ((i == DSP_MUSIC_SOURCE) != music) continue;
        DspGuestSource *src = &dsp_guest_src[i];
        for (unsigned k = 0; k < src->pending_count; ++k) src->pending[k].payload = NULL;
        src->pending_count = 0; src->reset = 0; src->clear_pending = 0;
        src->enabled_dirty = src->rate_dirty = src->gain_dirty = src->sync_dirty = 0;
    }
}
static int dsp_audio_submit(uint64_t deadline) {
    if (!dsp_pending_music_job && !dsp_pending_sfx_job) {
        uint32_t read, write;
        if (!dsp_audio_parse_frame(&read, &write)) return 0;
        dsp_pending_music_job = dsp_audio_make_job(dsp_audio_sequence, deadline, read, write, 1);
        dsp_pending_sfx_job = dsp_audio_make_job(dsp_audio_sequence, deadline, read, write, 0);
        if (!dsp_pending_music_job || !dsp_pending_sfx_job) {
            dsp_job_free(dsp_pending_music_job); dsp_job_free(dsp_pending_sfx_job);
            dsp_pending_music_job = dsp_pending_sfx_job = NULL;
            fputs("dsp audio: cannot allocate frame snapshot\n", stderr); return 0;
        }
    }
    if (dsp_pending_music_job && dsp_push_music_in_try((uintptr_t)dsp_pending_music_job)) {
        dsp_audio_transfer_shadow(1); dsp_pending_music_job = NULL;
    }
    if (dsp_pending_sfx_job && dsp_push_sfx_in_try((uintptr_t)dsp_pending_sfx_job)) {
        dsp_audio_transfer_shadow(0); dsp_pending_sfx_job = NULL;
    }
    if (dsp_pending_music_job || dsp_pending_sfx_job) return 0;
    ++dsp_audio_sequence;
    return 1;
}
static void dsp_audio_commit(DspAudioJob *job) {
    /* The HLE reference chooses the double-buffer region at each DSP tick.
     * Worker latency lets the guest advance its frame counters after submit,
     * so choose the matching output region when this frame is actually
     * committed instead of reusing a stale submission-time region. */
    uint32_t read = dsp_current_region();
    uint32_t write = (read == DSP_REGION0) ? DSP_REGION1 : DSP_REGION0;
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        uint32_t st = write + DSP_OFF_SRC_STATUS + i * 12u;
        DspSourceResult *s = &job->result[i];
        ram[st] = s->enabled; ram[st + 1u] = s->buffer_update;
        dsp_w16(st + 2u, s->sync_count); dsp_w32dsp(st + 4u, s->pos);
        dsp_w16(st + 8u, s->current_buffer_id); dsp_w16(st + 10u, s->last_buffer_id);
    }
    for (unsigned n = 0; n < DSP_FRAME_SAMPLES; ++n) {
        dsp_w16(write + DSP_OFF_FINAL + n * 4u, (uint16_t)job->final_pcm[n * 2u]);
        dsp_w16(write + DSP_OFF_FINAL + n * 4u + 2u, (uint16_t)job->final_pcm[n * 2u + 1u]);
    }
    dsp_w16(write + DSP_OFF_STATUS, 0); dsp_w16(write + DSP_OFF_STATUS + 2u, 0);
    ++dsp_frames_out;
    dsp_sources_started += job->sources_started; dsp_sources_finished += job->sources_finished;

    /* SourceStatus is fully committed above before the Azahar-compatible
     * audio-pipe interrupt wakes guest audio/refill callbacks. */
    if (job->notify_guest && dsp_pipe_interrupt_event && dsp_pipe_interrupt_event < next_event_handle &&
        event_live[dsp_pipe_interrupt_event]) {
        if (job->sources_started || job->sources_finished)
            dsp_source_event_pending = 1;
        event_signaled[dsp_pipe_interrupt_event] = 1u;
        guest_notify_handle(dsp_pipe_interrupt_event);
    }
    if (getenv("DQ8_TRACE_AUDIO") && (dsp_frames_out % (DSP_RATE / 4u)) == 0u)
        fprintf(stderr, "dsp audio: frames=%llu started=%llu finished=%llu on=%d\n",
                (unsigned long long)dsp_frames_out, (unsigned long long)dsp_sources_started,
                (unsigned long long)dsp_sources_finished, dsp_audio_on);
    dsp_job_free(job);
}
/* Do not overwrite a one-frame SourceStatus.current_buffer_id_dirty update
 * with later completed frames before the guest has had a scheduler turn to
 * service the Pipe IRQ. The caller invokes this before choosing a runnable
 * guest thread, so returning at the first source update exposes that status. */
static unsigned dsp_audio_commit_ready(void) {
    uintptr_t completed;
    unsigned committed = 0;
    if (dsp_source_event_pending) return 0;
    while (dsp_pop_completions_try(&completed)) {
        DspAudioJob *job = (DspAudioJob *)completed;
        int source_status_update = job->sources_started != 0 || job->sources_finished != 0;
        dsp_audio_commit(job);
        ++committed;
        if (source_status_update) break;
    }
    return committed;
}
static void dsp_audio_tick(void) {
    uint64_t now = host_monotonic_ns();
    unsigned caught = 0;
    /* IDLE THROTTLE: while armed the only audio is silence (a live source
     * keeps the throttle disarmed), so the quantum cadence is stretched by
     * idle_stretch_wait (~204/s -> the idle poll rate).  On idle release the
     * deadline resyncs so music that starts with the state is serviced on
     * the very next quantum, not after the stale stretched deadline. */
    static int dsp_idle_armed_prev;
    int idle_armed_now = idle_throttle_armed();
    if (dsp_idle_armed_prev && !idle_armed_now && dsp_next_frame_ns)
        dsp_next_frame_ns = now;
    dsp_idle_armed_prev = idle_armed_now;
    (void)dsp_audio_commit_ready();
    if (dsp_source_event_pending) return;
    if (!dsp_audio_on) return;
    if (!dsp_workers_started) { dsp_audio_out_init(); dsp_audio_workers_start(); }
    if (!dq8_worker_scaffold_is_active(&dsp_audio_worker_gate)) return;
    if (!dsp_next_frame_ns) dsp_next_frame_ns = now;
    uint64_t quantum = idle_stretch_wait(DSP_FRAME_NS);
    while ((int64_t)(now - dsp_next_frame_ns) >= 0 && caught < 8u) {
        if (!dsp_audio_submit(dsp_next_frame_ns)) break;
        dsp_next_frame_ns += quantum; ++caught;
    }
    if (caught >= 8u && (int64_t)(now - dsp_next_frame_ns) >= 0) {
        dsp_next_frame_ns = now + quantum;
        ++perf_dsp_resyncs;
    }
    if (caught) ++perf_dsp_ticks;
}
