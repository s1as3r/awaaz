#ifndef _H_AWAAZ_AUDIO_ENGINE
#define _H_AWAAZ_AUDIO_ENGINE

#include <bace/bace.h>
#include <stdio.h>

#define trace_log(...) fprintf(stderr, __VA_ARGS__)

typedef struct {
  Str8 id;   // endpoint id string used to open the device.
  Str8 name; // human friendly name shown in the ui.
} AeDeviceInfo;

typedef struct {
  AeDeviceInfo *devices;
  u32 count;
} AeDeviceList;

// enumerates every active render output.
// these can be used as caputure devices or as outputs
AeDeviceList ae_enumerate_render_devices(Arena *arena);

// opens WASAPI loopback capture on `capture_device_id`, opens a WASAPI render
// client on every id in `output_ids[0..output_count)` and spins up one capture
// thread + one thread per output
bool ae_start(Arena *cap_arena, Arena *outputs_arena, Str8 capture_device_id,
              const Str8 *output_ids, u32 output_count);

// stops all the threads and releases all WASAPI resources.
void ae_stop(void);

bool ae_is_running(void);

// 0..1 peak of the most recently captured output block for a simple level
// meter.
float ae_peak_level(void);

#endif // ! _H_AWAAZ_AUDIO_ENGINE
