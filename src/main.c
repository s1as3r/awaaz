#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_USE_SDL3
#include <cimgui.h>
#include <cimgui_impl.h>

#include <SDL3/SDL.h>

#include <bace/bace.h>

#include "audio_engine.h"
#include "audio_engine.c"
#include "ring_buffer.c"

bool ImGui_ImplSDLRenderer3_Init(SDL_Renderer *renderer);
void ImGui_ImplSDLRenderer3_NewFrame(void);
void ImGui_ImplSDLRenderer3_RenderDrawData(ImDrawData *draw_data,
                                           SDL_Renderer *renderer);
void ImGui_ImplSDLRenderer3_Shutdown(void);

int main(int argc, char **argv) {
  bace_os_state_init();
  Arena *prog_arena = arena_alloc();
  Str8 base_path =
      str8_copy(prog_arena, str8_chop_last_slash(str8_cstring(argv[0])));
  trace_log("[info] argc = %d | argv[0] = %s\n", argc, argv[0]);
  trace_log("[info] base path = %s\n", base_path.str);

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    trace_log("[error] failed to init sdl: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  f32 main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  SDL_WindowFlags window_flags =
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;

  i32 width = 800;
  i32 height = 600;
  if (!SDL_CreateWindowAndRenderer("awaaz", width, height, window_flags,
                                   &window, &renderer)) {
    trace_log("[error] failed to create window + renderer: %s\n",
              SDL_GetError());
    return EXIT_FAILURE;
  }

  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(window);

  igCreateContext(NULL);
  ImGuiIO *io = igGetIO_Nil();
  io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  igStyleColorsDark(NULL);

  ImGuiStyle *style = igGetStyle();
  ImGuiStyle_ScaleAllSizes(style, main_scale);
  style->FontScaleDpi = main_scale;
  io->ConfigDpiScaleFonts = true;
  io->ConfigDpiScaleViewports = true;

  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  AeDeviceList devices = ae_enumerate_render_devices(prog_arena);

  ImVec4 clear_color = {0.45f, 0.55f, 0.60f, 1.00f};

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      }
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
          event.window.windowID == SDL_GetWindowID(window)) {
        running = false;
      }
    }

    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(10);
      continue;
    }

    SDL_SetRenderDrawColorFloat(renderer, clear_color.x, clear_color.y,
                                clear_color.z, clear_color.w);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    igNewFrame();

    igBegin("devices", NULL, ImGuiWindowFlags_AlwaysAutoResize);
    for (u32 i = 0; i < devices.count; i += 1) {
      igBulletText("%s", devices.devices[i].name.str);
    }
    igEnd();

    igRender();
    ImDrawData *draw_data = igGetDrawData();
    ImGui_ImplSDLRenderer3_RenderDrawData(draw_data, renderer);
    SDL_RenderPresent(renderer);
  }

  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLRenderer3_Shutdown();
  igDestroyContext(NULL);

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  arena_release(prog_arena);
  return EXIT_SUCCESS;
}
