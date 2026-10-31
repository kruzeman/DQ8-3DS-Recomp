#!/usr/bin/env python3
"""Exercise the real emitted DSP frame tick and bounded catch-up policy."""

from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <assert.h>
_Static_assert(DSP_FRAME_NS == 4888791ull, "DSP quantum changed");
static void test_source_boundary_delivery(uint32_t event) {
    DspWorkerSource finite = {0}, looping = {0};
    uint64_t starts_before = dsp_sources_started, finishes_before = dsp_sources_finished;
    finite.rate = looping.rate = 1.0f;
    for (unsigned frame = 0; frame < 2u; ++frame) {
        DspAudioJob *job = calloc(1, sizeof(*job));
        assert(job);
        job->notify_guest = 1; job->write_region = DSP_REGION0; job->master = 1.0f;
        if (frame == 0u) {
            DspWorkerInput *in = &job->source[DSP_MUSIC_SOURCE];
            in->enabled = 1; in->enabled_dirty = 1; in->rate = 1.0f; in->rate_dirty = 1;
            in->gain[0][0] = in->gain[0][1] = 1.0f; in->gain_dirty = 1;
            in->pending_count = 2u; in->pending = calloc(2u, sizeof(*in->pending));
            assert(in->pending);
            for (unsigned b = 0; b < 2u; ++b) {
                DspPending *p = &in->pending[b];
                p->length = DSP_FRAME_SAMPLES; p->channels = 1u; p->format = 1u;
                p->buffer_id = (uint16_t)(11u + b); p->payload_bytes = DSP_FRAME_SAMPLES * 2u;
                p->payload = malloc(p->payload_bytes); assert(p->payload);
                for (unsigned n = 0; n < p->payload_bytes; ++n) p->payload[n] = (uint8_t)(n + b);
            }
        }
        dsp_process_source(DSP_MUSIC_SOURCE, &finite, &job->source[DSP_MUSIC_SOURCE], job);
        if (frame == 0u) {
            assert(job->sources_started == 2u && job->sources_finished == 1u);
            assert(job->result[DSP_MUSIC_SOURCE].buffer_update == 1u);
            assert(job->result[DSP_MUSIC_SOURCE].current_buffer_id == 12u);
        } else {
            assert(job->sources_finished == 1u);
            assert(job->result[DSP_MUSIC_SOURCE].buffer_update == 1u);
            assert(job->result[DSP_MUSIC_SOURCE].current_buffer_id == 0u);
            assert(job->result[DSP_MUSIC_SOURCE].last_buffer_id == 12u);
        }
        assert(svc_clear_event(event) == 0u);
        dsp_audio_commit(job);
        assert(event_signaled[event] == 1u);
    }
    assert(dsp_r16(DSP_REGION0 + DSP_OFF_SRC_STATUS + DSP_MUSIC_SOURCE * 12u + 8u) == 0u);
    assert(dsp_r16(DSP_REGION0 + DSP_OFF_SRC_STATUS + DSP_MUSIC_SOURCE * 12u + 10u) == 12u);

    for (unsigned frame = 0; frame < 2u; ++frame) {
        DspAudioJob *job = calloc(1, sizeof(*job));
        DspWorkerInput *in;
        assert(job);
        job->notify_guest = 1; job->write_region = DSP_REGION0; job->master = 1.0f;
        in = &job->source[DSP_MUSIC_SOURCE];
        if (frame == 0u) {
            in->enabled = 1; in->enabled_dirty = 1; in->rate = 1.0f; in->rate_dirty = 1;
            in->gain[0][0] = in->gain[0][1] = 1.0f; in->gain_dirty = 1;
            in->pending_count = 1u; in->pending = calloc(1u, sizeof(*in->pending));
            assert(in->pending);
            in->pending[0].length = DSP_FRAME_SAMPLES; in->pending[0].channels = 1u;
            in->pending[0].format = 1u; in->pending[0].looping = 1u; in->pending[0].buffer_id = 33u;
            in->pending[0].payload_bytes = DSP_FRAME_SAMPLES * 2u;
            in->pending[0].payload = calloc(1u, in->pending[0].payload_bytes);
            assert(in->pending[0].payload);
        }
        dsp_process_source(DSP_MUSIC_SOURCE, &looping, in, job);
        assert(job->sources_finished == 1u); /* once per completed loop */
        assert(job->result[DSP_MUSIC_SOURCE].current_buffer_id == 33u);
        if (frame == 1u) assert(job->result[DSP_MUSIC_SOURCE].buffer_update == 0u);
        assert(svc_clear_event(event) == 0u);
        dsp_audio_commit(job);
        assert(event_signaled[event] == 1u);
    }
    assert(dsp_sources_started - starts_before == 3u);
    assert(dsp_sources_finished - finishes_before == 4u);
    dsp_worker_source_free_current(&finite);
    dsp_worker_source_free_pending(&finite);
    dsp_worker_source_free_current(&looping);
    dsp_worker_source_free_pending(&looping);
}
static void test_status_commit_barrier(uint32_t event) {
    uint64_t frames_before = dsp_frames_out;
    DspAudioJob *boundary = calloc(1, sizeof(*boundary));
    DspAudioJob *following = calloc(1, sizeof(*following));
    uint32_t status0 = DSP_REGION0 + DSP_OFF_SRC_STATUS + DSP_MUSIC_SOURCE * 12u;
    uint32_t status1 = DSP_REGION1 + DSP_OFF_SRC_STATUS + DSP_MUSIC_SOURCE * 12u;
    assert(boundary && following);
    dsp_w16(DSP_REGION0 + DSP_OFF_FRAME_COUNTER, 0u);
    dsp_w16(DSP_REGION1 + DSP_OFF_FRAME_COUNTER, 0u);
    dsp_w16(DSP_REGION0 + DSP_OFF_DSP_CONFIG, 0u);
    dsp_w16(DSP_REGION1 + DSP_OFF_DSP_CONFIG, 0u);
    dsp_audio_guest_region = DSP_REGION1;
    boundary->write_region = following->write_region = DSP_REGION0;
    boundary->notify_guest = following->notify_guest = 1;
    boundary->result[DSP_MUSIC_SOURCE].buffer_update = 1u;
    boundary->result[DSP_MUSIC_SOURCE].current_buffer_id = 77u;
    boundary->sources_finished = 1u;
    following->result[DSP_MUSIC_SOURCE].current_buffer_id = 88u;
    assert(dq8_spsc_try_push(&dsp_completions, (uintptr_t)boundary));
    assert(dq8_spsc_try_push(&dsp_completions, (uintptr_t)following));
    assert(svc_clear_event(event) == 0u);
    assert(dsp_audio_commit_ready() == 1u);
    assert(dsp_frames_out == frames_before + 1u);
    assert(dsp_r16(status0 + 8u) == 77u);
    assert(event_signaled[event] == 1u);
    assert(dsp_source_event_pending == 1);
    assert(dsp_audio_commit_ready() == 0u); /* wait for guest ClearEvent ack */
    /* The second worker job also captured region0 at submit time, but the
       guest has advanced to region0; the result must target live region1. */
    dsp_audio_guest_region = DSP_REGION0;
    assert(svc_clear_event(event) == 0u);
    assert(dsp_source_event_pending == 0);
    assert(dsp_audio_commit_ready() == 1u);
    assert(dsp_frames_out == frames_before + 2u);
    assert(dsp_r16(status0 + 8u) == 77u);
    assert(dsp_r16(status1 + 8u) == 88u);
}
static void wait_for_frames(uint64_t target) {
    struct timespec pause = {0, 100000};
    uint64_t until = host_monotonic_ns() + 2000000000ull;
    while (dsp_frames_out < target && host_monotonic_ns() < until) {
        dsp_audio_tick();
        nanosleep(&pause, NULL);
    }
    assert(dsp_frames_out >= target);
}
int main(void) {
    unsetenv("DQ8_TRACE_AUDIO");
    setenv("DQ8_AUDIO_SDL", "0", 1);
    int16_t pcm_probe[DSP_FRAME_SAMPLES * 2u] = {0};
    assert(!dsp_audio_pcm_active(pcm_probe));
    pcm_probe[0] = 1;
    assert(dsp_audio_pcm_active(pcm_probe));
    ram = calloc(1, GUEST_RAM_SIZE);
    assert(ram);

    /* Two copied source buffers deliberately live in mutable guest RAM. */
    const uint32_t sfx_data = 0x1FF10000u, music_data = 0x1FF20000u;
    for (unsigned i = 0; i < 20000u; ++i) {
        dsp_w16(sfx_data + i * 2u, 3000u);
        dsp_w16(music_data + i * 2u, 5000u);
    }
    uint32_t app_cfg = DSP_REGION1 + DSP_OFF_APP_DSP_CONFIG;
    dsp_w32(app_cfg, 1u << 16);
    { float v = 1.0f; uint32_t bits; memcpy(&bits, &v, sizeof(bits)); dsp_w32(app_cfg + 4u, bits); }
    for (unsigned src = 0; src <= 1u; ++src) {
        uint32_t cfg = DSP_REGION1 + DSP_OFF_SRC_CONFIG + src * 192u;
        uint32_t addr = src ? music_data : sfx_data;
        dsp_w32(cfg, (1u << 16) | (1u << 18) | (1u << 25) | (1u << 30));
        { float v = 1.0f; uint32_t bits; memcpy(&bits, &v, sizeof(bits)); dsp_w32(cfg + 52u, bits); }
        dsp_w16(cfg + 180u, 4u); /* mono PCM16 */
        dsp_w32(cfg + 172u, (addr << 16) | (addr >> 16));
        dsp_w32(cfg + 176u, (20000u << 16) | (20000u >> 16));
        dsp_w16(cfg + 182u, 0u); dsp_w16(cfg + 184u, 0u); dsp_w16(cfg + 186u, 0u);
        dsp_w16(cfg + 190u, (uint16_t)(7u + src));
        dsp_w32(cfg + 4u, 0x3F800000u); /* left gain */
        dsp_w32(cfg + 8u, 0x3F800000u); /* right gain */
        dsp_w32(cfg + 160u, 1u);
    }

    /* Disabled audio does not service a stale/past deadline. */
    dsp_audio_on = 0;
    dsp_next_frame_ns = host_monotonic_ns() - 1u;
    dsp_audio_tick();
    assert(dsp_frames_out == 0u && perf_dsp_ticks == 0u && perf_dsp_resyncs == 0u);

    /* A future deadline does not produce a frame or signal an event. */
    uint32_t regs[16] = {0};
    regs[1] = 0u;
    assert(svc_create_event(regs) == 0u);
    dsp_pipe_interrupt_event = regs[1];
    dsp_audio_on = 1;
    dsp_next_frame_ns = host_monotonic_ns() + DSP_FRAME_NS * 100u;
    dsp_audio_tick();
    assert(dsp_frames_out == 0u && perf_dsp_ticks == 0u);
    assert(event_signaled[dsp_pipe_interrupt_event] == 0u);

    /* The delayed workers own copies: post source 0 (SFX) and source 1
       (music), then mutate all guest bytes before either consumer runs. */
    setenv("DQ8_AUDIO_TEST_DELAY_NS", "20000000", 1);
    dsp_next_frame_ns = host_monotonic_ns() - 1u;
    dsp_audio_tick();
    dsp_audio_on = 0; /* drain the deliberately delayed frame without scheduling new ones */
    uint32_t read_region = dsp_current_region();
    uint32_t write_region = (read_region == DSP_REGION0) ? DSP_REGION1 : DSP_REGION0;
    assert(dsp_frames_out == 0u && event_signaled[dsp_pipe_interrupt_event] == 0u);
    memset(ram + sfx_data, 0, 40000u);
    memset(ram + music_data, 0, 40000u);
    memset(ram + DSP_REGION1 + DSP_OFF_SRC_CONFIG, 0, 2u * 192u);
    wait_for_frames(1u);
    assert(dsp_frames_out == 1u && perf_dsp_ticks == 1u && perf_dsp_resyncs == 0u);
    assert(event_signaled[dsp_pipe_interrupt_event] == 1u);
    assert(dsp_r16(write_region + DSP_OFF_FINAL) == 8000u);
    assert(dsp_r16(write_region + DSP_OFF_FINAL + 2u) == 8000u);
    assert(dsp_r16(write_region + DSP_OFF_SRC_STATUS + 8u) == 7u);
    assert(dsp_r16(write_region + DSP_OFF_SRC_STATUS + 12u + 8u) == 8u);
    assert(dsp_audio_deadline_misses >= 1u); /* intentional delayed refill */
    assert(dsp_audio_sink_checks == 0u && dsp_audio_sink_empty == 0u); /* SDL is disabled */
    unsetenv("DQ8_AUDIO_TEST_DELAY_NS");
    dsp_audio_on = 1;

    /* Ten overdue quanta are capped at eight frames, then resynchronized one
       quantum ahead; guest notification remains a single tick completion. */
    assert(svc_clear_event(dsp_pipe_interrupt_event) == 0u);
    uint64_t frames_before = dsp_frames_out;
    uint64_t ticks_before = perf_dsp_ticks;
    uint64_t resync_before = perf_dsp_resyncs;
    dsp_next_frame_ns = host_monotonic_ns() - DSP_FRAME_NS * 10u;
    dsp_audio_tick();
    wait_for_frames(frames_before + 8u);
    assert(dsp_frames_out == frames_before + 8u);
    assert(perf_dsp_ticks == ticks_before + 1u);
    assert(perf_dsp_resyncs == resync_before + 1u);
    assert(dsp_next_frame_ns > host_monotonic_ns());
    assert(event_signaled[dsp_pipe_interrupt_event] == 1u);

    /* Verify guest-visible status and the audio-pipe IRQ at queued-buffer and
       loop ends, not just at an arbitrary audio frame. */
    dsp_audio_workers_shutdown();
    test_source_boundary_delivery(dsp_pipe_interrupt_event);
    test_status_commit_barrier(dsp_pipe_interrupt_event);

    puts("dsp_service: frame tick/resync, source events and status commit barrier OK");
    free(ram);
    return 0;
}
'''


class DspServiceTest(unittest.TestCase):
    def test_tick_deadline_event_and_resync(self):
        tool = Path(os.environ.get("DQ8_TOOL", ROOT / "target/release/dq8tool"))
        with tempfile.TemporaryDirectory(prefix="dq8-dsp-service-") as tmp:
            tmp = Path(tmp)
            code = tmp / "stub.code"
            source = tmp / "stub.c"
            binary = tmp / "test"
            code.write_bytes(bytes.fromhex("1eff2fe1"))
            subprocess.run(
                [str(tool), "recompile-arm", str(code), "0x00100000", str(source)],
                check=True, capture_output=True,
            )
            runtime = source.read_text().replace(
                "int main(int argc, char **argv)",
                "int guest_main(int argc, char **argv)",
                1,
            )
            source.write_text(runtime + FIXTURE)
            subprocess.run(
                ["clang", "-std=c11", "-O0", "-w", "-DDQ8_AUDIO_TESTING", str(source), "-lm", "-o", str(binary)],
                check=True,
            )
            env = dict(os.environ)
            env["DQ8_RUN_GUEST_THREADS"] = "1"
            env["DQ8_AUDIO_SDL"] = "0"
            wav = tmp / "audio.wav"
            env["DQ8_AUDIO_DUMP"] = str(wav)
            subprocess.run([str(binary)], check=True, timeout=30, env=env)
            data = wav.read_bytes()
            self.assertEqual(data[:4], b"RIFF")
            self.assertEqual(struct.unpack_from("<I", data, 4)[0], len(data) - 8)
            self.assertEqual(struct.unpack_from("<I", data, 40)[0], len(data) - 44)


if __name__ == "__main__":
    unittest.main()
