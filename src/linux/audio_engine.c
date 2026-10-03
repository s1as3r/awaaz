#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include <threads.h>
#include <stdatomic.h>
#include <string.h>

#include <bace/bace.h>

#include "../audio_engine.h"
#include "../ring_buffer.h"

#define AE_SAMPLE_RATE 48000
#define AE_CHANNELS 2

// per-output state
typedef struct {
  pa_simple *pa;
  thrd_t thread;
  bool thread_valid;
  RingBuffer rb;
  atomic_bool stop_flag;

  // pre-allocated buffer
  f32 *io_buf;
  u32 io_buf_bytes;
} OutputCtx;

// capture state
typedef struct {
  pa_simple *pa;
  thrd_t thread;
  bool thread_valid;
  atomic_bool stop_flag;

  // pre-allocated buffer
  f32 *io_buf;
  u32 io_buf_bytes;
} CaptureCtx;

// engine wide state
global CaptureCtx g_capture_ctx = {0};

global OutputCtx *g_outputs = NULL;
global u32 g_output_count = 0;

global atomic_bool g_running = 0;
global volatile f32 g_peak = 0.0f;

// device enumeration

typedef struct {
  u32 count;
  bool is_done;
} PaCountState;

internal void pa_count_cb(pa_context *c, const pa_sink_info *i, int eol,
                          void *userdata) {
  (void)c;
  PaCountState *state = (PaCountState *)userdata;
  if (eol > 0) {
    state->is_done = true;
    return;
  }
  if (i) {
    state->count += 1;
  }
}

typedef struct {
  AeDeviceList *out;
  Arena *arena;
  u32 capacity;
  bool is_done;
} PaPopulateState;

internal void pa_populate_cb(pa_context *c, const pa_sink_info *i, int eol,
                             void *userdata) {
  (void)c;
  PaPopulateState *state = (PaPopulateState *)userdata;
  if (eol > 0) {
    state->is_done = true;
    return;
  }

  if (i && state->out->count < state->capacity) {
    AeDeviceInfo *info = &state->out->devices[state->out->count];
    info->id = str8f(state->arena, "%s", i->name ? i->name : "");
    info->name = str8f(state->arena, "%s",
                       i->description ? i->description : "(unknown device)");
    state->out->count += 1;
  }
}

internal void pa_state_cb(pa_context *c, void *userdata) {
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

  pa_mainloop_api *api = pa_mainloop_get_api(m);
  pa_context *ctx = pa_context_new(api, "awaaz");

  if (!ctx || pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
    if (ctx) {
      pa_context_unref(ctx);
    }
    pa_mainloop_free(m);
    return out;
  }

  i32 ctx_status = 0;
  pa_context_set_state_callback(ctx, pa_state_cb, &ctx_status);

  while (ctx_status == 0) {
    pa_mainloop_iterate(m, 1, NULL);
  }

  if (ctx_status == 1) {
    PaCountState count_state = {0};
    pa_operation *op =
        pa_context_get_sink_info_list(ctx, pa_count_cb, &count_state);
    if (op) {
      while (!count_state.is_done) {
        pa_mainloop_iterate(m, 1, NULL);
      }
      pa_operation_unref(op);
    }

    if (count_state.count > 0) {
      out.devices = push_array_no_zero(arena, AeDeviceInfo, count_state.count);
      PaPopulateState pop_state = {&out, arena, count_state.count, 0};

      op = pa_context_get_sink_info_list(ctx, pa_populate_cb, &pop_state);
      if (op) {
        while (!pop_state.is_done) {
          pa_mainloop_iterate(m, 1, NULL);
        }
        pa_operation_unref(op);
      }
    }
  }

  pa_context_disconnect(ctx);
  pa_context_unref(ctx);
  pa_mainloop_free(m);

  return out;
}

internal int output_thread_proc(void *param) {
  OutputCtx *oc = (OutputCtx *)param;
  u32 nframes = oc->io_buf_bytes / (AE_CHANNELS * sizeof(f32));

  while (!atomic_load(&oc->stop_flag)) {
    if (oc->rb.available >= nframes) {
      rb_read(&oc->rb, oc->io_buf, nframes);
    } else {
      memset(oc->io_buf, 0, oc->io_buf_bytes);
    }

    int err;
    if (pa_simple_write(oc->pa, oc->io_buf, oc->io_buf_bytes, &err) < 0) {
      break;
    }
  }
  return 0;
}

internal int capture_thread_proc(void *param) {
  (void)param;
  u32 nframes = g_capture_ctx.io_buf_bytes / (AE_CHANNELS * sizeof(f32));

  while (!atomic_load(&g_capture_ctx.stop_flag)) {
    int err;
    if (pa_simple_read(g_capture_ctx.pa, g_capture_ctx.io_buf,
                       g_capture_ctx.io_buf_bytes, &err) < 0) {
      break;
    }

    f32 peak = 0.0f;
    for (u32 i = 0; i < nframes * AE_CHANNELS; i += 1) {
      f32 a = fabsf(g_capture_ctx.io_buf[i]);
      if (a > peak) {
        peak = a;
      }
    }
    g_peak = peak;

    for (u32 o = 0; o < g_output_count; o += 1) {
      rb_write(&g_outputs[o].rb, g_capture_ctx.io_buf, nframes);
    }
  }
  return 0;
}

internal void free_output(OutputCtx *oc) {
  if (oc->pa) {
    pa_simple_free(oc->pa);
  }
  if (oc->rb.buf) {
    rb_free(&oc->rb);
  }
  memset(oc, 0, sizeof(*oc));
}

internal void free_capture(CaptureCtx *cc) {
  if (cc->pa) {
    pa_simple_free(cc->pa);
  }
  memset(cc, 0, sizeof(*cc));
}

void ae_stop(void) {
  if (!atomic_load(&g_running)) {
    return;
  }

  atomic_store(&g_capture_ctx.stop_flag, true);
  for (u32 i = 0; i < g_output_count; i += 1) {
    atomic_store(&g_outputs[i].stop_flag, true);
  }

  if (g_capture_ctx.thread_valid) {
    thrd_join(g_capture_ctx.thread, NULL);
    g_capture_ctx.thread_valid = false;
  }

  for (u32 i = 0; i < g_output_count; i += 1) {
    if (g_outputs[i].thread_valid) {
      thrd_join(g_outputs[i].thread, NULL);
      g_outputs[i].thread_valid = false;
    }
    free_output(&g_outputs[i]);
  }

  g_output_count = 0;
  free_capture(&g_capture_ctx);
  atomic_store(&g_running, false);
}

bool ae_is_running(void) {
  return atomic_load(&g_running);
}

bool ae_start(Arena *cap_arena, Arena *outputs_arena, Str8 capture_device_id,
              const Str8 *output_ids, u32 output_count) {
  if (ae_is_running()) {
    ae_stop();
    arena_pop_to(cap_arena, 0);
    arena_pop_to(outputs_arena, 0);
  }

  pa_sample_spec ss = {.format = PA_SAMPLE_FLOAT32LE,
                       .rate = AE_SAMPLE_RATE,
                       .channels = AE_CHANNELS};

  u32 chunk_frames = (AE_SAMPLE_RATE * AE_BUFFER_TIME_MS) / 1000;
  u32 chunk_bytes = chunk_frames * AE_CHANNELS * sizeof(f32);

  pa_buffer_attr attr = {
      .maxlength = (u32)-1,
      .tlength = (u32)pa_usec_to_bytes(AE_BUFFER_TIME_MS * 1000, &ss),
      .prebuf = (u32)-1,
      .minreq = (u32)-1,
      .fragsize = chunk_bytes};

  g_capture_ctx.io_buf =
      push_array_no_zero(cap_arena, f32, chunk_frames * AE_CHANNELS);
  g_capture_ctx.io_buf_bytes = chunk_bytes;

  char monitor_id[512];
  snprintf(monitor_id, sizeof(monitor_id), "%.*s.monitor",
           (int)capture_device_id.size, capture_device_id.str);

  int err;
  g_capture_ctx.pa = pa_simple_new(NULL, "awaaz", PA_STREAM_RECORD, monitor_id,
                                   "capture", &ss, NULL, &attr, &err);
  if (!g_capture_ctx.pa) {
    trace_log("[error] initialize(capture) failed: %s\n", pa_strerror(err));
    return false;
  }

  g_outputs = push_array_no_zero(outputs_arena, OutputCtx, output_count);
  g_output_count = 0;

  for (u32 i = 0; i < output_count; i += 1) {
    OutputCtx *oc = &g_outputs[g_output_count];
    memset(oc, 0, sizeof(*oc));

    oc->io_buf =
        push_array_no_zero(outputs_arena, f32, chunk_frames * AE_CHANNELS);
    oc->io_buf_bytes = chunk_bytes;

    oc->pa = pa_simple_new(NULL, "awaaz", PA_STREAM_PLAYBACK,
                           (char *)output_ids[i].str, "playback", &ss, NULL,
                           &attr, &err);

    if (!oc->pa) {
      trace_log("[warning] could not open output device: %s\n",
                pa_strerror(err));
      continue;
    }

    rb_init(outputs_arena, &oc->rb, ss.rate, ss.channels);
    g_output_count += 1;
  }

  if (g_output_count == 0) {
    trace_log("[error] no output devices could be opened\n");
    ae_stop();
    arena_pop_to(cap_arena, 0);
    arena_pop_to(outputs_arena, 0);
    return false;
  }

  atomic_store(&g_capture_ctx.stop_flag, false);
  if (thrd_create(&g_capture_ctx.thread, capture_thread_proc, NULL) !=
      thrd_success) {
    trace_log("[error] could not create capture thread\n");
    ae_stop();
    arena_pop_to(cap_arena, 0);
    arena_pop_to(outputs_arena, 0);
    return false;
  }
  g_capture_ctx.thread_valid = true;

  for (u32 i = 0; i < g_output_count; i += 1) {
    atomic_store(&g_outputs[i].stop_flag, false);
    if (thrd_create(&g_outputs[i].thread, output_thread_proc, &g_outputs[i]) !=
        thrd_success) {
      trace_log("[warning] could not create output thread %u\n", i);
    } else {
      g_outputs[i].thread_valid = true;
    }
  }

  atomic_store(&g_running, 1);
  return true;
}

f32 ae_peak_level(void) {
  return g_peak;
}
