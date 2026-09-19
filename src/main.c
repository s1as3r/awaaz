#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>

#include <bace/bace.h>

#include "audio_engine.h"
#include "audio_engine.c"
#include "ring_buffer.c"

int main(int argc, char **argv) {
  bace_os_state_init();
  Arena *prog_arena = arena_alloc();
  Str8 base_path =
      str8_copy(prog_arena, str8_chop_last_slash(str8_cstring(argv[0])));
  trace_log("[info] argc = %d | argv[0] = %s\n", argc, argv[0]);
  trace_log("[info] base path = %s\n", base_path.str);

  AeDeviceList devices = ae_enumerate_render_devices(prog_arena);

  for (u32 i = 0; i < devices.count; i += 1) {
    printf("[%u] %s: %s\n", i, devices.devices[i].id.str,
           devices.devices[i].name.str);
  }

  // test code
  // TODO: imgui ftw
  printf("\ninput: ");
  u32 input_idx;
  scanf("%u", &input_idx);

  printf("n outputs: ");
  u32 n_out;
  scanf("%u", &n_out);

  u32 *outputs = push_array_no_zero(prog_arena, u32, n_out);
  Str8 *output_ids = push_array_no_zero(prog_arena, Str8, n_out);
  for (u32 i = 0; i < n_out; i += 1) {
    printf("%u: ", i);
    scanf("%u", &outputs[i]);
    output_ids[i] = devices.devices[outputs[i]].id;
  }

  printf("input = %s\n", devices.devices[input_idx].name.str);
  printf("outputs = ");
  for (u32 i = 0; i < n_out; i += 1) {
    printf("%s", devices.devices[outputs[i]].name.str);
    if (i != n_out - 1) {
      printf(", ");
    } else {
      printf("\n");
    }
  }

  Arena *a1 = arena_alloc();
  Arena *a2 = arena_alloc();
  ae_start(a1, a2, devices.devices[input_idx].id, output_ids, n_out);

  printf("ae started\n");
  i32 stop;
  scanf("%d", &stop);

  ae_stop();
  arena_release(a1);
  arena_release(a2);
  arena_release(prog_arena);

  return EXIT_SUCCESS;
}
