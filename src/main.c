#include <stdio.h>
#include <stdlib.h>

#include <bace/bace.h>

#define trace_log(...) fprintf(stderr, __VA_ARGS__)

int main(int argc, char **argv) {
  bace_os_state_init();
  Arena *prog_arena = arena_alloc();
  Str8 base_path =
      str8_copy(prog_arena, str8_chop_last_slash(str8_cstring(argv[0])));
  trace_log("[info] argc = %d | argv[0] = %s\n", argc, argv[0]);
  trace_log("[info] base path = %s\n", base_path.str);

  return EXIT_SUCCESS;
}
