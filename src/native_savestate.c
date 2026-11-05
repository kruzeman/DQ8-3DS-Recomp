/* ==========================================================================
   SAVESTATE (t_d50c9503) — RAM snapshot / restore for fast operator testing.

   Design (mirrors Azahar's savestate.cpp: CSTHeader{magic,title id,build
   revision,time} + compressed stream of timing/CPU/services/kernel/memory/
   GPU/movie objects; see reports/SAVESTATE_TOOLING.md):

     * the ENTIRE 1 GiB guest address space (`ram`) is captured, sparse: only
       64 KiB chunks with a non-zero byte are written, so a state is roughly
       as big as the game actually touched (tens of MiB) instead of 1 GiB;
     * every host-side variable that holds guest-visible kernel / service
       state is captured by name from `savestate_vars[]` (threads + wait
       state, mutex/event/arbiter tables, handle allocators, HID ring, GSP
       scan-out bookkeeping, PICA RT/present bookkeeping, Y2R config, DSP
       HLE counters, FS archive/dir bookkeeping, shadow framebuffers);
     * host RESOURCES that cannot be copied across processes (FILE*, DIR*,
       GL/EGL objects, SDL window) are re-created on load: FS handles are
       recorded as {path, mode, offset} and reopened, everything else is
       already alive in the target process because main() initialised the
       runtime before a startup load restored the state;
     * the MAIN guest context is a flat dispatch loop (`translated_from`), so
       a state is taken and resumed at a dispatch boundary: pc/regs/flags are
       exact and there is no host call stack to unwind (this is why guest
       "calls" are pc transitions, not host calls).

   Header validation follows Azahar: magic + format version + a build
   signature computed from the runtime's own struct/var-table layout, so a
   state from a different build is rejected with a message instead of
   corrupting the guest.

   Triggers: SDL F5 = save, F9 = load (live), `--load-state PATH` /
   `--state PATH` (and DQ8_STATE_FILE / DQ8_LOAD_STATE / DQ8_STATE_DIR envs).
   Test hooks: DQ8_SAVESTATE_AUTOSAVE_AT=<sec> / DQ8_SAVESTATE_AUTOLOAD_AT=<sec>
   (headless runs have no SDL window, so the operator can still smoke-test).
   ========================================================================== */
#include <stdatomic.h>
#include <sys/types.h>

#define SS_MAGIC "DQ8STATE"
#define SS_VERSION 3u
#define SS_CHUNK_LOG2 16u
#define SS_CHUNK_SIZE (1u << SS_CHUNK_LOG2)
#define SS_CHUNKS ((uint32_t)(GUEST_RAM_SIZE >> SS_CHUNK_LOG2))
#define SS_PATH_CAP 2048u

typedef struct {
    const char *name;
    void *addr;
    uint32_t size;
} SavestateVar;

/* Guest-visible state that must survive a save/load.  Anything holding a host
   pointer (FILE*, DIR*, EGL/GL names, pthread handles, SPSC queues) is
   deliberately NOT in this table — those are re-created, see savestate_reopen_fs. */
static const SavestateVar savestate_vars[] = {
    /* --- guest execution contexts ------------------------------------- */
    { "guest_threads", &guest_threads, (uint32_t)sizeof guest_threads },
    { "guest_thread_count", &guest_thread_count, 4u },
    { "guest_thread_cursor", &guest_thread_cursor, 4u },
    { "current_guest_thread_id", &current_guest_thread_id, 4u },
    { "next_guest_thread_handle", &next_guest_thread_handle, 4u },
    { "next_memory_block_handle", &next_memory_block_handle, 4u },
    { "active_tls_base", &active_tls_base, 4u },
    { "exclusive_addr", &exclusive_addr, 4u },
    { "exclusive_size", &exclusive_size, 4u },
    { "main_guest_pc", &main_guest_pc, 4u },
    { "current_guest_pc", &current_guest_pc, 4u },
    { "previous_guest_pc", &previous_guest_pc, 4u },
    { "guest_stack_top", &guest_stack_top, (uint32_t)sizeof guest_stack_top },
    { "guest_stack_low", &guest_stack_low, (uint32_t)sizeof guest_stack_low },
    { "guest_stack_live", &guest_stack_live, (uint32_t)sizeof guest_stack_live },
    /* --- kernel object tables (Azahar: Kernel::serialize) -------------- */
    { "next_mutex_handle", &next_mutex_handle, 4u },
    { "mutex_live", &mutex_live, (uint32_t)sizeof mutex_live },
    { "mutex_locked", &mutex_locked, (uint32_t)sizeof mutex_locked },
    { "mutex_owner", &mutex_owner, (uint32_t)sizeof mutex_owner },
    { "mutex_recursion", &mutex_recursion, (uint32_t)sizeof mutex_recursion },
    { "next_event_handle", &next_event_handle, 4u },
    { "event_live", &event_live, (uint32_t)sizeof event_live },
    { "event_signaled", &event_signaled, (uint32_t)sizeof event_signaled },
    { "event_manual_reset", &event_manual_reset, (uint32_t)sizeof event_manual_reset },
    { "next_arbiter_handle", &next_arbiter_handle, 4u },
    { "arbiter_live", &arbiter_live, (uint32_t)sizeof arbiter_live },
    { "main_arbiter_waiting", &main_arbiter_waiting, 1u },
    { "main_arbiter_signaled", &main_arbiter_signaled, 1u },
    { "main_arbiter_handle", &main_arbiter_handle, 4u },
    { "main_arbiter_address", &main_arbiter_address, 4u },
    /* --- APT / GSP service bookkeeping -------------------------------- */
    { "apt_pending_signal", &apt_pending_signal, 4u },
    { "apt_notification_event", &apt_notification_event, 4u },
    { "apt_parameter_event", &apt_parameter_event, 4u },
    { "gsp_relay_registered", &gsp_relay_registered, 1u },
    { "gsp_relay_event", &gsp_relay_event, 4u },
    { "gsp_shared_base", &gsp_shared_base, 4u },
    { "gsp_thread_id", &gsp_thread_id, 4u },
    { "gsp_lcd_force_black", &gsp_lcd_force_black, 4u },
    { "gsp_fb_addr", &gsp_fb_addr, (uint32_t)sizeof gsp_fb_addr },
    { "gsp_fb_shown", &gsp_fb_shown, (uint32_t)sizeof gsp_fb_shown },
    { "gsp_fb_valid", &gsp_fb_valid, (uint32_t)sizeof gsp_fb_valid },
    { "host_cap_last_valid", &host_cap_last_valid, (uint32_t)sizeof host_cap_last_valid },
    { "pica_rt_addr", &pica_rt_addr, 4u },
    { "pica_rt_w", &pica_rt_w, 4u },
    { "pica_rt_h", &pica_rt_h, 4u },
    { "pica_rt_format", &pica_rt_format, 4u },
    { "pica_draw_count", &pica_draw_count, 8u },
    /* --- presented framebuffers (so the loaded frame is on screen) ----- */
    { "host_shadow_top", &host_shadow_top, (uint32_t)sizeof host_shadow_top },
    { "host_shadow_bot", &host_shadow_bot, (uint32_t)sizeof host_shadow_bot },
    /* --- HID ring (native_hid.c) ------------------------------------- */
    { "native_hid_base", &native_hid_base, 4u },
    { "native_hid_previous", &native_hid_previous, 4u },
    { "native_hid_events", &native_hid_events, (uint32_t)sizeof native_hid_events },
    { "native_hid_index", &native_hid_index, 4u },
    /* --- DSP HLE (pointer-free part of native_audio.c) ---------------- */
    { "dsp_pipe_interrupt_event", &dsp_pipe_interrupt_event, 4u },
    { "dsp_source_event_pending", &dsp_source_event_pending, 4u },
    { "dsp_recv_data", &dsp_recv_data, (uint32_t)sizeof dsp_recv_data },
    { "dsp_recv_ready", &dsp_recv_ready, (uint32_t)sizeof dsp_recv_ready },
    { "dsp_audio_pipe", &dsp_audio_pipe, (uint32_t)sizeof dsp_audio_pipe },
    { "dsp_audio_pipe_size", &dsp_audio_pipe_size, 4u },
    { "dsp_audio_pipe_pos", &dsp_audio_pipe_pos, 4u },
    { "dsp_audio_on", &dsp_audio_on, 4u },
    { "dsp_frames_out", &dsp_frames_out, 8u },
    { "dsp_sources_started", &dsp_sources_started, 8u },
    { "dsp_sources_finished", &dsp_sources_finished, 8u },
    { "dsp_master_volume", &dsp_master_volume, 4u },
    { "dsp_aux_volume", &dsp_aux_volume, (uint32_t)sizeof dsp_aux_volume },
    { "dsp_audio_guest_region", &dsp_audio_guest_region, 4u },
    /* --- Y2R (movie colour conversion service) ----------------------- */
    { "y2r", &y2r, (uint32_t)sizeof y2r },
    { "y2r_inited", &y2r_inited, 4u },
    /* --- FS bookkeeping (the host FILE and DIR resources are reopened) - */
    { "guest_file_is_romfs", &guest_file_is_romfs, (uint32_t)sizeof guest_file_is_romfs },
    { "guest_archive_id", &guest_archive_id, (uint32_t)sizeof guest_archive_id },
    { "guest_dir_open", &guest_dir_open, (uint32_t)sizeof guest_dir_open },
    { "native_archive_paths", &native_archive_paths, (uint32_t)sizeof native_archive_paths },
    { "native_dir_paths", &native_dir_paths, (uint32_t)sizeof native_dir_paths },
    { "native_next_archive", &native_next_archive, 4u },
};
#define SS_VAR_COUNT ((uint32_t)(sizeof savestate_vars / sizeof savestate_vars[0]))

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint64_t build_sig;      /* struct/var-table layout identity of the build */
    uint32_t ram_size;
    uint32_t chunk_log2;
    uint32_t chunk_count;    /* saved 64 KiB chunks */
    uint32_t var_count;
    uint32_t var_bytes;
    uint32_t thread_count;
    uint32_t main_pc;
    uint32_t main_flags;     /* bit0 n, bit1 z, bit2 c, bit3 v, bit4 ge */
    uint32_t start_pc;
    uint32_t fs_file_count;
    uint64_t saved_unix;
    uint64_t file_bytes;     /* full state size, written last */
    uint32_t main_r[16];
} SavestateHeader;

typedef struct {
    uint32_t handle;
    uint32_t mode;           /* 0 = "rb", 1 = "r+b", 2 = "w+b" */
    uint64_t offset;
    char path[SS_PATH_CAP];
} SavestateFsFile;

static char savestate_path[SS_PATH_CAP];
static char savestate_path_cache[SS_PATH_CAP];   /* resolved default slot */
static _Atomic int savestate_pending;            /* 0 none, 1 save, 2 load */
static int savestate_resume_valid;
static int savestate_resume_n, savestate_resume_z, savestate_resume_c;
static int savestate_resume_v, savestate_resume_ge;
static uint64_t savestate_auto_t0_ns;
static int savestate_auto_saved, savestate_auto_loaded;
static int savestate_busy;

static void savestate_set_path(const char *path) {
    if (!path || !*path) return;
    snprintf(savestate_path, sizeof savestate_path, "%s", path);
    fprintf(stderr, "SAVESTATE: explicit state path %s\n", savestate_path);
}

/* Default slot: next to the binary (/proc/self/exe), overridable by
   DQ8_STATE_FILE (full path) or DQ8_STATE_DIR (directory). */
static const char *savestate_default_path(void) {
    if (savestate_path[0]) return savestate_path;
    if (savestate_path_cache[0]) return savestate_path_cache;
    const char *file = getenv("DQ8_STATE_FILE");
    if (file && *file) {
        snprintf(savestate_path_cache, sizeof savestate_path_cache, "%s", file);
        return savestate_path_cache;
    }
    char dir[SS_PATH_CAP];
    const char *direnv = getenv("DQ8_STATE_DIR");
    if (direnv && *direnv) {
        snprintf(dir, sizeof dir, "%s", direnv);
    } else {
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1u);
        if (n <= 0) snprintf(dir, sizeof dir, ".");
        else {
            dir[n] = 0;
            char *slash = strrchr(dir, '/');
            if (slash && slash != dir) *slash = 0;
            else snprintf(dir, sizeof dir, ".");
        }
    }
    snprintf(savestate_path_cache, sizeof savestate_path_cache, "%s/dq8-state.sav", dir);
    return savestate_path_cache;
}

/* Layout signature: rejects a state written by a different build (Azahar
   compares the git revision + build version in CSTHeader). */
static uint64_t savestate_build_sig(void) {
    static uint64_t cached;
    if (cached) return cached;
    uint64_t h = 1469598103934665603ull; /* FNV-1a 64 */
    #define SS_HASH_BYTES(ptr, len) do { \
        const unsigned char *_p = (const unsigned char *)(ptr); \
        for (size_t _i = 0; _i < (size_t)(len); ++_i) { h ^= _p[_i]; h *= 1099511628211ull; } \
    } while (0)
    #define SS_HASH_U32(v) do { uint32_t _v = (uint32_t)(v); SS_HASH_BYTES(&_v, 4u); } while (0)
    SS_HASH_U32(SS_VERSION);
    SS_HASH_U32(sizeof(SavestateHeader));
    SS_HASH_U32(sizeof(GuestThread));
    SS_HASH_U32(sizeof(SavestateFsFile));
    SS_HASH_U32(SS_VAR_COUNT);
    for (uint32_t i = 0; i < SS_VAR_COUNT; ++i) {
        const char *name = savestate_vars[i].name;
        SS_HASH_BYTES(name, strlen(name));
        SS_HASH_U32(savestate_vars[i].size);
    }
    cached = h;
    #undef SS_HASH_U32
    #undef SS_HASH_BYTES
    return h;
}

static void savestate_request(int kind) {
    atomic_store(&savestate_pending, kind);
}

/* HOST COMMANDS IN A WAIT (t_56936fec): the seeker the main-context wait
   loops use.  A request is normally consumed by savestate_service() at the
   next main-context dispatch boundary, but a main-context SVC wait that never
   completes never reaches one -- so the wait loop polls this instead of the
   variable, breaks the wait, and lets the dispatcher act.  Relaxed load: the
   no-request case costs one plain load per wait-loop iteration. */
static int savestate_pending_kind(void) {
    return atomic_load_explicit(&savestate_pending, memory_order_relaxed);
}

/* One line per host command that had to leave a wait, so an operator's log
   shows the command was taken from inside the wait rather than at a frame
   boundary.  Bounded: a wedged guest cannot flood the log. */
static unsigned savestate_wait_escapes;
static void savestate_wait_escape(int kind, uint32_t handle, unsigned spin) {
    if (savestate_wait_escapes++ >= 8u) return;
    fprintf(stderr,
            "HOSTCMD: %s escaped from a main-context wait (handle=%08X spin=%u)"
            " -> guest keeps its timeout result, the state load replaces it\n",
            kind == 2 ? "load (F9)" : "save (F5)", handle, spin);
}

static int savestate_take_resume(int *n, int *z, int *c, int *v, int *ge) {
    if (!savestate_resume_valid) return 0;
    savestate_resume_valid = 0;
    *n = savestate_resume_n; *z = savestate_resume_z; *c = savestate_resume_c;
    *v = savestate_resume_v; *ge = savestate_resume_ge;
    return 1;
}

/* ---- FS resource reopen ------------------------------------------------ */
/* The guest's open files are host FILE* objects: they do not survive a new
   process, so the state stores {path, mode, offset} per handle and this
   reopens them (Azahar serializes ArchiveManager/File backends instead). */
static void savestate_record_fs_files(SavestateFsFile *out, uint32_t cap, uint32_t *count) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 256u && n < cap; ++i) {
        if (!native_files[i] || !native_file_paths[i][0]) continue;
        SavestateFsFile *f = &out[n];
        memset(f, 0, sizeof *f);
        f->handle = i;
        f->mode = native_file_modes[i];
        off_t pos = ftello(native_files[i]);
        f->offset = pos < 0 ? 0ull : (uint64_t)pos;
        snprintf(f->path, sizeof f->path, "%s", native_file_paths[i]);
        ++n;
    }
    *count = n;
}

static void savestate_reopen_fs(const SavestateFsFile *in, uint32_t count) {
    /* Drop every host file/dir handle this process currently owns. */
    FILE *seen[256];
    uint32_t seen_n = 0;
    for (uint32_t i = 0; i < 256u; ++i) {
        FILE *file = native_files[i];
        if (!file) continue;
        int dup = 0;
        for (uint32_t j = 0; j < seen_n; ++j) if (seen[j] == file) { dup = 1; break; }
        if (!dup) { if (seen_n < 256u) seen[seen_n++] = file; fclose(file); }
        native_files[i] = NULL;
        native_file_paths[i][0] = 0;
    }
    for (uint32_t i = 0; i < 256u; ++i) {
        if (native_dirs[i]) { closedir(native_dirs[i]); native_dirs[i] = NULL; }
    }
    uint32_t reopened = 0, failed = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const SavestateFsFile *rec = &in[k];
        if (rec->handle >= 256u || !rec->path[0]) continue;
        const char *mode = rec->mode == 2u ? "w+b" : rec->mode == 1u ? "r+b" : "rb";
        FILE *file = fopen(rec->path, mode);
        if (!file) file = fopen(rec->path, "rb");
        if (!file) { ++failed; continue; }
        if (rec->offset) fseeko(file, (off_t)rec->offset, SEEK_SET);
        native_files[rec->handle] = file;
        native_file_modes[rec->handle] = (uint8_t)rec->mode;
        snprintf(native_file_paths[rec->handle], SS_PATH_CAP, "%s", rec->path);
        ++reopened;
    }
    for (uint32_t i = 0; i < 256u; ++i) {
        if (!guest_dir_open[i] || !native_dir_paths[i][0]) continue;
        DIR *dir = opendir(native_dir_paths[i]);
        if (dir) native_dirs[i] = dir;
    }
    fprintf(stderr, "SAVESTATE: reopened %u/%u guest files (%u failed)\n",
            reopened, count, failed);
}

/* ---- save -------------------------------------------------------------- */
static int savestate_write(const char *path, uint32_t pc, const uint32_t *r,
                           int n, int z, int c, int v, int ge) {
    char tmp[SS_PATH_CAP + 8u];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "SAVESTATE save FAILED: cannot create %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    uint64_t t0 = host_monotonic_ns();
    /* A GL "episode" whose FBO content has not been written back to guest RAM
       cannot be captured: the state would restore the previous RAM content for
       that render target.  Say so instead of silently producing a state whose
       first frames differ. */
    if (pica_gl_pending())
        fprintf(stderr, "SAVESTATE warning: PICA GL render target has unwritten content at save time; "
                        "the restored frame for that target may be one episode old\n");
    SavestateHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, SS_MAGIC, 8);
    h.version = SS_VERSION;
    h.header_bytes = (uint32_t)sizeof h;
    h.build_sig = savestate_build_sig();
    h.ram_size = (uint32_t)GUEST_RAM_SIZE;
    h.chunk_log2 = SS_CHUNK_LOG2;
    h.var_count = SS_VAR_COUNT;
    h.thread_count = guest_thread_count;
    h.main_pc = pc;
    h.main_flags = (uint32_t)((n ? 1u : 0u) | (z ? 2u : 0u) | (c ? 4u : 0u) |
                              (v ? 8u : 0u) | (ge ? 16u : 0u));
    memcpy(h.main_r, r, sizeof h.main_r);
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    h.saved_unix = (uint64_t)wall.tv_sec;

    /* Pass 1: find 64 KiB guest-RAM chunks that are not all zero. */
    uint32_t *indices = malloc(sizeof(uint32_t) * (size_t)SS_CHUNKS);
    if (!indices) { fclose(f); remove(tmp); return -1; }
    uint32_t count = 0;
    for (uint32_t ci = 0; ci < SS_CHUNKS; ++ci) {
        const uint64_t *q = (const uint64_t *)(const void *)(ram + ((size_t)ci << SS_CHUNK_LOG2));
        uint64_t acc = 0;
        for (uint32_t w = 0; w < SS_CHUNK_SIZE / 8u; ++w) acc |= q[w];
        if (acc) indices[count++] = ci;
    }
    uint64_t nonzero_bytes = (uint64_t)count << SS_CHUNK_LOG2;
    h.chunk_count = count;
    h.var_bytes = 0;
    for (uint32_t i = 0; i < SS_VAR_COUNT; ++i) h.var_bytes += savestate_vars[i].size;

    SavestateFsFile *fs_files = calloc(256u, sizeof(SavestateFsFile));
    if (!fs_files) { free(indices); fclose(f); remove(tmp); return -1; }
    savestate_record_fs_files(fs_files, 256u, &h.fs_file_count);

    int ok = 1;
    ok &= fwrite(&h, sizeof h, 1u, f) == 1u;
    /* named host-state variables */
    for (uint32_t i = 0; ok && i < SS_VAR_COUNT; ++i) {
        uint32_t name_len = (uint32_t)strlen(savestate_vars[i].name);
        uint32_t size = savestate_vars[i].size;
        ok &= fwrite(&name_len, 4u, 1u, f) == 1u;
        ok &= fwrite(&size, 4u, 1u, f) == 1u;
        ok &= fwrite(savestate_vars[i].name, 1u, name_len, f) == name_len;
        ok &= fwrite(savestate_vars[i].addr, 1u, size, f) == size;
    }
    /* FS handle table */
    ok &= fwrite(&h.fs_file_count, 4u, 1u, f) == 1u;
    if (ok && h.fs_file_count) ok &= fwrite(fs_files, sizeof(SavestateFsFile), h.fs_file_count, f) == h.fs_file_count;
    /* sparse guest RAM */
    ok &= fwrite(&count, 4u, 1u, f) == 1u;
    if (ok && count) ok &= fwrite(indices, 4u, count, f) == count;
    for (uint32_t i = 0; ok && i < count; ++i)
        ok &= fwrite(ram + ((size_t)indices[i] << SS_CHUNK_LOG2), 1u, SS_CHUNK_SIZE, f) == SS_CHUNK_SIZE;
    if (ok) {
        h.file_bytes = (uint64_t)ftello(f);
        if (fseeko(f, 0, SEEK_SET) == 0) ok &= fwrite(&h, sizeof h, 1u, f) == 1u;
    }
    if (ok) ok &= fflush(f) == 0;
    ok &= !ferror(f);
    fclose(f);
    double ms = (double)(host_monotonic_ns() - t0) / 1e6;
    if (!ok) {
        fprintf(stderr, "SAVESTATE save FAILED while writing %s (disk full?)\n", tmp);
        remove(tmp);
        free(fs_files); free(indices);
        return -1;
    }
    if (rename(tmp, path)) {
        fprintf(stderr, "SAVESTATE save FAILED: rename %s -> %s: %s\n", tmp, path, strerror(errno));
        remove(tmp);
        free(fs_files); free(indices);
        return -1;
    }
    fprintf(stderr,
            "SAVESTATE save OK: %s  pc=%08X  threads=%u  chunks=%u/%u (%.1f MiB nonzero)  "
            "vars=%u (%.1f KiB)  files=%u  size=%.1f MiB  %.0f ms\n",
            path, pc, guest_thread_count, count, SS_CHUNKS,
            (double)nonzero_bytes / (1024.0 * 1024.0),
            SS_VAR_COUNT, (double)h.var_bytes / 1024.0, h.fs_file_count,
            (double)h.file_bytes / (1024.0 * 1024.0), ms);
    free(fs_files);
    free(indices);
    return 0;
}

/* ---- load -------------------------------------------------------------- */
static int savestate_read(const char *path, uint32_t *pc_out, uint32_t *r_out,
                          int *n_out, int *z_out, int *c_out, int *v_out, int *ge_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "SAVESTATE load FAILED: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    uint64_t t0 = host_monotonic_ns();
    SavestateHeader h;
    if (fread(&h, sizeof h, 1u, f) != 1u) {
        fprintf(stderr, "SAVESTATE load FAILED: %s is truncated\n", path);
        fclose(f);
        return -1;
    }
    if (memcmp(h.magic, SS_MAGIC, 8) || h.version != SS_VERSION || h.header_bytes != (uint32_t)sizeof h) {
        fprintf(stderr, "SAVESTATE load REJECTED: %s is not a version-%u DQ8 state file "
                        "(magic/version/header mismatch)\n", path, SS_VERSION);
        fclose(f);
        return -1;
    }
    if (h.build_sig != savestate_build_sig()) {
        fprintf(stderr, "SAVESTATE load REJECTED: %s was written by a different build "
                        "(state sig %016llX != runtime sig %016llX)\n",
                path, (unsigned long long)h.build_sig,
                (unsigned long long)savestate_build_sig());
        fclose(f);
        return -1;
    }
    if (h.ram_size != (uint32_t)GUEST_RAM_SIZE) {
        fprintf(stderr, "SAVESTATE load REJECTED: state guest RAM is %u bytes, runtime has %u\n",
                h.ram_size, (uint32_t)GUEST_RAM_SIZE);
        fclose(f);
        return -1;
    }
    if (h.var_count != SS_VAR_COUNT) {
        fprintf(stderr, "SAVESTATE load REJECTED: state carries %u vars, runtime has %u\n",
                h.var_count, SS_VAR_COUNT);
        fclose(f);
        return -1;
    }
    /* host-state variables, matched by name */
    uint32_t applied = 0, skipped = 0;
    for (uint32_t i = 0; i < h.var_count; ++i) {
        uint32_t name_len = 0, size = 0;
        if (fread(&name_len, 4u, 1u, f) != 1u || fread(&size, 4u, 1u, f) != 1u ||
            name_len == 0u || name_len > 256u) {
            fprintf(stderr, "SAVESTATE load FAILED: corrupt variable record\n");
            fclose(f);
            return -1;
        }
        char name[257];
        if (fread(name, 1u, name_len, f) != name_len) { fclose(f); return -1; }
        name[name_len] = 0;
        const SavestateVar *dst = NULL;
        for (uint32_t j = 0; j < SS_VAR_COUNT; ++j)
            if (!strcmp(savestate_vars[j].name, name)) { dst = &savestate_vars[j]; break; }
        if (dst && dst->size == size) {
            if (fread(dst->addr, 1u, size, f) != size) { fclose(f); return -1; }
            ++applied;
        } else {
            if (fseeko(f, (off_t)size, SEEK_CUR)) { fclose(f); return -1; }
            ++skipped;
        }
    }
    uint32_t fs_count = 0;
    if (fread(&fs_count, 4u, 1u, f) != 1u || fs_count > 256u) {
        fprintf(stderr, "SAVESTATE load FAILED: corrupt FS handle table\n");
        fclose(f);
        return -1;
    }
    SavestateFsFile *fs_files = fs_count ? calloc(fs_count, sizeof(SavestateFsFile)) : NULL;
    if (fs_count && (!fs_files || fread(fs_files, sizeof(SavestateFsFile), fs_count, f) != fs_count)) {
        fprintf(stderr, "SAVESTATE load FAILED: truncated FS handle table\n");
        free(fs_files);
        fclose(f);
        return -1;
    }
    /* guest RAM: zero-fill, then the saved chunks */
    uint32_t count = 0;
    if (fread(&count, 4u, 1u, f) != 1u || count > SS_CHUNKS) {
        fprintf(stderr, "SAVESTATE load FAILED: corrupt RAM chunk count\n");
        free(fs_files);
        fclose(f);
        return -1;
    }
    uint32_t *indices = count ? malloc(sizeof(uint32_t) * (size_t)count) : NULL;
    if (count && !indices) { free(fs_files); fclose(f); return -1; }
    if (count && fread(indices, 4u, count, f) != count) {
        fprintf(stderr, "SAVESTATE load FAILED: truncated RAM chunk index\n");
        free(indices); free(fs_files); fclose(f); return -1;
    }
    /* A state file is an all-or-nothing restore: validate the index before
       touching guest memory, then rebuild RAM. */
    for (uint32_t i = 0; i < count; ++i) {
        if (indices[i] >= SS_CHUNKS || (i && indices[i] <= indices[i - 1u])) {
            fprintf(stderr, "SAVESTATE load FAILED: bad RAM chunk index %u\n", indices[i]);
            free(indices); free(fs_files); fclose(f);
            return -1;
        }
    }
    memset(ram, 0, GUEST_RAM_SIZE);
    for (uint32_t i = 0; i < count; ++i) {
        if (fread(ram + ((size_t)indices[i] << SS_CHUNK_LOG2), 1u, SS_CHUNK_SIZE, f) != SS_CHUNK_SIZE) {
            fprintf(stderr, "SAVESTATE load FAILED: truncated RAM chunk %u\n", indices[i]);
            free(indices); free(fs_files); fclose(f);
            return -1;
        }
    }
    uint64_t consumed = (uint64_t)ftello(f);
    fclose(f);

    /* A state is only meaningful with its host resources re-created. */
    savestate_reopen_fs(fs_files, fs_count);

    *pc_out = h.main_pc;
    memcpy(r_out, h.main_r, sizeof h.main_r);
    *n_out = (h.main_flags & 1u) != 0u;
    *z_out = (h.main_flags & 2u) != 0u;
    *c_out = (h.main_flags & 4u) != 0u;
    *v_out = (h.main_flags & 8u) != 0u;
    *ge_out = (h.main_flags & 16u) != 0u;

    /* Timing bases restored from the state are wall-clock values of the
       saving process: rebase the ones that gate pacing so the guest does not
       see a burst of "already due" deadlines. */
    if (dq8_det_mode) {
        /* DETERMINISTIC (t_115d297c): the guest clock is virtual
           (base + idle + retired * ns_per_step), while host_monotonic_ns()
           is host UPTIME.  Anchoring the pacing deadlines there put the next
           vblank hours into the virtual future, so a reloaded guest waited
           for a frame that the virtual chain could not reach: observed as
           "main retires 0 instructions, vblanks=0" with the GSP relay event
           parked forever (t_115d297c).  Arm the chain one period ahead on
           the virtual clock instead; the cadence is then a pure function of
           the state, identical for every run that loads it. */
        uint64_t vnow = dq8_guest_now_ns();
        gsp_next_vblank_ns = vnow + GSP_VBLANK_NS;
        native_hid_next_update = vnow;
        dsp_next_frame_ns = vnow;
        guest_next_host_poll_ns = 0;
    } else {
        gsp_next_vblank_ns = host_monotonic_ns();
        native_hid_next_update = host_monotonic_ns();
        dsp_next_frame_ns = host_monotonic_ns();
    }
    /* A held button from the saving run must not stick in the loaded one. */
    native_hid_buttons = 0;
    native_hid_previous = 0;
    host_shadow_dirty = 1;

    double ms = (double)(host_monotonic_ns() - t0) / 1e6;
    fprintf(stderr,
            "SAVESTATE load OK: %s  pc=%08X  threads=%u  chunks=%u  vars=%u (skipped %u)  "
            "files=%u  size=%.1f MiB  %.0f ms\n",
            path, h.main_pc, h.thread_count, count, applied, skipped, fs_count,
            (double)consumed / (1024.0 * 1024.0), ms);
    free(indices);
    free(fs_files);
    return 0;
}

/* Startup load (before the first guest instruction).  Fills r[] with the main
   context regs, returns the resume pc through *pc, and arms the flag restore
   that translated_from() applies on entry. */
static int savestate_cold_load(const char *path, uint32_t *pc, uint32_t *r) {
    int n = 0, z = 0, c = 0, v = 0, ge = 0;
    if (savestate_read(path, pc, r, &n, &z, &c, &v, &ge)) return -1;
    savestate_resume_n = n; savestate_resume_z = z; savestate_resume_c = c;
    savestate_resume_v = v; savestate_resume_ge = ge;
    savestate_resume_valid = 1;
    fprintf(stderr, "SAVESTATE: startup restore complete — boot sequence skipped, "
                    "entering guest at pc=%08X\n", *pc);
    return 0;
}

/* Scripted triggers (headless smoke / operator automation): the same code path
   F5/F9 use, fired from a wall-clock deadline instead of a key press. */
static int savestate_auto_enabled = -1;   /* -1 unknown, 0 no autosave env, 1 armed */
static void savestate_autotrigger_check(void) {
    if (!savestate_auto_t0_ns) savestate_auto_t0_ns = host_monotonic_ns();
    if (savestate_auto_saved && savestate_auto_loaded) return;
    double elapsed = (double)(host_monotonic_ns() - savestate_auto_t0_ns) / 1e9;
    const char *save_at = getenv("DQ8_SAVESTATE_AUTOSAVE_AT");
    const char *load_at = getenv("DQ8_SAVESTATE_AUTOLOAD_AT");
    if (!savestate_auto_saved && save_at && *save_at && elapsed >= atof(save_at)) {
        savestate_auto_saved = 1;
        savestate_request(1);
    }
    if (!savestate_auto_loaded && load_at && *load_at && elapsed >= atof(load_at)) {
        savestate_auto_loaded = 1;
        savestate_request(2);
    }
}

/* Called at every main-context dispatch boundary.  Returns 1 when a state was
   loaded (pc/regs/flags replaced), 0 otherwise. */
static int savestate_service(uint32_t *pc, uint32_t *r,
                             int *n, int *z, int *c, int *v, int *ge) {
    /* Hot path: this runs at every main-context dispatch boundary, so the
       no-request case is a single relaxed load (a locked exchange here would
       tax the whole dispatch loop) plus one tri-state test for the scripted
       autosave hooks. */
    if (savestate_auto_enabled != 0) {
        if (savestate_auto_enabled < 0) {
            const char *a = getenv("DQ8_SAVESTATE_AUTOSAVE_AT");
            const char *b = getenv("DQ8_SAVESTATE_AUTOLOAD_AT");
            savestate_auto_enabled = ((a && *a) || (b && *b)) ? 1 : 0;
        }
        if (savestate_auto_enabled) savestate_autotrigger_check();
    }
    if (!atomic_load_explicit(&savestate_pending, memory_order_relaxed)) return 0;
    int kind = atomic_exchange(&savestate_pending, 0);
    if (kind != 1 && kind != 2) return 0;
    if (savestate_busy) return 0;   /* re-entrancy guard */
    savestate_busy = 1;
    const char *path = savestate_default_path();
    if (kind == 1) {
        savestate_write(path, *pc, r, *n, *z, *c, *v, *ge);
        savestate_busy = 0;
        return 0;
    }
    uint32_t npc = *pc;
    uint32_t regs[16];
    int nn = *n, nz = *z, nc = *c, nv = *v, nge = *ge;
    memcpy(regs, r, sizeof regs);
    if (savestate_read(path, &npc, regs, &nn, &nz, &nc, &nv, &nge)) {
        savestate_busy = 0;
        fprintf(stderr, "SAVESTATE: load request ignored, guest continues at pc=%08X\n", *pc);
        return 0;
    }
    *pc = npc;
    memcpy(r, regs, sizeof regs);
    *n = nn; *z = nz; *c = nc; *v = nv; *ge = nge;
    savestate_busy = 0;
    return 1;
}
