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
/* Queue depths are sized so a main-loop stall never stalls the sink: they hold
 * the frames the pacer produces while the guest loop is away (DSP_QUEUE_CAPACITY
 * * 4.888 ms = 156 ms of input backlog per worker, DSP_COMPLETION_CAPACITY *
 * 4.888 ms = 1.25 s of uncommitted completion results). */
#define DSP_QUEUE_CAPACITY 32u
#define DSP_COMPLETION_CAPACITY 256u
#define DSP_LATENCY_SAMPLES 65536u
#define DSP_MUSIC_SOURCE 1u
/* DSP pacer (t_d808299b): frames are produced on their own wall-clock thread,
 * never gated by the guest/main loop.  Prefill = sink lead after (re)start. */
#define DSP_PREFILL_FRAMES 8u
/* Sink lead repair (t_37438ae5): how many frames the pacer may produce in one
 * catch-up burst to rebuild the device queue after a stall. */
#define DSP_SINK_MAX_CATCHUP 24u
#define DSP_PACER_CORE 5u
#define DSP_TEMPLATE_CAPACITY 16u
#define DSP_TRACE_FRAMES 204u        /* ~1 s of DSP frames between trace lines */

static _Atomic int dsp_audio_on;     /* written by the guest service path */
static _Atomic uint64_t dsp_next_frame_ns; /* pacer cadence, main-loop wake hint */
static uint64_t dsp_frames_out;
static uint64_t dsp_sources_started, dsp_sources_finished;
/* Guest DSP state revision.  Every dirty config word the parse consumes and
 * every buffer it snapshots bumps the counter; the harvest only builds a new
 * template when the revision moved, so the common tick is a single compare. */
static uint64_t dsp_guest_progress, dsp_guest_harvested;
static float dsp_master_volume, dsp_aux_volume[2];
static int guest_memory_range(uint32_t, uint64_t, uint32_t *);
/* Defined with the DSP pacer section; shutdown joins it before the worker
 * sentinels go out so the pacer can never push into a dead queue. */
static void dsp_pacer_stop_and_join(void);

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
    ++dsp_guest_progress;
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
    ++dsp_guest_progress;
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
/* t_37438ae5 crackle instrumentation.  DQ8_TRACE_AUDIO_Q=1 emits one compact
 * line per mixed quantum (AQ) plus an ASTARVE line whenever an enabled source
 * runs out of guest buffers mid-quantum (guest-side starvation), so a click
 * can be attributed to the sink (queue depth) or to the source (refill late).
 * DQ8_TRACE_AUDIO_QLOW sets the "sink low" threshold in microseconds. */
static int dq8_trace_audio_q;
static uint64_t dsp_src_starve_total;      /* enabled source ran dry mid-frame */
static uint64_t dsp_src_starve[24];        /* per source */
static uint64_t dsp_sink_low_total;        /* quanta submitted with < QLOW lead */
static uint64_t dsp_sink_low_thresh_us = 2000u;
/* t_37438ae5 sink hardening.  The device queue only ever held the 8-frame
 * (39 ms) start prefill: one 100-200 ms process stall drained it and NOTHING
 * rebuilt the lead afterwards, so from then on every scheduler hiccup cut the
 * device queue -> the crackle the operator hears while the game runs slowly.
 * The sink now (a) publishes its queue depth, (b) asks the pacer for a lead
 * refill when it dips under the target, (c) re-arms the prefill after a real
 * underrun, and (d) clamps latency at the ceiling. */
static _Atomic uint64_t dsp_sink_ahead_ns;     /* last published device lead */
static _Atomic int dsp_sink_reseed;            /* underrun: rebuild the lead */
static uint64_t dsp_sink_underruns;            /* pushes that found an empty device queue */
static uint64_t dsp_sink_reseeds;              /* prefill re-arms after an underrun */
static uint64_t dsp_sink_lead_refills;         /* pacer catch-up bursts */
static uint64_t dsp_sink_lead_holds;           /* production pauses (lead overshoot) */
static uint64_t dsp_sink_target_ns = 60000000ull;   /* 60 ms lead */
static uint64_t dsp_sink_ceiling_ns = 250000000ull; /* 250 ms max queued */
static uint32_t dsp_sink_target_frames = 12u;       /* target in DSP quanta */
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
    {
        const char *q = getenv("DQ8_TRACE_AUDIO_Q");
        const char *qlow = getenv("DQ8_TRACE_AUDIO_QLOW");
        const char *tgt = getenv("DQ8_AUDIO_SINK_TARGET_MS");
        const char *ceil = getenv("DQ8_AUDIO_SINK_CEILING_MS");
        dq8_trace_audio_q = q && q[0] && q[0] != '0';
        if (qlow && qlow[0]) dsp_sink_low_thresh_us = strtoull(qlow, NULL, 10);
        if (tgt && tgt[0]) dsp_sink_target_ns = strtoull(tgt, NULL, 10) * 1000000ull;
        if (ceil && ceil[0]) dsp_sink_ceiling_ns = strtoull(ceil, NULL, 10) * 1000000ull;
        if (dsp_sink_target_ns / DSP_FRAME_NS > 32u) dsp_sink_target_ns = 32u * DSP_FRAME_NS;
        dsp_sink_target_frames = (uint32_t)(dsp_sink_target_ns / DSP_FRAME_NS);
        if (dsp_sink_target_frames < 4u) dsp_sink_target_frames = 4u;
        if (dsp_sink_target_frames > 32u) dsp_sink_target_frames = 32u;
        fprintf(stderr, "host audio: sink lead target %llu us (%u frames), ceiling %llu us\n",
                (unsigned long long)(dsp_sink_target_ns / 1000u), dsp_sink_target_frames,
                (unsigned long long)(dsp_sink_ceiling_ns / 1000u));
        if (dq8_trace_audio_q)
            fprintf(stderr, "host audio: per-quantum trace on (sink-low threshold %llu us)\n",
                    (unsigned long long)dsp_sink_low_thresh_us);
    }
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
        atomic_store_explicit(&dsp_sink_ahead_ns, queue_ahead_ns, memory_order_relaxed);
        /* A late refill is not an underrun. Track an empty SDL queue separately;
         * count a content gap only between adjacent non-silent output frames. */
        if (dsp_audio_sdl_has_queued_audio) {
            ++dsp_audio_sink_checks;
            if (queued == 0u) {
                ++dsp_audio_sink_empty;
                ++dsp_sink_underruns;
                /* The device ran dry: arm a prefill so the pacer rebuilds the
                 * lead instead of leaving the sink permanently on the edge. */
                atomic_store_explicit(&dsp_sink_reseed, 1, memory_order_relaxed);
                if (dsp_audio_previous_frame_active && frame_active) ++dsp_audio_content_gaps;
            }
        }
        if (queue_ahead_ns <= dsp_sink_ceiling_ns) {
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
    /* Instrumentation (t_37438ae5): wall time of the last successful dequeue,
     * so a starvation event can report how long the source went without a
     * guest refill. */
    uint64_t last_dequeue_ns;
    /* Hold-last-good (t_37438ae5): when a source has no next buffer (guest
     * refill late, or a clip ends mid-waveform) the mixer holds the last
     * sample and fades it out over one quantum, then fades the next buffer in,
     * instead of cutting to zero — the cut is the click. */
    float fade_out, fade_in;
    int fade_in_active, fade_init;
    int16_t hold_l, hold_r;
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
        s->fade_out = 0.0f; s->fade_in = 1.0f; s->fade_in_active = 0;
        s->hold_l = s->hold_r = 0;
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
    s->last_dequeue_ns = host_monotonic_ns();
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
    if (!s->fade_init) { s->fade_init = 1; s->fade_in = 1.0f; }
    dsp_worker_apply_input(s, in, job);
    if (s->enabled) {
        gl = job->master * (s->gain[0][0] + s->gain[0][2]) +
             job->aux[0] * (s->gain[1][0] + s->gain[1][2]) + job->aux[1] * (s->gain[2][0] + s->gain[2][2]);
        gr = job->master * (s->gain[0][1] + s->gain[0][3]) +
             job->aux[0] * (s->gain[1][1] + s->gain[1][3]) + job->aux[1] * (s->gain[2][1] + s->gain[2][3]);
        for (unsigned n = 0; n < DSP_FRAME_SAMPLES; ++n) {
            float p; uint32_t i0; int32_t vl, vr;
            while (s->fade_out <= 0.0f && !s->sample_count && s->enabled)
                if (!dsp_worker_dequeue(i, s, job)) {
                    /* Guest-side starvation: the guest did not post the next
                     * buffer before this quantum needed it (or the clip ended
                     * mid-waveform).  Hold the last good sample and fade it to
                     * zero over this quantum instead of cutting to silence. */
                    uint64_t starve_now = host_monotonic_ns();
                    uint64_t gap_us = s->last_dequeue_ns && starve_now > s->last_dequeue_ns
                                    ? (starve_now - s->last_dequeue_ns) / 1000u : 0u;
                    ++dsp_src_starve_total;
                    if (i < 24u) ++dsp_src_starve[i];
                    if (dq8_trace_audio_q)
                        fprintf(stderr, "ASTARVE t_us=%llu seq=%llu src=%u gap_us=%llu pos=%u rate=%.4f\n",
                                (unsigned long long)(starve_now / 1000ull),
                                (unsigned long long)job->sequence, i,
                                (unsigned long long)gap_us, s->pos, (double)s->rate);
                    s->enabled = 0; s->buffer_update = 1; s->last_buffer_id = s->current_buffer_id;
                    s->current_buffer_id = 0;
                    if (s->hold_l || s->hold_r) s->fade_out = 1.0f;
                }
            if (s->fade_out > 0.0f) {
                int32_t hl = (int32_t)((float)s->hold_l * s->fade_out);
                int32_t hr = (int32_t)((float)s->hold_r * s->fade_out);
                job->mix_l[n] += (int32_t)((double)hl * (double)gl);
                job->mix_r[n] += (int32_t)((double)hr * (double)gr);
                s->fade_out -= 1.0f / (float)DSP_FRAME_SAMPLES;
                if (s->fade_out <= 0.0f) {
                    s->fade_out = 0.0f; s->hold_l = s->hold_r = 0;
                    s->fade_in = 0.0f; s->fade_in_active = 1;
                }
                continue;
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
            s->hold_l = (int16_t)vl; s->hold_r = (int16_t)vr;
            if (s->fade_in_active) {
                vl = (int32_t)((float)vl * s->fade_in); vr = (int32_t)((float)vr * s->fade_in);
                s->fade_in += 1.0f / (float)DSP_FRAME_SAMPLES;
                if (s->fade_in >= 1.0f) { s->fade_in = 1.0f; s->fade_in_active = 0; }
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
                            /* Clip ended with no successor: fade the last
                             * sample out over the rest of this quantum. */
                            if (s->hold_l || s->hold_r) s->fade_out = 1.0f;
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
            if (queued_ahead_ns / 1000u < dsp_sink_low_thresh_us) ++dsp_sink_low_total;
            if (dq8_trace_audio_q) {
                unsigned pend_smax = 0, act = 0, i;
                for (i = 0; i < DSP_NUM_SOURCES; ++i) {
                    const DspWorkerSource *s = i == DSP_MUSIC_SOURCE
                        ? &dsp_music_state[DSP_MUSIC_SOURCE] : &dsp_sfx_state[i];
                    if (s->enabled) ++act;
                    if (i != DSP_MUSIC_SOURCE && s->pending_count > pend_smax)
                        pend_smax = s->pending_count;
                }
                fprintf(stderr, "AQ t_us=%llu seq=%llu ahead_us=%llu act=%u pend_m=%u pend_smax=%u "
                        "late_us=%lld starve=%llu low=%llu\n",
                        (unsigned long long)(now / 1000ull),
                        (unsigned long long)job->sequence,
                        (unsigned long long)(queued_ahead_ns / 1000u), act,
                        dsp_music_state[DSP_MUSIC_SOURCE].pending_count, pend_smax,
                        (long long)((int64_t)(now - job->deadline_ns) / 1000),
                        (unsigned long long)dsp_src_starve_total,
                        (unsigned long long)dsp_sink_low_total);
            }
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
    if (!dsp_workers_started) { dsp_pacer_stop_and_join(); dsp_free_guest_sources(); return; }
    dsp_pacer_stop_and_join();
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
/* ---------------------------------------------------------------------------
 * DSP PACER (t_d808299b): frame production runs on its own wall-clock thread.
 *
 * The main loop spends 100-200 ms inside the 3D/GL/present shims on every
 * spike frame.  While it was away the old tick could not submit DSP frames,
 * the sink drained, and the voice played in fragments separated by silence
 * (the operator's "the same line takes several times as long" report: the
 * content timeline slipped by every stall).  Production is therefore split in
 * two, by what each side is allowed to touch:
 *
 *   main thread  - harvest: parse the guest DSP regions, snapshot the delta
 *                  into owned DspAudioJob templates, publish the template.
 *                - commit : write finished frames back into guest RAM and
 *                  signal the audio-pipe interrupt (scheduler state).
 *   pacer thread - every DSP_FRAME_NS (4.888791 ms, 204.55 frames/s): clone
 *                  the newest template (with its deltas) or the previous one
 *                  as a *continuation* (no pending buffers, no dirty flags, so
 *                  the worker just mixes on from its own source state) and
 *                  push it into the worker SPSC queues.
 *
 * The pacer never reads or writes guest RAM: it only copies owned snapshots,
 * so a guest rewrite can never tear a sample and no lock is needed on `ram`.
 * The completion path back to the guest stays on the main thread, and its
 * queue is deep enough (DSP_COMPLETION_CAPACITY = 1.25 s) that a stalled main
 * loop never blocks the mixer.
 * ------------------------------------------------------------------------- */
typedef struct {
    DspAudioJob *music, *sfx;
} DspTemplate;

static pthread_mutex_t dsp_tpl_mu = PTHREAD_MUTEX_INITIALIZER;
static DspTemplate dsp_tpl_ring[DSP_TEMPLATE_CAPACITY];
static unsigned dsp_tpl_head, dsp_tpl_tail;      /* guarded by dsp_tpl_mu */
static DspAudioJob *dsp_tpl_last_music, *dsp_tpl_last_sfx; /* pacer-owned */
static int dsp_tpl_last_valid;
static int dsp_pacer_started, dsp_pacer_affinity_ok;
static _Atomic int dsp_pacer_stop;
static pthread_t dsp_pacer_thread;
static _Atomic uint64_t dsp_pacer_next_ns;       /* published cadence/wake hint */
static uint64_t dsp_pacer_t0_ns, dsp_pacer_t0_frames; /* pacing epoch (budget) */
static uint64_t dsp_harvest_last_ns;             /* harvest (parse) cadence */
static uint64_t dsp_pacer_frames;                /* frames emitted by the pacer */
static uint64_t dsp_pacer_drops;                 /* frames discarded (queues stayed full) */
static uint64_t dsp_pacer_prefills;

static void dsp_job_free_owned(DspAudioJob *job) {
    if (!job) return;
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        DspWorkerInput *in = &job->source[i];
        for (unsigned k = 0; k < in->pending_count; ++k) free(in->pending[k].payload);
        free(in->pending);
    }
    free(job);
}
/* Deep-copy one template job.  deltas == 0 builds a continuation frame: the
 * worker keeps every source register and its current position, only the
 * frame-level envelope (volumes, regions) is carried over. */
static DspAudioJob *dsp_job_clone(const DspAudioJob *tpl, uint64_t sequence,
                                  uint64_t deadline, int deltas) {
    DspAudioJob *job;
    if (!tpl) return NULL;
    job = (DspAudioJob *)calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->sequence = sequence;
    job->submitted_ns = host_monotonic_ns();
    job->deadline_ns = deadline + DSP_FRAME_NS;
    job->read_region = tpl->read_region;
    job->write_region = tpl->write_region;
    job->notify_guest = 1;
    job->master = tpl->master;
    job->aux[0] = tpl->aux[0]; job->aux[1] = tpl->aux[1];
    if (!deltas) return job;
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i) {
        const DspWorkerInput *src = &tpl->source[i];
        DspWorkerInput *dst = &job->source[i];
        dst->enabled = src->enabled; dst->reset = src->reset;
        dst->clear_pending = src->clear_pending;
        dst->enabled_dirty = src->enabled_dirty; dst->rate_dirty = src->rate_dirty;
        dst->gain_dirty = src->gain_dirty; dst->sync_dirty = src->sync_dirty;
        dst->rate = src->rate; dst->sync_count = src->sync_count;
        memcpy(dst->gain, src->gain, sizeof(dst->gain));
        dst->pending_count = src->pending_count;
        if (!src->pending_count) continue;
        dst->pending = (DspPending *)calloc(src->pending_count, sizeof(DspPending));
        if (!dst->pending) { dsp_job_free_owned(job); return NULL; }
        for (unsigned k = 0; k < src->pending_count; ++k) {
            dst->pending[k] = src->pending[k];
            dst->pending[k].payload = NULL;
            if (!src->pending[k].payload || !src->pending[k].payload_bytes) continue;
            dst->pending[k].payload = (uint8_t *)malloc(src->pending[k].payload_bytes);
            if (!dst->pending[k].payload) { dsp_job_free_owned(job); return NULL; }
            memcpy(dst->pending[k].payload, src->pending[k].payload,
                   src->pending[k].payload_bytes);
        }
    }
    return job;
}

/* Push one frame (music + SFX job at the same sequence).  Returns 1 when the
 * frame left the producer, 0 when a queue was full and the frame must be
 * retried on the next tick (the prepared jobs stay in dsp_pending_*_job). */
static int dsp_pacer_submit(const DspAudioJob *music_tpl, const DspAudioJob *sfx_tpl,
                            uint64_t deadline, int deltas) {
    if (!dsp_pending_music_job && !dsp_pending_sfx_job) {
        dsp_pending_music_job = dsp_job_clone(music_tpl, dsp_audio_sequence, deadline, deltas);
        dsp_pending_sfx_job = dsp_job_clone(sfx_tpl, dsp_audio_sequence, deadline, deltas);
        if (!dsp_pending_music_job || !dsp_pending_sfx_job) {
            dsp_job_free_owned(dsp_pending_music_job);
            dsp_job_free_owned(dsp_pending_sfx_job);
            dsp_pending_music_job = dsp_pending_sfx_job = NULL;
            return 1;   /* cannot snapshot a frame: skip it, keep pacing */
        }
    }
    if (dsp_pending_music_job && dsp_push_music_in_try((uintptr_t)dsp_pending_music_job))
        dsp_pending_music_job = NULL;
    if (dsp_pending_sfx_job && dsp_push_sfx_in_try((uintptr_t)dsp_pending_sfx_job))
        dsp_pending_sfx_job = NULL;
    if (dsp_pending_music_job || dsp_pending_sfx_job) return 0;
    ++dsp_audio_sequence;
    return 1;
}

/* Bootstrap template: the very first frames must still be produced and
 * committed, because committing them is what signals the audio-pipe interrupt
 * that makes the guest's audio worker post its first buffers. */
static int dsp_tpl_ensure_default(void) {
    if (dsp_tpl_last_valid) return 1;
    dsp_tpl_last_music = (DspAudioJob *)calloc(1, sizeof(DspAudioJob));
    dsp_tpl_last_sfx = (DspAudioJob *)calloc(1, sizeof(DspAudioJob));
    if (!dsp_tpl_last_music || !dsp_tpl_last_sfx) {
        free(dsp_tpl_last_music); free(dsp_tpl_last_sfx);
        dsp_tpl_last_music = dsp_tpl_last_sfx = NULL;
        return 0;
    }
    dsp_tpl_last_valid = 1;
    return 1;
}

/* One pacer frame: a new harvest replaces the continuation template, otherwise
 * the previous template is replayed without deltas. */
static int dsp_pacer_emit(uint64_t deadline) {
    DspAudioJob *music = NULL, *sfx = NULL;
    int have_new = 0, started = 0;
    pthread_mutex_lock(&dsp_tpl_mu);
    if (dsp_tpl_tail != dsp_tpl_head) {
        unsigned slot = dsp_tpl_tail % DSP_TEMPLATE_CAPACITY;
        music = dsp_tpl_ring[slot].music; sfx = dsp_tpl_ring[slot].sfx;
        dsp_tpl_ring[slot].music = dsp_tpl_ring[slot].sfx = NULL;
        dsp_tpl_tail++;
        have_new = 1;
    }
    pthread_mutex_unlock(&dsp_tpl_mu);

    if (have_new) {
        /* New guest state: this frame carries the deltas.  The template stays
         * alive as the continuation source once the frame is accepted. */
        started = dsp_pacer_submit(music, sfx, deadline, 1);
        if (started) {
            dsp_job_free_owned(dsp_tpl_last_music);
            dsp_job_free_owned(dsp_tpl_last_sfx);
            dsp_tpl_last_music = music; dsp_tpl_last_sfx = sfx;
            dsp_tpl_last_valid = 1;
            return 1;
        }
        /* Queue full: keep the deltas for the next tick by putting the
         * template back at the tail (it is still the newest harvest). */
        pthread_mutex_lock(&dsp_tpl_mu);
        dsp_tpl_tail--;
        {
            unsigned slot = dsp_tpl_tail % DSP_TEMPLATE_CAPACITY;
            dsp_tpl_ring[slot].music = music; dsp_tpl_ring[slot].sfx = sfx;
        }
        pthread_mutex_unlock(&dsp_tpl_mu);
        return 0;
    }
    if (!dsp_tpl_ensure_default()) return 0;
    return dsp_pacer_submit(dsp_tpl_last_music, dsp_tpl_last_sfx, deadline, 0);
}

static void dsp_audio_trace_line(uint64_t report_t0_ns, uint64_t sink_frames_at_t0) {
    uint64_t now = host_monotonic_ns();
    double secs = (double)(now - report_t0_ns) / 1e9;
    double rate = secs > 0.0 ? (double)(dsp_wav_frames - sink_frames_at_t0) / secs : 0.0;
    fprintf(stderr,
            "dsp audio: wall=%.3f sink_frames=%llu sink_rate=%.2f/s pacer_frames=%llu "
            "resyncs=%llu drops=%llu deadline_misses=%llu sink_empty=%llu content_gaps=%llu "
            "sink_drops=%llu commits=%llu prefill=%llu lag_us=%.0f src_starve=%llu sink_low=%llu "
            "sink_underruns=%llu reseeds=%llu lead_refills=%llu lead_holds=%llu\n",
            secs,
            (unsigned long long)dsp_wav_frames, rate,
            (unsigned long long)dsp_pacer_frames,
            (unsigned long long)perf_dsp_resyncs,
            (unsigned long long)dsp_pacer_drops,
            (unsigned long long)dsp_audio_deadline_misses,
            (unsigned long long)dsp_audio_sink_empty,
            (unsigned long long)dsp_audio_content_gaps,
            (unsigned long long)dsp_audio_sink_drops,
            (unsigned long long)dsp_frames_out,
            (unsigned long long)dsp_pacer_prefills,
            ((double)now - (double)atomic_load(&dsp_pacer_next_ns)) / 1000.0,
            (unsigned long long)dsp_src_starve_total,
            (unsigned long long)dsp_sink_low_total,
            (unsigned long long)dsp_sink_underruns,
            (unsigned long long)dsp_sink_reseeds,
            (unsigned long long)dsp_sink_lead_refills,
            (unsigned long long)dsp_sink_lead_holds);
}

static void *dsp_pacer_main(void *unused) {
    uint64_t next, report_t0, report_frames, report_sink;
    uint64_t last_lead_refill = 0, stall_at = 0, stall_ms = 0, stall_every_ns = 3000000000ull;
    int prefilled = 0;
    (void)unused;
    dsp_set_worker_affinity(DSP_PACER_CORE, "dsp-pacer", &dsp_pacer_affinity_ok);
    {
        /* Fault injection for the crackle test harness: park the pacer for
         * N ms every M ms, which is exactly what a 100-200 ms frame spike does
         * to the device queue when the whole process is descheduled. */
        const char *s = getenv("DQ8_AUDIO_PACER_STALL_MS");
        const char *e = getenv("DQ8_AUDIO_PACER_STALL_EVERY_MS");
        if (s && s[0]) stall_ms = strtoull(s, NULL, 10);
        if (e && e[0]) stall_every_ns = strtoull(e, NULL, 10) * 1000000ull;
        if (stall_ms) fprintf(stderr, "dsp pacer: fault injection stall=%llums every=%llums\n",
                              (unsigned long long)stall_ms,
                              (unsigned long long)(stall_every_ns / 1000000ull));
    }
    next = host_monotonic_ns();
    report_t0 = next; report_frames = 0; report_sink = 0;
    for (;;) {
        uint64_t now = host_monotonic_ns();
        unsigned caught = 0, burst_allow = 1;
        if (atomic_load_explicit(&dsp_pacer_stop, memory_order_relaxed)) break;
        if (stall_ms && now - stall_at >= stall_every_ns) {
            struct timespec ts = {(time_t)(stall_ms / 1000ull),
                                  (long)((stall_ms % 1000ull) * 1000000ull)};
            stall_at = now;
            nanosleep(&ts, NULL);   /* test-only: emulate a 100-200 ms spike */
            now = host_monotonic_ns();
        }
        if (!atomic_load_explicit(&dsp_audio_on, memory_order_relaxed)) {
            prefilled = 0;
            next = now + DSP_FRAME_NS;
            atomic_store(&dsp_pacer_next_ns, next);
            {
                struct timespec ts = {0, (long)DSP_FRAME_NS};
                nanosleep(&ts, NULL);
            }
            continue;
        }
        if (!prefilled) {
            uint64_t pf = dsp_sink_target_frames;
            if (pf < 4u) pf = 4u; if (pf > 32u) pf = 32u;
            dsp_pacer_t0_ns = now;
            dsp_pacer_t0_frames = dsp_pacer_frames;
            /* Back-dated deadline => the budget loop fills the whole lead. */
            next = now > (pf - 1u) * DSP_FRAME_NS ? now - (pf - 1u) * DSP_FRAME_NS : 0u;
            burst_allow = (unsigned)pf;
            prefilled = 1;
            ++dsp_pacer_prefills;
            report_t0 = now; report_frames = dsp_pacer_frames; report_sink = dsp_wav_frames;
        }
        if (atomic_exchange(&dsp_sink_reseed, 0)) {
            ++dsp_sink_reseeds;
            /* A stall longer than the current lead got through: grow the lead
             * so the next one is absorbed instead of clicked, and re-burst it. */
            if (dsp_sink_target_ns < 200000000ull) {
                uint64_t grown = dsp_sink_target_ns * 3u / 2u;
                uint32_t f;
                if (grown > 200000000ull) grown = 200000000ull;
                if (grown > dsp_sink_ceiling_ns) grown = dsp_sink_ceiling_ns;
                dsp_sink_target_ns = grown;
                f = (uint32_t)(grown / DSP_FRAME_NS);
                if (f > 32u) f = 32u;
                if (f > dsp_sink_target_frames) dsp_sink_target_frames = f;
                fprintf(stderr, "dsp audio: sink underrun -> lead target raised to %llu us (%u frames)\n",
                        (unsigned long long)(dsp_sink_target_ns / 1000u), dsp_sink_target_frames);
            }
            {
                uint64_t pf = dsp_sink_target_frames;
                uint64_t t;
                if (pf > 32u) pf = 32u;
                t = now > (pf - 1u) * DSP_FRAME_NS ? now - (pf - 1u) * DSP_FRAME_NS : 0u;
                if ((int64_t)(t - next) < 0) next = t;
                burst_allow = (unsigned)pf;
            }
        }
        for (;;) {
            /*  - budget: never more than elapsed wall-clock frames + the lead
             *    (no runaway, no queue ratcheting into the latency clamp);
             *  - catch-up: frames owed by a stall are only produced while the
             *    device queue is below the lead target.  Normal wall-clock
             *    emission is never paused: a sink left idle goes silent and
             *    stops draining (PipeWire idle-suspend), which would deadlock
             *    the lead. */
            uint64_t elapsed = (now - dsp_pacer_t0_ns) / DSP_FRAME_NS;
            uint64_t emitted = (dsp_pacer_frames + caught) - dsp_pacer_t0_frames;
            uint64_t ahead;
            if (emitted >= elapsed + (uint64_t)dsp_sink_target_frames) { ++dsp_sink_lead_holds; break; }
            if ((int64_t)(now - next) < 0) break;
            if (caught >= burst_allow) break;   /* one frame per tick, except the arm burst */
            if (!dsp_pacer_emit(next)) { ++dsp_pacer_drops; break; }
            next += DSP_FRAME_NS;
            ++caught;
            if (caught >= DSP_SINK_MAX_CATCHUP + 8u) break;
        }
        if (caught) {
            dsp_pacer_frames += caught;
            perf_dsp_ticks += caught;
            if (getenv("DQ8_TRACE_AUDIO") &&
                dsp_pacer_frames - report_frames >= DSP_TRACE_FRAMES) {
                dsp_audio_trace_line(report_t0, report_sink);
                report_t0 = host_monotonic_ns();
                report_frames = dsp_pacer_frames;
                report_sink = dsp_wav_frames;
            }
        }
        if ((int64_t)(host_monotonic_ns() - next) >= 0) {
            /* Fell behind the wall clock (descheduled): resync instead of
             * spiralling, exactly like the hardware DSP's own clock. */
            next = host_monotonic_ns() + DSP_FRAME_NS;
            ++perf_dsp_resyncs;
        }
        atomic_store(&dsp_pacer_next_ns, next);
        now = host_monotonic_ns();
        if (next > now) {
            uint64_t d = next - now;
            struct timespec ts;
            ts.tv_sec = (time_t)(d / 1000000000ull);
            ts.tv_nsec = (long)(d % 1000000000ull);
            nanosleep(&ts, NULL);
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static void dsp_pacer_start(void) {
    if (dsp_pacer_started) return;
    atomic_store(&dsp_pacer_stop, 0);
    if (pthread_create(&dsp_pacer_thread, NULL, dsp_pacer_main, NULL) != 0) {
        fputs("dsp audio: cannot create DSP pacer thread\n", stderr);
        return;
    }
    dsp_pacer_started = 1;
}
static void dsp_pacer_stop_and_join(void) {
    if (!dsp_pacer_started) return;
    atomic_store(&dsp_pacer_stop, 1);
    pthread_join(dsp_pacer_thread, NULL);
    dsp_pacer_started = 0;
    dsp_job_free_owned(dsp_pending_music_job); dsp_pending_music_job = NULL;
    dsp_job_free_owned(dsp_pending_sfx_job); dsp_pending_sfx_job = NULL;
    dsp_job_free_owned(dsp_tpl_last_music); dsp_job_free_owned(dsp_tpl_last_sfx);
    dsp_tpl_last_music = dsp_tpl_last_sfx = NULL;
    dsp_tpl_last_valid = 0;
    pthread_mutex_lock(&dsp_tpl_mu);
    while (dsp_tpl_tail != dsp_tpl_head) {
        unsigned slot = dsp_tpl_tail % DSP_TEMPLATE_CAPACITY;
        dsp_job_free_owned(dsp_tpl_ring[slot].music);
        dsp_job_free_owned(dsp_tpl_ring[slot].sfx);
        dsp_tpl_ring[slot].music = dsp_tpl_ring[slot].sfx = NULL;
        dsp_tpl_tail++;
    }
    pthread_mutex_unlock(&dsp_tpl_mu);
}

/* Main-thread harvest: snapshot the guest's DSP deltas into a template and
 * hand it to the pacer.  Runs on the guest thread only, so `ram` stays single
 * threaded.  Cheap no-op when the guest has not touched the DSP state. */
static void dsp_audio_harvest(void) {
    uint32_t read, write;
    DspAudioJob *music, *sfx;
    unsigned slot;
    uint64_t now;
    if (dsp_source_event_pending) return;   /* keep the one-shot SourceStatus latch */
    /* Parse at the DSP frame cadence (the old main-loop tick parsed once per
     * produced frame).  It cannot be gated on the progress counter itself:
     * bumping that counter is what the parse does, so gating on it would
     * never let the first guest write be seen. */
    now = dq8_guest_now_ns();
    if (dsp_harvest_last_ns && now - dsp_harvest_last_ns < DSP_FRAME_NS) return;
    dsp_harvest_last_ns = now;
    if (getenv("DQ8_TRACE_DSP_HARVEST")) {
        static uint64_t next_report;
        if (now >= next_report) {
            fprintf(stderr, "dsp harvest: progress=%llu harvested=%llu event_pending=%d "
                    "audio_on=%d tpl_valid=%d ring=%u under_region=%08X\n",
                    (unsigned long long)dsp_guest_progress,
                    (unsigned long long)dsp_guest_harvested, dsp_source_event_pending,
                    (int)dsp_audio_on, dsp_tpl_last_valid,
                    (unsigned)(dsp_tpl_head - dsp_tpl_tail), dsp_current_region());
            next_report = now + 1000000000ull;
        }
    }
    if (!dsp_audio_parse_frame(&read, &write)) return;
    if (dsp_guest_progress == dsp_guest_harvested) return;   /* nothing new */
    pthread_mutex_lock(&dsp_tpl_mu);
    if ((unsigned)(dsp_tpl_head - dsp_tpl_tail) >= DSP_TEMPLATE_CAPACITY) {
        pthread_mutex_unlock(&dsp_tpl_mu);
        return;                             /* pacer busy: keep deltas in the shadow */
    }
    pthread_mutex_unlock(&dsp_tpl_mu);
    music = dsp_audio_make_job(dsp_audio_sequence, 0u, read, write, 1);
    sfx = dsp_audio_make_job(dsp_audio_sequence, 0u, read, write, 0);
    if (!music || !sfx) {
        dsp_job_free_owned(music); dsp_job_free_owned(sfx);
        fputs("dsp audio: cannot allocate harvest template\n", stderr);
        return;
    }
    /* Ownership of the snapshotted payloads moved into the two jobs. */
    dsp_audio_transfer_shadow(1);
    dsp_audio_transfer_shadow(0);
    dsp_guest_harvested = dsp_guest_progress;
    pthread_mutex_lock(&dsp_tpl_mu);
    slot = dsp_tpl_head % DSP_TEMPLATE_CAPACITY;
    dsp_tpl_ring[slot].music = music;
    dsp_tpl_ring[slot].sfx = sfx;
    dsp_tpl_head++;
    pthread_mutex_unlock(&dsp_tpl_mu);
}

/* ---------------------------------------------------------------------------
 * DETERMINISTIC (t_115d297c): synchronous DSP frame production.
 *
 * The shipped path paces frames from a host thread (`dsp-pacer`) and mixes
 * them on two more host threads, so the guest's audio-pipe interrupts,
 * SourceStatus updates and refill callbacks land at wall-clock points that
 * differ run to run -- one of the sources of the 22.83% frame divergence in
 * t_8510204b.  With DQ8_DETERMINISTIC=1 no audio thread is started and no
 * device is opened: the frame cadence comes from the same virtual clock the
 * vblank chain uses (dq8_guest_now_ns), the newest harvested template is
 * mixed in place and committed on the guest thread, and therefore the guest
 * sees exactly the same DSP state at the same instruction count every run.
 * No real-time wait (SDL queue pacing, nanosleep hand-off) is reached.
 * ------------------------------------------------------------------------- */
static uint64_t dsp_det_next_ns;
static int dsp_det_frame(const DspAudioJob *music_tpl, const DspAudioJob *sfx_tpl, int deltas) {
    DspAudioJob *music = dsp_job_clone(music_tpl, dsp_audio_sequence, 0u, deltas);
    DspAudioJob *sfx = dsp_job_clone(sfx_tpl, dsp_audio_sequence, 0u, deltas);
    if (!music || !sfx) {
        dsp_job_free_owned(music); dsp_job_free_owned(sfx);
        return 0;
    }
    dsp_process_music(music);
    dsp_process_sfx(sfx, music);
    dsp_audio_commit(sfx);          /* writes guest RAM, signals the pipe IRQ */
    dsp_job_free_owned(music);
    ++dsp_audio_sequence;
    ++dq8_det_audio_frames;
    return 1;
}
static void dsp_audio_det_tick(uint64_t now) {
    unsigned caught = 0;
    if (!atomic_load(&dsp_audio_on)) return;
    if (!dsp_det_next_ns) dsp_det_next_ns = now;
    while ((int64_t)(now - dsp_det_next_ns) >= 0 && caught < 8u) {
        dsp_audio_harvest();
        if (dsp_tpl_tail != dsp_tpl_head) {
            DspAudioJob *music, *sfx;
            unsigned slot;
            pthread_mutex_lock(&dsp_tpl_mu);
            slot = dsp_tpl_tail % DSP_TEMPLATE_CAPACITY;
            music = dsp_tpl_ring[slot].music; sfx = dsp_tpl_ring[slot].sfx;
            dsp_tpl_ring[slot].music = dsp_tpl_ring[slot].sfx = NULL;
            dsp_tpl_tail++;
            pthread_mutex_unlock(&dsp_tpl_mu);
            if (dsp_det_frame(music, sfx, 1)) {
                dsp_job_free_owned(dsp_tpl_last_music);
                dsp_job_free_owned(dsp_tpl_last_sfx);
                dsp_tpl_last_music = music; dsp_tpl_last_sfx = sfx;
                dsp_tpl_last_valid = 1;
            } else {
                dsp_job_free_owned(music); dsp_job_free_owned(sfx);
            }
        } else if (dsp_tpl_ensure_default()) {
            (void)dsp_det_frame(dsp_tpl_last_music, dsp_tpl_last_sfx, 0);
        }
        dsp_det_next_ns += DSP_FRAME_NS;
        ++caught;
    }
    if (caught >= 8u && (int64_t)(now - dsp_det_next_ns) >= 0)
        dsp_det_next_ns = now + DSP_FRAME_NS;
    if (caught) ++perf_dsp_ticks;
    atomic_store(&dsp_next_frame_ns, dsp_det_next_ns);
}

/* DETERMINISTIC (t_115d297c): a savestate restores the DSP latch/cadence
   bookkeeping (dsp_source_event_pending is one of the saved vars), but those
   describe a guest turn that will never happen again after a cold load: the
   latched SourceStatus/pipe-IRQ hand-off has no pending guest turn to consume
   it, and dsp_audio_harvest() refuses to parse while it is set.  Re-arm the
   synchronous path so the first det tick produces a frame and signals the
   pipe interrupt the resumed guest is waiting for. */
static void dsp_audio_det_reset_after_load(void) {
    if (!dq8_det_mode) return;
    dsp_source_event_pending = 0;
    dsp_det_next_ns = 0;
    dsp_harvest_last_ns = 0;
    dsp_tpl_last_valid = 0;
    dsp_tpl_head = dsp_tpl_tail = 0;
    atomic_store(&dsp_next_frame_ns, 0u);
    fprintf(stderr, "DETERMINISTIC: DSP audio path re-armed after state load\n");
}
static void dsp_audio_tick(void) {
    if (dq8_det_mode) { dsp_audio_det_tick(dq8_guest_now_ns()); return; }
    /* Commit finished frames first: this is the only part that writes guest
     * RAM and signals the audio-pipe interrupt, so it stays on this thread. */
    (void)dsp_audio_commit_ready();
    if (!atomic_load(&dsp_audio_on)) return;
    if (!dsp_workers_started) { dsp_audio_out_init(); dsp_audio_workers_start(); }
    if (!dq8_worker_scaffold_is_active(&dsp_audio_worker_gate)) return;
    dsp_pacer_start();
    dsp_audio_harvest();
    /* Wake hint for the guest wait loops: while the idle throttle is armed the
     * only audio is silence, so do not shorten their sleep for it. */
    if (idle_throttle_armed())
        atomic_store(&dsp_next_frame_ns, 0u);
    else
        atomic_store(&dsp_next_frame_ns, atomic_load(&dsp_pacer_next_ns));
}
