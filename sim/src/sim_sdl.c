/* Interactive window. Round: the window shows the circle only (outside is black). Square: the full panel with an outline. */
#include "ru_geom.h"
#include "sim_core.h"
#include "screens.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t sim_sdl_ticks(void) { return SDL_GetTicks(); }

static int find(const char *n) {
  for (int i = 0; i < sim_screen_count; i++) if (!strcmp(sim_screen_names[i], n)) return i;
  return 0;
}

static void load(int idx) {
  lv_obj_t *old = lv_screen_active();
  lv_obj_t *scr = sim_screen_build(sim_screen_names[idx], sim_now_ms() + 500000);
  lv_screen_load(scr);
  if (old) lv_obj_delete(old);
}

int sim_sdl_run(sim_t *s, const char *screen, int frames) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  SDL_Window *win = SDL_CreateWindow("dzPanel simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, s->size, s->size, 0);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
  SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, s->size, s->size) : NULL;
  uint32_t *masked = malloc((size_t)s->size * (size_t)s->size * 4);
  if (!tex || !masked) { fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError()); return 1; }
  sim_use_real_clock();
  int idx = find(screen);
  lv_obj_t *first = sim_screen_build(sim_screen_names[idx], 500000);
  lv_screen_load(first);
  bool run = true, down = false;
  int n = 0, mx = 0, my = 0;
  while (run && (frames == 0 || n < frames)) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) run = false;
      else if (e.type == SDL_MOUSEMOTION) { mx = e.motion.x; my = e.motion.y; }
      else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { down = true; mx = e.button.x; my = e.button.y; }
      else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) { down = false; mx = e.button.x; my = e.button.y; }
      else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_ESCAPE || k == SDLK_q) run = false;
        else if (k == SDLK_RIGHT) { idx = (idx + 1) % sim_screen_count; load(idx); }
        else if (k == SDLK_LEFT) { idx = (idx + sim_screen_count - 1) % sim_screen_count; load(idx); }
        else if (k == SDLK_p) {
          char p[128];
          snprintf(p, sizeof p, "%s_%d%s.png", sim_screen_names[idx], s->size, s->square ? "sq" : "");
          if (sim_save_png(s, p, true)) printf("wrote %s\n", p);
        }
      }
    }
    sim_pointer_panel(mx, my, down); /* the window shows the panel: a turned screen turns the mouse back */
    lv_timer_handler();
    sim_masked_copy(s, masked);
    SDL_UpdateTexture(tex, NULL, masked, s->size * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    if (s->square) {
      SDL_Rect o = { 0, 0, s->size, s->size };
      SDL_SetRenderDrawColor(ren, 90, 90, 90, 255);
      SDL_RenderDrawRect(ren, &o);
    }
    SDL_RenderPresent(ren);
    SDL_Delay(16);
    n++;
  }
  free(masked);
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
