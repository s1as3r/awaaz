#ifndef _H_AWAAZ_AUDIO_ENGINE
#define _H_AWAAZ_AUDIO_ENGINE

#include <bace/bace.h>
#include <stdatomic.h>
#include <stdio.h> // IWYU pragma: export

#define trace_log(...) fprintf(stderr, __VA_ARGS__)

//- tuning

// device period (windows only)
#define AE_BUFFER_TIME_MS 10

// capture thread sleep amount before re-checking its stop flag
#define AE_WAIT_TIMEOUT_MS (AE_BUFFER_TIME_MS * 3)

#define AE_JITTER_MS 2

// silence prepended whenever an output's queue is found empty
#define AE_CUSHION_MS 2

//- types

typedef struct {
  Str8 id;   // endpoint id string used to open the device.
  Str8 name; // human friendly name shown in the ui.
} AeDeviceInfo;

typedef struct {
  AeDeviceInfo *devices;
  u32 count;
} AeDeviceList;

//- error reporting

// `ae_start()` returns `false` and leaves a human-readable reason here so the
// ui can show it
#define AE_ERR_SZ 256
global char g_ae_error[AE_ERR_SZ];

#define ae_set_error(...)                                                      \
  (snprintf(g_ae_error, AE_ERR_SZ, __VA_ARGS__),                               \
   trace_log("[error] %s\n", g_ae_error))

static inline const char *ae_last_error(void) {
  return g_ae_error;
}

//- api

// lists every active render endpoint
AeDeviceList ae_enumerate_render_devices(Arena *arena);

// opens loopback capture on `capture_device_id` and a render stream on every
// id in `output_ids[0..output_count)`.
// on failure returns false with the reason in `ae_last_error()`
bool ae_start(Arena *cap_arena, Arena *outputs_arena, Str8 capture_device_id,
              const Str8 *output_ids, u32 output_count);

// stops streaming and releases all audio resources
void ae_stop(void);

bool ae_is_running(void);

#endif // ! _H_AWAAZ_AUDIO_ENGINE
