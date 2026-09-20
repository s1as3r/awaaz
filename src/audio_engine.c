#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <functiondiscoverykeys.h>

#include <threads.h>
#include <stdatomic.h>

#include <string.h>
#include <bace/bace.h>

#include "audio_engine.h"
#include "ring_buffer.h"

// per-output state
typedef struct {
  IMMDevice *device;
  IAudioClient *client;
  IAudioRenderClient *render;
  WAVEFORMATEX *format;

  thrd_t thread;
  bool thread_valid;
  HANDLE event; // WASAPI event for shared-mode render
  RingBuffer rb;
  f64 resample_pos;
  atomic_bool stop_flag;
} OutputCtx;

typedef struct {
  IMMDevice *device;
  IAudioClient *client;
  IAudioCaptureClient *capture;
  WAVEFORMATEX *format;

  thrd_t thread;
  bool thread_valid;
  HANDLE event;

  atomic_bool stop_flag;
} CaptureCtx;

// engine wide state
// TODO: refactor
global IMMDeviceEnumerator *g_enumerator = NULL;
global CaptureCtx g_capture_ctx = {0};

global OutputCtx *g_outputs;
global u32 g_output_count = 0;

global atomic_bool g_running = 0;
global volatile f32 g_peak = 0.0f;

internal bool is_float_format(const WAVEFORMATEX *wf) {
  if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return true;
  }
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wf;
    return IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  }
  return false;
}

// device enumeration
internal bool ensure_enumerator(void) {
  if (g_enumerator) {
    return true;
  }
  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (LPVOID *)&g_enumerator);
  return SUCCEEDED(hr);
}

AeDeviceList ae_enumerate_render_devices(Arena *arena) {
  AeDeviceList out = {0};
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  if (!ensure_enumerator()) {
    return out;
  }

  IMMDeviceCollection *collection = NULL;
  HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(
      g_enumerator, eRender, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr)) {
    return out;
  }

  UINT count = 0;
  IMMDeviceCollection_GetCount(collection, &count);
  out.devices = push_array_no_zero(arena, AeDeviceInfo, count);

  for (UINT i = 0; i < count; i += 1) {
    IMMDevice *dev = NULL;
    if (FAILED(IMMDeviceCollection_Item(collection, i, &dev))) {
      continue;
    }
    LPWSTR id = NULL;
    IMMDevice_GetId(dev, &id);

    IPropertyStore *props = NULL;
    Str8 friendly = s("(unknown device)");
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
      PROPVARIANT v;
      PropVariantInit(&v);

      if (SUCCEEDED(
              IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &v)) &&
          v.pwszVal) {
        friendly = str8_from_16(arena, str16_cstring(v.pwszVal));
      }
      PropVariantClear(&v);
      IPropertyStore_Release(props);
    }

    AeDeviceInfo *info = &out.devices[out.count];
    info->name = friendly;
    info->id = str8_from_16(arena, str16_cstring((u16 *)(id ? id : L"")));
    out.count += 1;

    if (id) {
      CoTaskMemFree(id);
    }
    IMMDevice_Release(dev);
  }
  IMMDeviceCollection_Release(collection);

  return out;
}

internal IMMDevice *open_device_by_id(Str16 id) {
  if (!ensure_enumerator()) {
    return NULL;
  }
  IMMDevice *dev = NULL;
  IMMDeviceEnumerator_GetDevice(g_enumerator, (LPCWSTR)id.str, &dev);
  return dev;
}

// output thread
// pulls frames from its ring buffer, linearly resamples + channel-maps to the
// devices's own mix format, and pushes them into WASAPI
internal void convert_channels(const f32 *in, u32 n_in_ch, f32 *out,
                               u32 n_out_ch) {
  if (n_in_ch == n_out_ch) {
    memcpy(out, in, sizeof(f32) * n_in_ch);
    return;
  }
  // mono -> n
  if (n_in_ch == 1) {
    for (u32 c = 0; c < n_out_ch; c += 1) {
      out[c] = in[0];
    }
    return;
  }
  // n -> mono
  if (n_out_ch == 1) {
    f32 sum = 0.0f;
    for (u32 c = 0; c < n_in_ch; c += 1) {
      sum += in[c];
    }
    out[0] = sum / (f32)n_in_ch;
    return;
  }

  u32 n = min(n_in_ch, n_out_ch);
  for (u32 c = 0; c < n; c += 1) {
    out[c] = in[c];
  }
  for (u32 c = n; c < n_out_ch; c += 1) {
    out[c] = 0.0f;
  }
}

internal int output_thread_proc(void *param) {
  OutputCtx *oc = (OutputCtx *)param;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  u32 buffer_frames = 0;
  IAudioClient_GetBufferSize(oc->client, &buffer_frames);

  f64 ratio = (f64)g_capture_ctx.format->nSamplesPerSec /
              (f64)oc->format->nSamplesPerSec;

  u32 n_in_ch = g_capture_ctx.format->nChannels;
  u32 n_out_ch = oc->format->nChannels;

  Temp scratch = scratch_begin(0, 0);
  const u32 n_scratch_frames = 4096;
  f32 *scratch_frames =
      push_array_no_zero(scratch.arena, f32, n_scratch_frames * n_in_ch);

  IAudioClient_Start(oc->client);

  while (!atomic_load(&oc->stop_flag)) {
    if (WaitForSingleObject(oc->event, 200) != WAIT_OBJECT_0) {
      continue;
    }

    u32 padding = 0;
    if (FAILED(IAudioClient_GetCurrentPadding(oc->client, &padding))) {
      continue;
    }

    u32 avail = buffer_frames - padding;
    if (avail == 0) {
      continue;
    }

    // number of input domain frames we roughly need to cover `avail` output
    // frames
    u32 needed_in = min((u32)(avail * ratio) + 4, n_scratch_frames);
    rb_read(&oc->rb, scratch_frames, needed_in);

    BYTE *data = NULL;
    if (FAILED(IAudioRenderClient_GetBuffer(oc->render, avail, &data))) {
      continue;
    }
    f32 *out = (f32 *)data;

    f64 pos = 0.0;
    for (u32 i = 0; i < avail; i += 1) {
      i32 idx = (i32)pos;
      if (idx >= (i32)needed_in - 1) {
        idx = (i32)needed_in - 2;
      }
      idx = max(0, idx);

      f64 frac = pos - idx;
      f32 mixed[8];
      for (u32 c = 0; c < n_in_ch && c < 8; c += 1) {
        f32 a = scratch_frames[(u32)idx * n_in_ch + c];
        f32 b = scratch_frames[((u32)idx + 1) * n_in_ch + c];
        mixed[c] = (f32)lerp_f64(a, b, frac);
      }
      convert_channels(mixed, n_in_ch, &out[i * n_out_ch], n_out_ch);
      pos += ratio;
    }
    IAudioRenderClient_ReleaseBuffer(oc->render, avail, 0);
  }

  IAudioClient_Stop(oc->client);
  scratch_end(scratch);
  CoUninitialize();
  return 0;
}

// caputure thread
// WASAPI capture on the input device, fans each block out to evey active
// output's ring buffer
internal int capture_thread_proc(void *param) {
  (void)param;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  IAudioClient_Start(g_capture_ctx.client);

  while (!atomic_load(&g_capture_ctx.stop_flag)) {
    if (WaitForSingleObject(g_capture_ctx.event, 200) != WAIT_OBJECT_0) {
      continue;
    }

    u32 packet_frames = 0;
    HRESULT hr = IAudioCaptureClient_GetNextPacketSize(g_capture_ctx.capture,
                                                       &packet_frames);
    while (SUCCEEDED(hr) && packet_frames > 0) {
      BYTE *data = NULL;
      DWORD flags = 0;
      u32 nframes = 0;
      hr = IAudioCaptureClient_GetBuffer(g_capture_ctx.capture, &data, &nframes,
                                         &flags, NULL, NULL);
      if (FAILED(hr)) {
        break;
      }

      u32 n_ch = g_capture_ctx.format->nChannels;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        // still feed silence, so outputs stay in sync
        local_persist f32 zero[4096 * 8] = {0.0f};
        u32 n = nframes;
        if (n * n_ch > array_count(zero)) {
          n = array_count(zero) / n_ch;
        }
        for (u32 o = 0; o < g_output_count; o += 1) {
          rb_write(&g_outputs[o].rb, zero, n);
        }
      } else {
        f32 *fdata = (f32 *)data;
        f32 peak = 0.0f;
        for (u32 i = 0; i < nframes * n_ch; i += 1) {
          f32 a = fabsf(fdata[i]);
          if (a > peak) {
            peak = a;
          }
        }
        g_peak = peak;

        for (u32 o = 0; o < g_output_count; o += 1) {
          rb_write(&g_outputs[o].rb, fdata, nframes);
        }
      }

      IAudioCaptureClient_ReleaseBuffer(g_capture_ctx.capture, nframes);
      hr = IAudioCaptureClient_GetNextPacketSize(g_capture_ctx.capture,
                                                 &packet_frames);
    }
  }

  IAudioClient_Stop(g_capture_ctx.client);
  CoUninitialize();
  return 0;
}

internal void free_output(OutputCtx *oc) {
  if (oc->render) {
    IAudioRenderClient_Release(oc->render);
  }
  if (oc->client) {
    IAudioClient_Release(oc->client);
  }
  if (oc->device) {
    IMMDevice_Release(oc->device);
  }
  if (oc->format) {
    CoTaskMemFree(oc->format);
  }
  if (oc->rb.buf) {
    rb_free(&oc->rb);
  }
  if (oc->event) {
    CloseHandle(oc->event);
  }
  memset(oc, 0, sizeof(*oc));
}

internal void free_capture(CaptureCtx *cc) {
  if (cc->capture) {
    IAudioCaptureClient_Release(cc->capture);
  }
  if (cc->client) {
    IAudioClient_Release(cc->client);
  }
  if (cc->device) {
    IMMDevice_Release(cc->device);
  }
  if (cc->format) {
    CoTaskMemFree(cc->format);
  }
  if (cc->event) {
    CloseHandle(cc->event);
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

  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (!ensure_enumerator()) {
    trace_log("[error] failed to create device enumerator\n");
    return false;
  }

  g_capture_ctx.device =
      open_device_by_id(str16_from_8(cap_arena, capture_device_id));
  if (!g_capture_ctx.device) {
    trace_log("[error] could not open capture device\n");
    return false;
  }

  HRESULT hr =
      IMMDevice_Activate(g_capture_ctx.device, &IID_IAudioClient, CLSCTX_ALL,
                         NULL, (void **)&g_capture_ctx.client);
  if (FAILED(hr)) {
    trace_log("[error] activate(capture) failed: 0x%08lx\n", hr);
    ae_stop();
    return false;
  }

  IAudioClient_GetMixFormat(g_capture_ctx.client, &g_capture_ctx.format);
  if (!is_float_format(g_capture_ctx.format)) {
    trace_log("[error] capture device mix format is not float32\n");
    ae_stop();
    return false;
  }

  hr = IAudioClient_Initialize(g_capture_ctx.client, AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK |
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               200 * 10000, // 200 ms buffer, in 100ns units
                               0, g_capture_ctx.format, NULL);
  if (FAILED(hr)) {
    trace_log("[error] initialize(capture) failed: 0x%07lx", hr);
    ae_stop();
    return false;
  }

  g_capture_ctx.event = CreateEventW(NULL, FALSE, FALSE, NULL);
  IAudioClient_SetEventHandle(g_capture_ctx.client, g_capture_ctx.event);
  IAudioClient_GetService(g_capture_ctx.client, &IID_IAudioCaptureClient,
                          (void **)&g_capture_ctx.capture);

  g_outputs = push_array_no_zero(outputs_arena, OutputCtx, output_count);
  g_output_count = 0;
  for (u32 i = 0; i < output_count; i += 1) {
    OutputCtx *oc = &g_outputs[g_output_count];
    memset(oc, 0, sizeof(*oc));

    oc->device = open_device_by_id(str16_from_8(outputs_arena, output_ids[i]));
    if (!oc->device) {
      continue;
    }

    hr = IMMDevice_Activate(oc->device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&oc->client);
    if (FAILED(hr)) {
      free_output(oc);
      continue;
    }

    IAudioClient_GetMixFormat(oc->client, &oc->format);

    hr = IAudioClient_Initialize(oc->client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200 * 10000,
                                 0, oc->format, NULL);
    if (FAILED(hr)) {
      free_output(oc);
      continue;
    }

    oc->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(oc->client, oc->event);
    IAudioClient_GetService(oc->client, &IID_IAudioRenderClient,
                            (void **)&oc->render);

    rb_init(outputs_arena, &oc->rb, g_capture_ctx.format->nSamplesPerSec,
            g_capture_ctx.format->nChannels);
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
    if (thread_launch_with_ctx(&g_outputs[i].thread, output_thread_proc,
                               &g_outputs[i]) != thrd_success) {
      trace_log("[warning] could not create output thread %u\n", i);
    } else {
      g_outputs[i].thread_valid = true;
    }
  }
  atomic_store(&g_running, 1);
  return true;
}

f32 ae_peak_level(void) {
  f32 p = g_peak;
  g_peak *= 0.90f;
  return p;
}
