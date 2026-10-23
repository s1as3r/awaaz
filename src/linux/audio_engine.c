// linux impl. pulseaudio / pipewire-pulse

#include <pulse/pulseaudio.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>

#include <bace/bace.h>

#include "../audio_engine.h"

//- tuning

#define AE_SAMPLE_RATE 48000
#define AE_CHANNELS 2
#define AE_FRAME_BYTES (AE_CHANNELS * sizeof(f32))

// size of the chunks the capture stream hands us
#define AE_PA_PACKET_MS 5

// target amount of audio buffered server-side per playback stream.
// two packets = one playing, one arriving.
// pulse waits until this much is buffered before starting playback,
// which acts as our cushion, and re-buffers the same amount after an underrun.
#define AE_PA_TLENGTH_MS (AE_PA_PACKET_MS * 2)

//- state

typedef struct {
  pa_stream *stream;
} OutputCtx;

global pa_threaded_mainloop *g_ml = NULL;
global bool g_ml_started = false;
global pa_context *g_ctx = NULL;
global pa_stream *g_cap = NULL;

global OutputCtx *g_outputs = NULL;
global u32 g_output_count = 0;
global atomic_bool g_running = 0;

#define AE_FAIL(...) (ae_set_error(__VA_ARGS__), false)

bool ae_is_running(void) {
  return atomic_load(&g_running);
}

//- enumeration

// bounded so a dead server can never hang the ui
#define AE_MAX_DEVICES 64
#define PA_WAIT_BUDGET 100 // x 50ms poll = 5s worst case

typedef struct {
  AeDeviceList *out;
  Arena *arena;
} PaListState;

// runs one iteration of a plain mainloop with a 50 ms timeout.
// returns `false` when the budget is exhausted or the loop errors, which lets
// callers bail out instead of spinning forever.
internal bool pa_pump(pa_mainloop *m, i32 *budget) {
  if ((*budget)-- <= 0) {
    return false;
  }
  if (pa_mainloop_prepare(m, 50 * 1000) < 0) {
    return false;
  }
  if (pa_mainloop_poll(m) < 0) {
    return false;
  }
  return pa_mainloop_dispatch(m) >= 0;
}

internal void pa_sink_cb(pa_context *c, const pa_sink_info *i, int eol,
                         void *userdata) {
  (void)c;
  PaListState *s = (PaListState *)userdata;
  if (eol != 0 || !i) {
    return; // eol > 0: end of list, < 0: error
  }
  if (s->out->count >= AE_MAX_DEVICES) {
    return;
  }

  AeDeviceInfo *info = &s->out->devices[s->out->count];
  info->id = str8f(s->arena, "%s", i->name ? i->name : "");
  info->name = str8f(s->arena, "%s",
                     i->description ? i->description : "(unknown device)");
  s->out->count += 1;
}

internal void pa_enum_state_cb(pa_context *c, void *userdata) {
  i32 *status = (i32 *)userdata;
  switch (pa_context_get_state(c)) {
  case PA_CONTEXT_READY: {
    *status = 1;
  } break;
  case PA_CONTEXT_FAILED:
  case PA_CONTEXT_TERMINATED: {
    *status = -1;
  } break;
  default: {
  } break;
  }
}

AeDeviceList ae_enumerate_render_devices(Arena *arena) {
  AeDeviceList out = {0};

  pa_mainloop *m = pa_mainloop_new();
  if (!m) {
    return out;
  }
  pa_context *ctx = pa_context_new(pa_mainloop_get_api(m), "awaaz");
  if (!ctx) {
    pa_mainloop_free(m);
    return out;
  }

  i32 ctx_status = 0;
  pa_context_set_state_callback(ctx, pa_enum_state_cb, &ctx_status);

  if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
    goto done;
  }

  i32 budget = PA_WAIT_BUDGET;
  while (ctx_status == 0) { // wait for the connection
    if (!pa_pump(m, &budget)) {
      ctx_status = -1;
    }
  }
  if (ctx_status != 1) {
    goto done;
  }

  out.devices = push_array_no_zero(arena, AeDeviceInfo, AE_MAX_DEVICES);
  PaListState st = {&out, arena};
  pa_operation *op = pa_context_get_sink_info_list(ctx, pa_sink_cb, &st);
  if (op) {
    budget = PA_WAIT_BUDGET;
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
      if (!pa_pump(m, &budget)) {
        break;
      }
    }
    pa_operation_unref(op);
  }

done:
  pa_context_disconnect(ctx);
  pa_context_unref(ctx);
  pa_mainloop_free(m);
  return out;
}

//- callbacks

// both state callbacks just wake whichever thread is blocked in
// `pa_threaded_mainloop_wait()` during setup
internal void ctx_state_cb(pa_context *c, void *ud) {
  (void)c;
  (void)ud;
  pa_threaded_mainloop_signal(g_ml, 0);
}

internal void stream_state_cb(pa_stream *s, void *ud) {
  (void)s;
  (void)ud;
  pa_threaded_mainloop_signal(g_ml, 0);
}

// called by pulse whenever captured audio is available
internal void capture_read_cb(pa_stream *s, size_t nbytes, void *ud) {
  (void)nbytes;
  (void)ud;

  while (pa_stream_readable_size(s) > 0) {
    const void *data = NULL;
    size_t len = 0;
    if (pa_stream_peek(s, &data, &len) < 0 || len == 0) {
      break;
    }

    if (data) {
      const f32 *f = (const f32 *)data;
      u32 nframes = (u32)(len / AE_FRAME_BYTES);

      for (u32 o = 0; o < g_output_count; o += 1) {
        pa_stream *out = g_outputs[o].stream;

        size_t writable = pa_stream_writable_size(out);
        if (writable == (size_t)-1 || writable < len) {
          continue;
        }
        pa_stream_write(out, data, len, NULL, 0, PA_SEEK_RELATIVE);
      }
    }
    pa_stream_drop(s);
  }
}

//- setup / teardown

// loop lock must be held.
// blocks until `PA_STREAM_READY` or failure.
internal bool wait_stream_ready(pa_stream *s) {
  for (;;) {
    pa_stream_state_t st = pa_stream_get_state(s);
    if (st == PA_STREAM_READY) {
      return true;
    }
    if (!PA_STREAM_IS_GOOD(st)) {
      return false;
    }
    pa_threaded_mainloop_wait(g_ml);
  }
}

// loop lock must be held
internal void drop_stream(pa_stream *s) {
  pa_stream_set_read_callback(s, NULL, NULL);
  pa_stream_set_state_callback(s, NULL, NULL);
  if (pa_stream_get_state(s) == PA_STREAM_READY) {
    pa_stream_disconnect(s);
  }
  pa_stream_unref(s);
}

internal void ae_teardown(void) {
  if (g_ml) {
    pa_threaded_mainloop_lock(g_ml);
    if (g_cap) {
      drop_stream(g_cap);
      g_cap = NULL;
    }
    for (u32 i = 0; i < g_output_count; i += 1) {
      if (g_outputs[i].stream) {
        drop_stream(g_outputs[i].stream);
      }
    }
    if (g_ctx) {
      pa_context_set_state_callback(g_ctx, NULL, NULL);
      pa_context_disconnect(g_ctx);
      pa_context_unref(g_ctx);
      g_ctx = NULL;
    }
    pa_threaded_mainloop_unlock(g_ml);

    if (g_ml_started) {
      pa_threaded_mainloop_stop(g_ml);
    }
    pa_threaded_mainloop_free(g_ml);
    g_ml = NULL;
    g_ml_started = false;
  }

  g_output_count = 0;
  g_outputs = NULL;

  atomic_store(&g_running, false);
}

void ae_stop(void) {
  if (atomic_load(&g_running)) {
    ae_teardown();
  }
}

internal bool setup_locked(Arena *outputs_arena, Str8 capture_device_id,
                           const Str8 *output_ids, u32 output_count) {
  for (;;) { // wait for the server connection
    pa_context_state_t st = pa_context_get_state(g_ctx);
    if (st == PA_CONTEXT_READY)
      break;
    if (!PA_CONTEXT_IS_GOOD(st))
      return AE_FAIL("could not connect to pulseaudio");
    pa_threaded_mainloop_wait(g_ml);
  }

  pa_sample_spec ss = {.format = PA_SAMPLE_FLOAT32LE,
                       .rate = AE_SAMPLE_RATE,
                       .channels = AE_CHANNELS};

  u32 packet_bytes =
      (u32)(AE_SAMPLE_RATE * AE_PA_PACKET_MS / 1000 * AE_FRAME_BYTES);
  u32 tlength_bytes =
      (u32)(AE_SAMPLE_RATE * AE_PA_TLENGTH_MS / 1000 * AE_FRAME_BYTES);

  g_outputs = push_array_no_zero(outputs_arena, OutputCtx, output_count);
  g_output_count = 0;

  for (u32 i = 0; i < output_count; i += 1) {
    OutputCtx *oc = &g_outputs[g_output_count];
    memset(oc, 0, sizeof(*oc));

    oc->stream = pa_stream_new(g_ctx, "playback", &ss, NULL);
    if (!oc->stream) {
      trace_log("[warning] pa_stream_new failed for output %u\n", i);
      continue;
    }
    pa_stream_set_state_callback(oc->stream, stream_state_cb, NULL);

    pa_buffer_attr attr = {.maxlength = (u32)-1,
                           .tlength = tlength_bytes,
                           .prebuf = tlength_bytes,
                           .minreq = (u32)-1,
                           .fragsize = (u32)-1};

    if (pa_stream_connect_playback(
            oc->stream, (char *)output_ids[i].str, &attr,
            PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE, NULL, NULL) < 0 ||
        !wait_stream_ready(oc->stream)) {
      trace_log("[warning] could not open output %u: %s\n", i,
                pa_strerror(pa_context_errno(g_ctx)));
      drop_stream(oc->stream);
      oc->stream = NULL;
      continue;
    }

    g_output_count += 1;
  }

  if (g_output_count == 0) {
    return AE_FAIL("no output devices could be opened");
  }

  char monitor_id[512];
  snprintf(monitor_id, sizeof(monitor_id), "%.*s.monitor",
           (int)capture_device_id.size, capture_device_id.str);

  g_cap = pa_stream_new(g_ctx, "capture", &ss, NULL);
  if (!g_cap) {
    return AE_FAIL("pa_stream_new(capture) failed");
  }
  pa_stream_set_state_callback(g_cap, stream_state_cb, NULL);
  pa_stream_set_read_callback(g_cap, capture_read_cb, NULL);

  pa_buffer_attr rattr = {.maxlength = (u32)-1,
                          .tlength = (u32)-1,
                          .prebuf = (u32)-1,
                          .minreq = (u32)-1,
                          .fragsize = packet_bytes};
  if (pa_stream_connect_record(g_cap, monitor_id, &rattr,
                               PA_STREAM_ADJUST_LATENCY) < 0 ||
      !wait_stream_ready(g_cap)) {
    return AE_FAIL("capture: %s", pa_strerror(pa_context_errno(g_ctx)));
  }

  return true;
}

bool ae_start(Arena *cap_arena, Arena *outputs_arena, Str8 capture_device_id,
              const Str8 *output_ids, u32 output_count) {
  g_ae_error[0] = 0;
  if (ae_is_running()) {
    return AE_FAIL("already running");
  }

  g_ml = pa_threaded_mainloop_new();
  if (g_ml) {
    g_ctx = pa_context_new(pa_threaded_mainloop_get_api(g_ml), "awaaz");
  }
  if (!g_ml || !g_ctx) {
    ae_set_error("could not create pulseaudio mainloop/context");
    goto fail;
  }

  pa_context_set_state_callback(g_ctx, ctx_state_cb, NULL);
  if (pa_context_connect(g_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
    ae_set_error("pa_context_connect: %s",
                 pa_strerror(pa_context_errno(g_ctx)));
    goto fail;
  }

  pa_threaded_mainloop_lock(g_ml);
  if (pa_threaded_mainloop_start(g_ml) < 0) {
    pa_threaded_mainloop_unlock(g_ml);
    ae_set_error("could not start pulseaudio mainloop");
    goto fail;
  }
  g_ml_started = true;

  bool ok =
      setup_locked(outputs_arena, capture_device_id, output_ids, output_count);
  pa_threaded_mainloop_unlock(g_ml);
  if (ok) {
    atomic_store(&g_running, true);
    return true;
  }

fail:
  ae_teardown();
  arena_pop_to(cap_arena, 0);
  arena_pop_to(outputs_arena, 0);
  return false;
}
