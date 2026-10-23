//- windows impl

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <functiondiscoverykeys.h>
#include <avrt.h>

#include <math.h>
#include <threads.h>
#include <stdatomic.h>
#include <string.h>

#include <bace/bace.h>

#include "../audio_engine.h"

//- state

// one render endpoint
typedef struct {
  IMMDevice *device;
  IAudioClient *client;
  IAudioRenderClient *render;

  u32 buffer_frames;    // total size of the device-side buffer
  u32 cushion_frames;   // silence prepended when the queue is empty
  u32 max_queue_frames; // if queued + new audio would exceed this, drop it
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

global IMMDeviceEnumerator *g_enumerator = NULL;
global CaptureCtx g_capture_ctx = {0};
global OutputCtx *g_outputs = NULL;
global u32 g_output_count = 0;
global atomic_bool g_running = 0;

//- helpers

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

internal HANDLE increase_audio_thread_priority(void) {
  DWORD task = 0;
  HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  if (!h) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  }
  return h;
}

//- output push

// hands one capture packet to one output device.
// called from the capture thread, once per output per packet.
// writes into the endpoint's buffer.
internal void output_push(OutputCtx *oc, const BYTE *data, u32 nframes,
                          bool silent) {
  const u32 block = g_capture_ctx.format->nBlockAlign;

  // frames we already wrote that the device hasn't played yet
  u32 padding = 0;
  if (FAILED(IAudioClient_GetCurrentPadding(oc->client, &padding))) {
    return;
  }

  u32 cushion = (padding == 0) ? oc->cushion_frames : 0;

  // if the device already holds too much, drop this packet for this output
  // instead of letting the queue keep growing.
  u32 total = cushion + nframes;
  u32 queued_after = padding + total;
  if (queued_after > oc->max_queue_frames || queued_after > oc->buffer_frames) {
    return;
  }

  BYTE *dst = NULL;
  if (FAILED(IAudioRenderClient_GetBuffer(oc->render, total, &dst))) {
    return;
  }
  memset(dst, 0, (usize)cushion * block);
  if (silent) {
    memset(dst + (usize)cushion * block, 0, (usize)nframes * block);
  } else {
    memcpy(dst + (usize)cushion * block, data, (usize)nframes * block);
  }
  IAudioRenderClient_ReleaseBuffer(oc->render, total, 0);
}

//- capture thread

internal int capture_thread_proc(void *param) {
  (void)param;
  HANDLE mmcss = increase_audio_thread_priority();
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  IAudioClient_Start(g_capture_ctx.client);

  while (!atomic_load(&g_capture_ctx.stop_flag)) {
    if (WaitForSingleObject(g_capture_ctx.event, AE_WAIT_TIMEOUT_MS) !=
        WAIT_OBJECT_0) {
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
      bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

      for (u32 o = 0; o < g_output_count; o += 1) {
        output_push(&g_outputs[o], data, nframes, silent);
      }

      IAudioCaptureClient_ReleaseBuffer(g_capture_ctx.capture, nframes);
      hr = IAudioCaptureClient_GetNextPacketSize(g_capture_ctx.capture,
                                                 &packet_frames);
    }
  }

  IAudioClient_Stop(g_capture_ctx.client);
  CoUninitialize();
  if (mmcss) {
    AvRevertMmThreadCharacteristics(mmcss);
  }
  return 0;
}

//- teardown

internal void free_output(OutputCtx *oc) {
  if (oc->client) {
    IAudioClient_Stop(oc->client);
  }
  if (oc->render) {
    IAudioRenderClient_Release(oc->render);
  }
  if (oc->client) {
    IAudioClient_Release(oc->client);
  }
  if (oc->device) {
    IMMDevice_Release(oc->device);
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

internal void ae_teardown(void) {
  atomic_store(&g_capture_ctx.stop_flag, true);

  if (g_capture_ctx.thread_valid) {
    thrd_join(g_capture_ctx.thread, NULL);
    g_capture_ctx.thread_valid = false;
  }

  for (u32 i = 0; i < g_output_count; i += 1) {
    free_output(&g_outputs[i]);
  }

  g_output_count = 0;
  g_outputs = NULL;

  free_capture(&g_capture_ctx);
  atomic_store(&g_running, false);
}

void ae_stop(void) {
  if (atomic_load(&g_running)) {
    ae_teardown();
  }
}

bool ae_is_running(void) {
  return atomic_load(&g_running);
}

//- start

// records the error, tears everything down, resets the arenas, returns false.
// uses `cap_arena` / `outputs_arena` from the enclosing function.
// TODO: someting better ig
#define AE_FAIL(...)                                                           \
  (ae_set_error(__VA_ARGS__), ae_teardown(), arena_pop_to(cap_arena, 0),       \
   arena_pop_to(outputs_arena, 0), false)

bool ae_start(Arena *cap_arena, Arena *outputs_arena, Str8 capture_device_id,
              const Str8 *output_ids, u32 output_count) {
  g_ae_error[0] = 0;
  if (ae_is_running()) {
    ae_set_error("already running");
    return false;
  }

  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (!ensure_enumerator()) {
    return AE_FAIL("failed to create device enumerator");
  }

  g_capture_ctx.device =
      open_device_by_id(str16_from_8(cap_arena, capture_device_id));
  if (!g_capture_ctx.device) {
    return AE_FAIL("could not open capture device");
  }

  HRESULT hr =
      IMMDevice_Activate(g_capture_ctx.device, &IID_IAudioClient, CLSCTX_ALL,
                         NULL, (void **)&g_capture_ctx.client);
  if (FAILED(hr)) {
    return AE_FAIL("activate(capture) failed: 0x%08lx", hr);
  }

  IAudioClient_GetMixFormat(g_capture_ctx.client, &g_capture_ctx.format);
  if (!is_float_format(g_capture_ctx.format)) {
    return AE_FAIL("capture device mix format is not float32");
  }

  hr = IAudioClient_Initialize(g_capture_ctx.client, AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK |
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               AE_BUFFER_TIME_MS * 10000, // in 100ns units
                               0, g_capture_ctx.format, NULL);
  if (FAILED(hr)) {
    return AE_FAIL("initialize(capture) failed: 0x%08lx", hr);
  }

  g_capture_ctx.event = CreateEventW(NULL, FALSE, FALSE, NULL);
  IAudioClient_SetEventHandle(g_capture_ctx.client, g_capture_ctx.event);
  IAudioClient_GetService(g_capture_ctx.client, &IID_IAudioCaptureClient,
                          (void **)&g_capture_ctx.capture);

  const u32 rate = g_capture_ctx.format->nSamplesPerSec;
  const u32 period_frames = rate * AE_BUFFER_TIME_MS / 1000;

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

    hr = IAudioClient_Initialize(oc->client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 AE_BUFFER_TIME_MS * 10000, 0,
                                 g_capture_ctx.format, NULL);
    if (FAILED(hr)) {
      trace_log("[warning] output %u init failed: 0x%08lx\n", i, hr);
      free_output(oc);
      continue;
    }

    IAudioClient_GetService(oc->client, &IID_IAudioRenderClient,
                            (void **)&oc->render);
    IAudioClient_GetBufferSize(oc->client, &oc->buffer_frames);

    u32 jitter = rate * AE_JITTER_MS / 1000;
    oc->cushion_frames = rate * AE_CUSHION_MS / 1000;
    oc->max_queue_frames =
        oc->cushion_frames + period_frames + period_frames + jitter;

    IAudioClient_Start(oc->client);
    g_output_count += 1;
  }

  if (g_output_count == 0) {
    return AE_FAIL("no output devices could be opened");
  }

  atomic_store(&g_capture_ctx.stop_flag, false);
  if (thrd_create(&g_capture_ctx.thread, capture_thread_proc, NULL) !=
      thrd_success) {
    return AE_FAIL("could not create capture thread");
  }
  g_capture_ctx.thread_valid = true;
  atomic_store(&g_running, true);
  return true;
}
