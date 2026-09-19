#ifndef _H_AWAAZ_AUDIO_ENGINE
#define _H_AWAAZ_AUDIO_ENGINE

#include <bace/bace.h> /// IWYU pragma: export

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

#endif // ! _H_AWAAZ_AUDIO_ENGINE
