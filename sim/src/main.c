/* dzpanel_sim: interactive SDL window (if built with SDL2) or headless PNG export. */
#include "sim_core.h"
#include "screens.h"
#include "apps_demo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef RU_SIM_SDL
int sim_sdl_run(sim_t *s, const char *screen, int frames);
#endif

static void usage(void) {
  puts("usage: dzpanel_sim [--size 466|480] [--shape round|square] [--screen NAME] [--shot FILE.png] [--all DIR] [--apps DIR] [--app NAME] [--rotate 0|90|180|270] [--headless] [--frames N] [--list]\n"
       "  --rotate N    turn the picture as a device with rotation N does (window, mouse and PNGs); LVGL itself stays upright\n"
       "  --shape S     round (default, circle-masked PNGs) or square (full panel, files named NAME_480sq.png)\n"
       "  --apps DIR    render every Domoticz app screen (fixture data, no network) to DIR/app_NAME_SIZE[sq].png and exit\n"
       "  --app NAME --shot FILE   render one app screen (see --list-apps)\n"
       "  --shot FILE   render NAME headless to a circle-masked (square: unmasked) PNG and exit\n"
       "  --all DIR     render every screen to DIR/NAME_SIZE[sq].png and exit\n"
       "  (no --shot/--all) opens an SDL window: left/right = screen, p = save PNG, q/Esc = quit\n"
       "  SDL_VIDEODRIVER=dummy runs the window code without a display (use with --frames N)");
}

static int shot(sim_t *s, const char *name, const char *path) {
  lv_obj_t *scr = sim_screen_build(name, 500000);
  if (!scr) { fprintf(stderr, "unknown screen '%s'\n", name); return 2; }
  lv_screen_load(scr);
  sim_render();
  if (!sim_save_png(s, path, true)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
  printf("wrote %s\n", path);
  return 0;
}

int main(int argc, char **argv) {
  int size = 466, frames = 0;
  bool square = false;
  int rotate = 0;
  const char *screen = "home", *shot_path = NULL, *all_dir = NULL, *apps_dir = NULL, *app_name = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--size") && i + 1 < argc) size = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--shape") && i + 1 < argc) {
      const char *v = argv[++i];
      if (!strcmp(v, "square")) square = true;
      else if (!strcmp(v, "round")) square = false;
      else { fprintf(stderr, "--shape must be round or square\n"); return 2; }
    }
    else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) { rotate = atoi(argv[++i]); if (rotate % 90 || rotate < 0 || rotate > 270) { fprintf(stderr, "--rotate must be 0, 90, 180 or 270\n"); return 2; } }
    else if (!strcmp(argv[i], "--screen") && i + 1 < argc) screen = argv[++i];
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
    else if (!strcmp(argv[i], "--all") && i + 1 < argc) all_dir = argv[++i];
    else if (!strcmp(argv[i], "--apps") && i + 1 < argc) apps_dir = argv[++i];
    else if (!strcmp(argv[i], "--app") && i + 1 < argc) app_name = argv[++i];
    else if (!strcmp(argv[i], "--list-apps")) { for (int k = 0; k < apps_demo_count; k++) puts(apps_demo_names[k]); return 0; }
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--headless")) { if (!shot_path && !all_dir) all_dir = "."; }
    else if (!strcmp(argv[i], "--list")) { for (int k = 0; k < sim_screen_count; k++) puts(sim_screen_names[k]); return 0; }
    else { usage(); return strcmp(argv[i], "--help") ? 2 : 0; }
  }
  if (size != 466 && size != 480) { fprintf(stderr, "--size must be 466 or 480\n"); return 2; }
  sim_t s;
  if (!sim_init_ex(&s, size, square)) { fprintf(stderr, "init failed\n"); return 1; }
  sim_set_rotation(rotate);
  char rsuf[8] = ""; /* a turned run names its files _r90 ... */
  if (rotate) snprintf(rsuf, sizeof rsuf, "_r%d", rotate);
  int rc = 0;
  if (app_name && shot_path) rc = apps_demo_shot(&s, FIXTURE_DIR, app_name, shot_path);
  else if (apps_dir) {
    mkdir(apps_dir, 0755);
    for (int k = 0; k < apps_demo_count && !rc; k++) {
      char p[512];
      snprintf(p, sizeof p, "%s/app_%s_%d%s%s.png", apps_dir, apps_demo_names[k], size, square ? "sq" : "", rsuf);
      rc = apps_demo_shot(&s, FIXTURE_DIR, apps_demo_names[k], p);
    }
  }
  else if (shot_path) rc = shot(&s, screen, shot_path);
  else if (all_dir) {
    mkdir(all_dir, 0755);
    for (int k = 0; k < sim_screen_count && !rc; k++) {
      char p[512];
      snprintf(p, sizeof p, "%s/%s_%d%s%s.png", all_dir, sim_screen_names[k], size, square ? "sq" : "", rsuf);
      rc = shot(&s, sim_screen_names[k], p);
    }
  } else {
#ifdef RU_SIM_SDL
    rc = sim_sdl_run(&s, screen, frames);
#else
    (void)frames;
    fprintf(stderr, "built without SDL2: use --shot or --all\n");
    rc = 2;
#endif
  }
  sim_shutdown(&s);
  return rc;
}
