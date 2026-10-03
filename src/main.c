// Emulateur Game Boy multi-jeux pour NumWorks (EADK + Peanut-GB)
// Les jeux sont regroupes dans UN fichier de donnees (voir tools/pack.py).
#include "eadk.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define ENABLE_LCD 1
#define ENABLE_SOUND 0
#include "peanut_gb.h"

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "GameBoy";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

#define MAX_GAMES 32
#define ENTRY_SIZE 32
#define LCD_X ((320 - LCD_WIDTH) / 2)
#define LCD_Y ((240 - LCD_HEIGHT) / 2)

typedef struct { char name[25]; uint32_t offset, size; } game_t;

static game_t games[MAX_GAMES];
static int game_count = 0;
static const uint8_t *rom_base;
static uint8_t cart_ram[32768];
static struct gb_s gb;
static eadk_color_t line_buf[LCD_WIDTH];
static bool render_frame = true;
// 4 nuances (vert Game Boy), du plus clair au plus fonce
static const eadk_color_t shades[4] = {0xCF33, 0x9E6B, 0x4C86, 0x1884};

static uint8_t gb_rom_read(struct gb_s *g, const uint_fast32_t a) { (void)g; return rom_base[a]; }
static uint8_t gb_cart_ram_read(struct gb_s *g, const uint_fast32_t a) { (void)g; return cart_ram[a]; }
static void gb_cart_ram_write(struct gb_s *g, const uint_fast32_t a, const uint8_t v) { (void)g; cart_ram[a] = v; }
static void gb_error(struct gb_s *g, const enum gb_error_e e, const uint16_t v) { (void)g; (void)e; (void)v; }

static void lcd_draw_line(struct gb_s *g, const uint8_t pixels[160], const uint_fast8_t line) {
  (void)g;
  if (!render_frame) return;
  for (int x = 0; x < LCD_WIDTH; x++) line_buf[x] = shades[pixels[x] & 3];
  eadk_rect_t r = {LCD_X, LCD_Y + line, LCD_WIDTH, 1};
  eadk_display_push_rect(r, line_buf);
}

static void text(const char *s, int x, int y, bool sel) {
  eadk_color_t fg = sel ? eadk_color_white : eadk_color_black;
  eadk_color_t bg = sel ? 0x1884 : eadk_color_white;
  eadk_display_draw_string(s, (eadk_point_t){x, y}, false, fg, bg);
}

static void load_games(void) {
  const uint8_t *d = (const uint8_t *)eadk_external_data;
  game_count = 0;
  if (eadk_external_data_size < 8 || memcmp(d, "GBPK", 4) != 0) return;
  uint32_t n; memcpy(&n, d + 4, 4);
  if (n > MAX_GAMES) n = MAX_GAMES;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *e = d + 8 + i * ENTRY_SIZE;
    memcpy(games[i].name, e, 24); games[i].name[24] = 0;
    memcpy(&games[i].offset, e + 24, 4);
    memcpy(&games[i].size, e + 28, 4);
  }
  game_count = (int)n;
}

static void wait_release(void) {
  while (eadk_keyboard_scan() != 0) eadk_timing_msleep(20);
}

static int menu(void) {
  int sel = 0, top = 0;
  const int visible = 8;
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_white);
  text("== Game Boy ==", 100, 8, false);
  if (game_count == 0) {
    text("Aucun jeu trouve.", 20, 60, false);
    text("Envoyez le fichier .bin", 20, 80, false);
    text("(pack.py) avec l'app.", 20, 100, false);
    while (true) eadk_timing_msleep(100);
  }
  bool redraw = true;
  while (true) {
    if (redraw) {
      for (int i = 0; i < visible; i++) {
        int idx = top + i;
        char buf[40];
        if (idx < game_count) snprintf(buf, sizeof buf, " %-28s", games[idx].name);
        else snprintf(buf, sizeof buf, "%34s", "");
        text(buf, 10, 36 + i * 22, idx == sel);
      }
      text("OK: jouer   Var: retour menu en jeu", 10, 220, false);
      redraw = false;
    }
    eadk_keyboard_state_t k = eadk_keyboard_scan();
    if (eadk_keyboard_key_down(k, eadk_key_down) && sel < game_count - 1) { sel++; redraw = true; }
    else if (eadk_keyboard_key_down(k, eadk_key_up) && sel > 0) { sel--; redraw = true; }
    else if (eadk_keyboard_key_down(k, eadk_key_ok) || eadk_keyboard_key_down(k, eadk_key_exe)) {
      wait_release(); return sel;
    }
    if (sel < top) top = sel;
    if (sel >= top + visible) top = sel - visible + 1;
    if (k) { eadk_timing_msleep(120); }
    else eadk_timing_msleep(20);
  }
}

static void message(const char *s) {
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_white);
  text(s, 20, 100, false);
  eadk_timing_msleep(2000);
}

// Retourne quand l'utilisateur appuie sur Var
static void play(int idx) {
  rom_base = (const uint8_t *)eadk_external_data + games[idx].offset;
  memset(cart_ram, 0, sizeof cart_ram);
  if (gb_init(&gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write, &gb_error, NULL) != GB_INIT_NO_ERROR) {
    message("ROM invalide / non supportee"); return;
  }
  gb_init_lcd(&gb, &lcd_draw_line);
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
  uint32_t frame = 0;
  while (true) {
    uint64_t t0 = eadk_timing_millis();
    eadk_keyboard_state_t k = eadk_keyboard_scan();
    if (eadk_keyboard_key_down(k, eadk_key_var)) { wait_release(); return; }
    uint8_t j = 0xFF;
    if (eadk_keyboard_key_down(k, eadk_key_ok))        j &= ~JOYPAD_A;
    if (eadk_keyboard_key_down(k, eadk_key_back))      j &= ~JOYPAD_B;
    if (eadk_keyboard_key_down(k, eadk_key_exe))       j &= ~JOYPAD_START;
    if (eadk_keyboard_key_down(k, eadk_key_backspace)) j &= ~JOYPAD_SELECT;
    if (eadk_keyboard_key_down(k, eadk_key_up))        j &= ~JOYPAD_UP;
    if (eadk_keyboard_key_down(k, eadk_key_down))      j &= ~JOYPAD_DOWN;
    if (eadk_keyboard_key_down(k, eadk_key_left))      j &= ~JOYPAD_LEFT;
    if (eadk_keyboard_key_down(k, eadk_key_right))     j &= ~JOYPAD_RIGHT;
    gb.direct.joypad = j;
    render_frame = (frame++ & 1) == 0;   // 1 image sur 2 affichee (plus rapide)
    gb_run_frame(&gb);
    uint64_t dt = eadk_timing_millis() - t0;
    if (dt < 16) eadk_timing_msleep(16 - (uint32_t)dt);
  }
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  load_games();
  while (true) {
    int g = menu();
    play(g);
  }
  return 0;
}
