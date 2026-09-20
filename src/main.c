#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_USE_SDL3
#include <cimgui.h>
#include <cimgui_impl.h>

#include <SDL3/SDL.h>

#include <bace/bace.h>

#include "audio_engine.h"
#include "audio_engine.c"
#include "ring_buffer.c"

#define AE_MAX_ERR_SZ 256

bool ImGui_ImplSDLRenderer3_Init(SDL_Renderer *renderer);
void ImGui_ImplSDLRenderer3_NewFrame(void);
void ImGui_ImplSDLRenderer3_RenderDrawData(ImDrawData *draw_data,
                                           SDL_Renderer *renderer);
void ImGui_ImplSDLRenderer3_Shutdown(void);

global const ImVec2 g_zero_vec2 = {0.0f, 0.0f};

typedef struct {
  Arena *capture_arena;
  Arena *outputs_arena;
  Arena *ui_arena;

  AeDeviceList devices;
  i32 capture_idx;
  bool *selected_outputs;

  char err[AE_MAX_ERR_SZ];
} AppState;

void refresh_devices(AppState *state) {
  arena_pop_to(state->ui_arena, 0);
  state->devices = ae_enumerate_render_devices(state->ui_arena);
  if (state->capture_idx >= (i32)state->devices.count) {
    state->capture_idx = -1;
  }

  state->selected_outputs =
      push_array_no_zero(state->ui_arena, bool, state->devices.count);
  for (u32 i = 0; i < state->devices.count; i += 1) {
    state->selected_outputs[i] = false;
  }
}

void draw_ui(AppState *state) {
  const ImGuiViewport *viewport = igGetMainViewport();
  igSetNextWindowPos(viewport->WorkPos, ImGuiCond_None, g_zero_vec2);
  igSetNextWindowSize(viewport->WorkSize, ImGuiCond_None);

  ImGuiWindowFlags window_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

  igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
  igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);

  igBegin("awaaz", NULL, window_flags);
  igPopStyleVar(2);
  igText("audio output to multiple devices");
  igSeparator();

  if (igButton("Refresh Devices", g_zero_vec2)) {
    refresh_devices(state);
  }
  igSpacing();

  igText("Capture Source:");
  const char *preview =
      (state->capture_idx >= 0)
          ? (char *)state->devices.devices[state->capture_idx].name.str
          : "(choose one)";

  if (igBeginCombo("##capture", preview, ImGuiComboFlags_None)) {
    for (i32 i = 0; i < (i32)state->devices.count; i += 1) {
      bool selected = (i == state->capture_idx);
      if (igSelectable_Bool((char *)state->devices.devices[i].name.str,
                            selected, 0, g_zero_vec2)) {
        state->capture_idx = i;
      }
      if (selected) {
        igSetItemDefaultFocus();
      }
    }
    igEndCombo();
  }
  igSpacing();
  igSeparator();

  igText("Outputs: ");
  igBeginChild_Str("outputs", (ImVec2){0.0f, 300.0f},
                   ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysAutoResize |
                       ImGuiChildFlags_AutoResizeX |
                       ImGuiChildFlags_AutoResizeY,
                   ImGuiWindowFlags_None);
  {
    for (i32 i = 0; i < (i32)state->devices.count; i += 1) {
      if (i == state->capture_idx) {
        continue;
      }
      igCheckbox((char *)state->devices.devices[i].name.str,
                 &state->selected_outputs[i]);
    }
  }
  igEndChild();

  igSpacing();
  igSeparator();

  bool running = ae_is_running();
  if (!running) {
    if (igButton("Start", g_zero_vec2)) {
      if (state->capture_idx < 0) {
        sprintf_s(state->err, AE_MAX_ERR_SZ, "pick a capture device first");
      } else {
        Str8 *out_ids = push_array_no_zero(state->outputs_arena, Str8,
                                           state->devices.count);
        u32 n = 0;
        for (u32 i = 0; i < state->devices.count; i += 1) {
          if (state->selected_outputs[i] && !((i32)i == state->capture_idx)) {
            out_ids[n++] = state->devices.devices[i].id;
          }
        }
        if (n == 0) {
          sprintf_s(state->err, AE_MAX_ERR_SZ,
                    "select at least one output device");
        } else {
          state->err[0] = 0;
          ae_start(state->capture_arena, state->outputs_arena,
                   state->devices.devices[state->capture_idx].id, out_ids, n);
        }
      }
    }
  } else {
    if (igButton("Stop", g_zero_vec2)) {
      ae_stop();
      arena_pop_to(state->capture_arena, 0);
      arena_pop_to(state->outputs_arena, 0);
    }
  }

  if (running) {
    igSpacing();
    igText("Level: ");
    igSameLine(0, 0);
    igProgressBar(ae_peak_level(), (ImVec2){-1.0f, 0.0f}, NULL);
  }

  if (state->err[0]) {
    igSpacing();
    igTextColored((ImVec4){1.0f, 0.4f, 0.4f, 1.0f}, "%s", state->err);
  }

  if (0) {
    igSpacing();
    igSeparator();
    igBeginChild_Str("stats", (ImVec2){0, 180},
                     ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX |
                         ImGuiChildFlags_AutoResizeY,
                     0);
    {
      igText("outputs arena: pos = %u | cmt = %u | res = %u",
             state->outputs_arena->pos, state->outputs_arena->cmt,
             state->outputs_arena->res);
      igText("capture arena: pos = %u | cmt = %u | res = %u",
             state->capture_arena->pos, state->capture_arena->cmt,
             state->capture_arena->res);
      igText("ui arena: pos = %u | cmt = %u | res = %u", state->ui_arena->pos,
             state->ui_arena->cmt, state->ui_arena->res);
    }
    igEndChild();
  }

  igEnd();
}

int main(void) {
  bace_os_state_init();
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

  ImVec4 clear_color = {0.45f, 0.55f, 0.60f, 1.00f};

  AppState app_state = {
      .capture_arena = arena_alloc(),
      .outputs_arena = arena_alloc(),
      .ui_arena = arena_alloc(),
      .capture_idx = -1,
  };
  refresh_devices(&app_state);
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
    draw_ui(&app_state);

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

  arena_release(app_state.capture_arena);
  arena_release(app_state.outputs_arena);
  arena_release(app_state.ui_arena);
  return EXIT_SUCCESS;
}
