#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <functiondiscoverykeys.h>

#include <threads.h>

#include <bace/bace.h>

#include "audio_engine.h"

#define UNKNOWN_FRIENDLY_NAME L"(unknown device)"

// engine wide state
static IMMDeviceEnumerator *g_enumerator = NULL;

// device enumeration
bool ensure_enumerator(void) {
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
