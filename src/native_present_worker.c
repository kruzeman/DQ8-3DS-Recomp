/* F4/F5 presentation worker (DQ8_SPLIT_RENDER=1), textually included after
   gsp_write_screen_ppms so it can call the conversion core and the PPM
   writer implementation.  The guest/runtime thread stays the producer: it
   snapshots framebuffer bytes out of guest RAM into an owned job
   (split_present_submit, defined before host_display_capture_transfer) and
   pushes it through src/native_spsc.h.  This thread owns the shadow
   buffers, the SDL window/event pump and every present from then on.

   Ownership transfer (H1 contract §2): a successful push transfers the job
   and its raw copy to this consumer; a failed push frees the job on the
   guest side and drops only a PRESENTED frame (never guest-visible state).
   The completion queue direction is the done_seq release store: after the
   conversion is visible the guest may free/reuse nothing (jobs are freed
   here) but flush-sync waits on it.  No pointer into guest RAM ever crosses
   the queue. */

static pthread_t split_present_thread;
static _Atomic int split_present_run;      /* thread started */
static _Atomic int split_present_ready;    /* window/init done on worker */
static _Atomic int split_present_stop;     /* shutdown request */
static _Atomic uint32_t split_present_hid_mask; /* SDL key state for the guest pump */
static _Atomic int split_present_quit;     /* SDL_QUIT seen on the worker */
static uint64_t split_present_converted;   /* conversions actually performed */
static uint64_t split_present_elided;      /* identical-content jobs skipped */
static uint64_t split_present_presents;    /* present calls issued */

static uint32_t split_render_hid_mask(void) {
    return atomic_load_explicit(&split_present_hid_mask, memory_order_acquire);
}
static int split_render_quit_requested(void) {
    return atomic_load_explicit(&split_present_quit, memory_order_acquire);
}

/* Content elision: the presenter keeps the previous raw snapshot per
   (w,h,fmt,bytes) shape and skips conversion+present when the new snapshot
   is byte-identical.  This replaces the guest-side dirty tracking for the
   split path: a static scene then costs one memcmp per vblank on THIS
   thread and zero X11 traffic, mirroring the idle-throttle present skip. */
typedef struct {
    uint8_t *raw;
    uint32_t bytes;
    uint32_t w, h, fmt;
    int valid;
} SplitPresentPrev;
#define SPLIT_PRESENT_PREV_SLOTS 4u
static SplitPresentPrev split_prev[SPLIT_PRESENT_PREV_SLOTS];
static unsigned split_prev_next; /* round-robin eviction for new shapes */

static void split_present_stats_dump(void) {
    fprintf(stderr,
            "split present worker: converted=%llu elided=%llu presents=%llu "
            "pushed=%llu dropped=%llu\n",
            (unsigned long long)split_present_converted,
            (unsigned long long)split_present_elided,
            (unsigned long long)split_present_presents,
            (unsigned long long)split_present_pushed,
            (unsigned long long)split_present_dropped);
}

static void split_present_convert_job(const SplitPresentJob *job) {
    /* Top and bottom screens arrive as alternating jobs with different
       shapes; each distinct (bytes,w,h,fmt) shape keeps its own previous
       snapshot so an unchanged screen still elides. */
    SplitPresentPrev *prev = NULL;
    unsigned free_slot = 0;
    for (unsigned i = 0; i < SPLIT_PRESENT_PREV_SLOTS; ++i) {
        SplitPresentPrev *s = &split_prev[i];
        if (s->valid && s->bytes == job->bytes && s->w == job->w &&
            s->h == job->h && s->fmt == job->fmt) { prev = s; break; }
        if (!s->valid && !split_prev[free_slot].valid) free_slot = i;
    }
    if (!prev) {
        prev = &split_prev[split_prev[free_slot].valid
                               ? (split_prev_next++ % SPLIT_PRESENT_PREV_SLOTS)
                               : free_slot];
    }
    if (prev->valid && prev->bytes == job->bytes && prev->w == job->w &&
        prev->h == job->h && prev->fmt == job->fmt &&
        memcmp(prev->raw, job->raw, job->bytes) == 0) {
        ++split_present_elided;
        return;
    }
    if (!prev->raw || prev->bytes < job->bytes) {
        free(prev->raw);
        prev->raw = (uint8_t *)malloc(job->bytes);
        prev->bytes = prev->raw ? job->bytes : 0;
    }
    if (prev->raw) {
        memcpy(prev->raw, job->raw, job->bytes);
        prev->w = job->w; prev->h = job->h; prev->fmt = job->fmt;
        prev->valid = 1;
    }
    host_display_convert_span(job->raw, job->bytes, job->w, job->h, job->fmt);
    host_shadow_dirty = 1; /* converted content needs a present */
    ++split_present_converted;
}

/* Wait (bounded) until every job submitted so far has been converted:
   done_seq is release-published after the conversion is visible.  Used by
   clean-exit paths (bench_report, quit) so the shadow buffers frame-dump
   tools read the final picture. */
static void split_render_flush_sync(void) {
    uint64_t want;
    if (!atomic_load_explicit(&split_present_run, memory_order_acquire)) return;
    want = split_present_pushed;
    while (atomic_load_explicit(&split_present_done_seq, memory_order_acquire) < want) {
        struct timespec ts = { 0, 200000L };
        nanosleep(&ts, NULL);
    }
}

static void split_render_shutdown(void) {
    if (!atomic_load_explicit(&split_present_run, memory_order_acquire)) return;
    split_render_flush_sync();
    atomic_store_explicit(&split_present_stop, 1, memory_order_release);
    pthread_join(split_present_thread, NULL);
    atomic_store_explicit(&split_present_run, 0, memory_order_release);
}

static void split_render_atexit(void) { split_render_shutdown(); }

#ifdef DQ8_ENABLE_SDL
/* SDL event collection on the worker thread: same handling as the guest
   pump's collector, but the button state crosses to the guest only through
   the copied atomic mask (H1 contract §3.4). */
static void split_present_collect(uint32_t *buttons) {
    if (host_display_ready) host_display_collect_events(buttons);
}
static void split_present_teardown(void) {
    host_x11_present_shutdown();
    if (host_shadow_surf_top) SDL_FreeSurface(host_shadow_surf_top);
    if (host_shadow_surf_bot) SDL_FreeSurface(host_shadow_surf_bot);
    host_shadow_surf_top = host_shadow_surf_bot = NULL;
    host_sdl_surface = NULL;
    if (host_sdl_tex_top) SDL_DestroyTexture(host_sdl_tex_top);
    if (host_sdl_tex_bot) SDL_DestroyTexture(host_sdl_tex_bot);
    if (host_sdl_renderer) SDL_DestroyRenderer(host_sdl_renderer);
    if (host_sdl_window) SDL_DestroyWindow(host_sdl_window);
    host_sdl_tex_top = host_sdl_tex_bot = NULL;
    host_sdl_renderer = NULL;
    host_sdl_window = NULL;
    SDL_Quit();
}
#else
static void split_present_collect(uint32_t *buttons) { (void)buttons; }
static void split_present_teardown(void) {}
#endif

static void *split_present_main(void *arg) {
    (void)arg;
    /* Window bring-up runs on THIS thread so every later SDL/X11 call stays
       on one host thread (no Xlib multi-threading is required). */
    host_display_init_body();
    atomic_store_explicit(&split_present_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&split_present_stop, memory_order_acquire)) {
        int worked = 0;
        uintptr_t tok;
        while (dq8_spsc_try_pop(&split_present_q, &tok)) {
            SplitPresentJob *job = (SplitPresentJob *)tok;
            split_present_convert_job(job);
            atomic_store_explicit(&split_present_done_seq, job->seq,
                                  memory_order_release);
            free(job->raw);
            free(job);
            worked = 1;
        }
        if (worked) gsp_write_screen_ppms_impl();
        uint32_t buttons = atomic_load_explicit(&split_present_hid_mask,
                                                memory_order_relaxed);
        split_present_collect(&buttons);
        atomic_store_explicit(&split_present_hid_mask, buttons,
                              memory_order_release);
        if (host_display_quit)
            atomic_store_explicit(&split_present_quit, 1, memory_order_release);
        if (host_display_ready && host_shadow_dirty) {
            host_display_present_impl();
            ++split_present_presents;
        }
        if (!worked) {
            /* Idle: bound input latency well under one HID period while
               staying cheap (this thread is off the guest loop). */
            struct timespec ts = { 0, 1000000L };
            nanosleep(&ts, NULL);
        }
    }
    /* Final drain so no submitted frame is lost at shutdown. */
    {
        uintptr_t tok;
        while (dq8_spsc_try_pop(&split_present_q, &tok)) {
            SplitPresentJob *job = (SplitPresentJob *)tok;
            split_present_convert_job(job);
            atomic_store_explicit(&split_present_done_seq, job->seq,
                                  memory_order_release);
            free(job->raw);
            free(job);
        }
        if (host_display_ready && host_shadow_dirty) host_display_present_impl();
    }
    split_present_teardown();
    return NULL;
}

static void split_render_start(void) {
    static int stats_registered;
    if (!stats_registered) {
        stats_registered = 1;
        atexit(split_present_stats_dump);
        atexit(split_render_atexit); /* join before other SDL teardown */
    }
    if (!dq8_spsc_init(&split_present_q, split_present_q_storage,
                       SPLIT_PRESENT_QUEUE_CAP)) {
        fprintf(stderr, "split present worker: queue init failed; disabled\n");
        return;
    }
    atomic_store_explicit(&split_present_stop, 0, memory_order_relaxed);
    if (pthread_create(&split_present_thread, NULL, split_present_main, NULL)) {
        fprintf(stderr, "split present worker: pthread_create failed; disabled\n");
        return;
    }
    atomic_store_explicit(&split_present_run, 1, memory_order_release);
    /* Bounded wait for window bring-up so host_display_ready is valid
       before the guest starts (same invariant as the inline init). */
    for (unsigned i = 0; i < 5000u; ++i) {
        if (atomic_load_explicit(&split_present_ready, memory_order_acquire)) {
            fprintf(stderr, "split present worker: ready (DQ8_SPLIT_RENDER=1)\n");
            return;
        }
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "split present worker: init timeout (continuing)\n");
}
