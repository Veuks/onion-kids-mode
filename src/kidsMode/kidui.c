// kidui - Kid Mode fullscreen favorites carousel for Onion OS
//
// Shows the device's favorites (/mnt/SDCARD/Roms/favourite.json) one game
// at a time: big box art, big label, left/right to browse, A to play.
// Holding SELECT+START for 3 seconds opens a 4-digit PIN entry.
//
// Output protocol (written to /tmp/kidmode_ui_result, consumed by
// kid_mode_loop.sh; stdout is NOT used for results because the device's
// SDL/driver stack prints noise there):
//   exit 0:  "LAUNCH" \n <launch path> \n <rom path>        (resume)
//            "LAUNCH_FRESH" \n <launch path> \n <rom path>  (start over)
//   exit 3:  "PIN" \n <4 digits>
//   exit 5:  "MENU" \n <UNLOCK|ADDTIME|TIMER> [\n <minutes>]
//   exit 1:  canceled / error / nothing selected (result file removed)
//
// Modes:
//   kidui                          carousel (default)
//   kidui --set-pin -t "..."       PIN entry only (for initial PIN setup)
//   kidui --parent-menu --timer N --remaining S
//                                  post-PIN parent menu (N = configured
//                                  minutes/day, S = seconds left, -1 = off)
//   kidui --pick-timer --no-off -t "Add play time"
//                                  minutes picker without an OFF option;
//                                  B cancels (exit 1) instead of choosing 0
//
// Play timer: kid_mode_loop.sh's ticker writes the remaining seconds to
// /tmp/kidmode_remaining. The carousel shows it as a small chip and flips
// to a friendly "Time's up!" screen at zero (SELECT+START still works).

#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "components/JsonGameEntry.h"
#include "system/keymap_sw.h"
#include "utils/keystate.h"
#include "utils/log.h"
#include "utils/msleep.h"
#include "utils/sdl_init.h"
#include "utils/str.h"

#define MAX_GAMES 100
#define PIN_LEN 4
#define UNLOCK_HOLD_MS 3000
#define UNLOCK_BAR_SHOW_MS 800
#define PIN_IDLE_TIMEOUT_MS 30000
#define REMAINING_POLL_MS 2000
#define REMAINING_FILE "/tmp/kidmode_remaining"
#define RESULT_FILE "/tmp/kidmode_ui_result"
#define FONT_MAIN "/customer/app/Exo-2-Bold-Italic.ttf"
#define FONT_FALLBACK "/mnt/SDCARD/miyoo/app/Exo-2-Bold-Italic.ttf"

typedef enum { SCREEN_CAROUSEL,
               SCREEN_PIN,
               SCREEN_EMPTY,
               SCREEN_TIMESUP,
               SCREEN_MENU,
               SCREEN_PICKTIMER,
               SCREEN_CONFIRM_RESTART } Screen;

#define MENU_UNLOCK 0
#define MENU_ADDTIME 1
#define MENU_BACK 2
#define MENU_COUNT 3
#define TIMER_STEP 5
#define TIMER_MAX 50
#define SYSTEM_JSON "/mnt/SDCARD/system.json"

static bool quit = false;

static JsonGameEntry games[MAX_GAMES];
static int games_count = 0;
static int current = 0;

static SDL_Surface *artwork = NULL;
static int artwork_index = -1;

static int pin_digits[PIN_LEN] = {0, 0, 0, 0};
static int pin_cursor = 0;

static const SDL_Color COLOR_WHITE = {255, 255, 255};
static const SDL_Color COLOR_DIM = {130, 140, 160};
static SDL_Color COLOR_ACCENT = {255, 200, 60}; // replaced by theme color
static uint32_t ACCENT_HEX = 0xFFC83C;
static const uint32_t BG_COLOR = 0x1A1B26;      // dark navy
static const uint32_t PIN_BOX_COLOR = 0x2E3350; // slate
static const uint32_t PIN_BOX_ACTIVE = 0x4A5480;

// Pick up the accent color of the active Onion theme so the PIN pad and
// highlights match the rest of the system (falls back to amber).
static bool parseHexColor(const char *hex, SDL_Color *out)
{
    if (hex == NULL)
        return false;
    if (hex[0] == '#')
        hex++;
    if (strlen(hex) < 6)
        return false;
    unsigned int r, g, b;
    if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) != 3)
        return false;
    out->r = (Uint8)r;
    out->g = (Uint8)g;
    out->b = (Uint8)b;
    return true;
}

static bool themeColorFromKey(cJSON *root, const char *section, SDL_Color *out)
{
    cJSON *json_section = cJSON_GetObjectItem(root, section);
    if (json_section == NULL)
        return false;
    cJSON *json_color = cJSON_GetObjectItem(json_section, "color");
    if (json_color == NULL)
        json_color = cJSON_GetObjectItem(json_section, "selectedcolor");
    if (json_color == NULL)
        return false;
    return parseHexColor(cJSON_GetStringValue(json_color), out);
}

static void loadThemeAccent(void)
{
    char *system_json = file_read(SYSTEM_JSON);
    if (system_json == NULL)
        return;

    char theme_path[STR_MAX] = "";
    cJSON *system_root = cJSON_Parse(system_json);
    free(system_json);
    if (system_root == NULL)
        return;
    json_getString(system_root, "theme", theme_path);
    cJSON_Delete(system_root);

    if (strlen(theme_path) == 0)
        return;

    char config_path[STR_MAX * 2];
    snprintf(config_path, sizeof(config_path), "%s%sconfig.json", theme_path,
             theme_path[strlen(theme_path) - 1] == '/' ? "" : "/");

    char *theme_json = file_read(config_path);
    if (theme_json == NULL)
        return;
    cJSON *theme_root = cJSON_Parse(theme_json);
    free(theme_json);
    if (theme_root == NULL)
        return;

    SDL_Color accent;
    if (themeColorFromKey(theme_root, "currentpage", &accent) ||
        themeColorFromKey(theme_root, "grid", &accent) ||
        themeColorFromKey(theme_root, "title", &accent)) {
        COLOR_ACCENT = accent;
        ACCENT_HEX = ((uint32_t)accent.r << 16) | ((uint32_t)accent.g << 8) |
                     accent.b;
    }
    cJSON_Delete(theme_root);
}

// On the Miyoo, image files come out of the loader 180°-rotated relative
// to text rendering — Onion's own theme_backgroundLoad() corrects this by
// rotating every loaded image (see common/theme/background.h). Do the same
// for box art. Rects and TTF text must NOT be rotated.
static void rotate180InPlace(SDL_Surface *surface)
{
    if (surface == NULL || surface->format->BytesPerPixel != 4)
        return;
    uint32_t *pixels = (uint32_t *)surface->pixels;
    int pitch = surface->pitch / 4;
    int total = surface->h * pitch;
    for (int i = 0, j = total - 1; i < j; i++, j--) {
        uint32_t tmp = pixels[i];
        pixels[i] = pixels[j];
        pixels[j] = tmp;
    }
}

// The device's libSDL_rotozoom flips zoomed surfaces vertically, so scale
// box art ourselves (simple bilinear, ARGB8888 in and out).
static SDL_Surface *scaleSurface(SDL_Surface *src, int dst_w, int dst_h)
{
    if (src == NULL || dst_w < 1 || dst_h < 1)
        return NULL;

    SDL_Surface *src32 = SDL_CreateRGBSurface(
        SDL_SWSURFACE, src->w, src->h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF,
        0xFF000000);
    if (src32 == NULL)
        return NULL;
    SDL_SetAlpha(src, 0, 255); // copy alpha channel as-is
    SDL_BlitSurface(src, NULL, src32, NULL);

    SDL_Surface *dst = SDL_CreateRGBSurface(
        SDL_SWSURFACE, dst_w, dst_h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF,
        0xFF000000);
    if (dst == NULL) {
        SDL_FreeSurface(src32);
        return NULL;
    }

    uint32_t *sp = (uint32_t *)src32->pixels;
    uint32_t *dp = (uint32_t *)dst->pixels;
    int sw = src32->w, sh = src32->h;
    int spitch = src32->pitch / 4, dpitch = dst->pitch / 4;

    for (int y = 0; y < dst_h; y++) {
        double fy = ((double)y + 0.5) * sh / dst_h - 0.5;
        int y0 = (int)fy;
        if (y0 < 0)
            y0 = 0;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        double wy = fy - y0;
        if (wy < 0)
            wy = 0;

        for (int x = 0; x < dst_w; x++) {
            double fx = ((double)x + 0.5) * sw / dst_w - 0.5;
            int x0 = (int)fx;
            if (x0 < 0)
                x0 = 0;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            double wx = fx - x0;
            if (wx < 0)
                wx = 0;

            uint32_t p00 = sp[y0 * spitch + x0], p01 = sp[y0 * spitch + x1];
            uint32_t p10 = sp[y1 * spitch + x0], p11 = sp[y1 * spitch + x1];

            uint32_t result = 0;
            for (int shift = 0; shift <= 24; shift += 8) {
                double c = ((p00 >> shift) & 0xFF) * (1 - wx) * (1 - wy) +
                           ((p01 >> shift) & 0xFF) * wx * (1 - wy) +
                           ((p10 >> shift) & 0xFF) * (1 - wx) * wy +
                           ((p11 >> shift) & 0xFF) * wx * wy;
                result |= ((uint32_t)(c + 0.5) & 0xFF) << shift;
            }
            dp[y * dpitch + x] = result;
        }
    }

    SDL_FreeSurface(src32);
    return dst;
}

// Results go through a file: stdout is unreliable on-device (SDL/driver
// messages land there ahead of anything we print).
static void writeResult(const char *l1, const char *l2, const char *l3)
{
    FILE *fp = fopen(RESULT_FILE, "w");
    if (fp == NULL)
        return;
    fprintf(fp, "%s\n", l1);
    if (l2 != NULL)
        fprintf(fp, "%s\n", l2);
    if (l3 != NULL)
        fprintf(fp, "%s\n", l3);
    fclose(fp);
}

static void sigHandler(int sig)
{
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        quit = true;
        break;
    default:
        break;
    }
}

static void loadFavorites(void)
{
    FILE *fp = fopen(FAVORITES_PATH, "r");
    if (fp == NULL)
        return;

    char line[STR_MAX * 6];
    while (games_count < MAX_GAMES && fgets(line, sizeof(line), fp) != NULL) {
        if (strlen(line) < 2)
            continue;

        JsonGameEntry entry = JsonGameEntry_fromJson(line);

        if (strlen(entry.launch) == 0 || strlen(entry.rompath) == 0)
            continue;
        // Skip the "Kid Mode" shortcut favorite (arms Kid Mode from MainUI)
        if (strstr(entry.launch, "/App/KidsMode/") != NULL ||
            strstr(entry.rompath, "/App/KidsMode/") != NULL)
            continue;
        // Skip favorites whose rom no longer exists (no dead-ends for the kid)
        if (access(entry.rompath, F_OK) != 0)
            continue;
        if (strlen(entry.label) == 0)
            strncpy(entry.label, "???", STR_MAX - 1);

        games[games_count++] = entry;
    }

    fclose(fp);
}

static TTF_Font *openFont(int size)
{
    const char *path = FONT_MAIN;
    if (access(path, F_OK) != 0)
        path = FONT_FALLBACK;
    return TTF_OpenFont(path, size);
}

static void drawText(const char *text, int center_x, int center_y,
                     TTF_Font *font, SDL_Color color, int max_width)
{
    if (font == NULL || text == NULL || strlen(text) == 0)
        return;

    char buf[STR_MAX];
    strncpy(buf, text, STR_MAX - 1);
    buf[STR_MAX - 1] = '\0';

    // Truncate with ellipsis until it fits
    if (max_width > 0) {
        int w = 0, h = 0;
        TTF_SizeUTF8(font, buf, &w, &h);
        while (w > max_width && strlen(buf) > 4) {
            buf[strlen(buf) - 4] = '\0';
            strcat(buf, "...");
            TTF_SizeUTF8(font, buf, &w, &h);
        }
    }

    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, buf, color);
    if (surface == NULL)
        return;

    SDL_Rect pos = {center_x - surface->w / 2, center_y - surface->h / 2};
    SDL_BlitSurface(surface, NULL, screen, &pos);
    SDL_FreeSurface(surface);
}

static void loadArtwork(void)
{
    if (artwork_index == current)
        return;

    if (artwork != NULL) {
        SDL_FreeSurface(artwork);
        artwork = NULL;
    }
    artwork_index = current;

    if (games_count == 0)
        return;

    const char *imgpath = games[current].imgpath;
    if (strlen(imgpath) == 0 || access(imgpath, F_OK) != 0)
        return;

    SDL_Surface *raw = IMG_Load(imgpath);
    if (raw == NULL)
        return;

    // Scale to fit the art box while keeping aspect ratio
    double max_w = g_display.width * 0.62;
    double max_h = g_display.height * 0.64;
    double scale_w = max_w / raw->w;
    double scale_h = max_h / raw->h;
    double scale = scale_w < scale_h ? scale_w : scale_h;

    // Always run through the scaler: it also normalizes to 32-bit ARGB,
    // which rotate180InPlace below relies on.
    SDL_Surface *scaled = scaleSurface(raw, (int)(raw->w * scale + 0.5),
                                       (int)(raw->h * scale + 0.5));
    if (scaled != NULL) {
        SDL_FreeSurface(raw);
        raw = scaled;
    }

#ifdef PLATFORM_MIYOOMINI
    rotate180InPlace(raw);
#endif

    artwork = SDL_DisplayFormatAlpha(raw);
    if (artwork == NULL)
        artwork = raw;
    else
        SDL_FreeSurface(raw);
}

static void fillRect(int x, int y, int w, int h, uint32_t color)
{
    SDL_Rect rect = {x, y, w, h};
    SDL_FillRect(screen, &rect,
                 SDL_MapRGB(screen->format, (color >> 16) & 0xFF,
                            (color >> 8) & 0xFF, color & 0xFF));
}

static void renderCarousel(TTF_Font *font_title, TTF_Font *font_arrow,
                           TTF_Font *font_small)
{
    fillRect(0, 0, g_display.width, g_display.height, BG_COLOR);
    loadArtwork();

    int cx = g_display.width / 2;
    int art_cy = (int)(g_display.height * 0.42);

    if (artwork != NULL) {
        SDL_Rect pos = {cx - artwork->w / 2, art_cy - artwork->h / 2};
        SDL_BlitSurface(artwork, NULL, screen, &pos);
    }
    else {
        // Fallback tile: colored panel, label drawn on top by title below
        int tile_w = (int)(g_display.width * 0.55);
        int tile_h = (int)(g_display.height * 0.5);
        fillRect(cx - tile_w / 2, art_cy - tile_h / 2, tile_w, tile_h,
                 PIN_BOX_COLOR);
        drawText("?", cx, art_cy, font_arrow, COLOR_DIM, 0);
    }

    // Game title
    drawText(games[current].label, cx, (int)(g_display.height * 0.84),
             font_title, COLOR_WHITE, g_display.width - 90);

    // Arrows (browsing wraps around)
    if (games_count > 1) {
        drawText("<", (int)(g_display.width * 0.05), art_cy, font_arrow,
                 COLOR_DIM, 0);
        drawText(">", (int)(g_display.width * 0.95), art_cy, font_arrow,
                 COLOR_DIM, 0);

        char counter[32];
        snprintf(counter, sizeof(counter), "%d / %d", current + 1,
                 games_count);
        drawText(counter, cx, (int)(g_display.height * 0.945), font_small,
                 COLOR_DIM, 0);
    }
}

static void renderEmpty(TTF_Font *font_title, TTF_Font *font_small)
{
    fillRect(0, 0, g_display.width, g_display.height, BG_COLOR);
    int cx = g_display.width / 2;
    drawText("No games yet!", cx, (int)(g_display.height * 0.4), font_title,
             COLOR_WHITE, g_display.width - 40);
    drawText("Ask a grown-up to add favorites", cx,
             (int)(g_display.height * 0.55), font_small, COLOR_DIM,
             g_display.width - 40);
}

// Remaining play time in seconds; -1 = timer off (file absent/invalid)
static int readRemaining(void)
{
    FILE *fp = fopen(REMAINING_FILE, "r");
    if (fp == NULL)
        return -1;
    char buf[32] = "";
    int result = -1;
    if (fgets(buf, sizeof(buf), fp) != NULL && strlen(buf) > 0 &&
        (buf[0] == '-' || (buf[0] >= '0' && buf[0] <= '9')))
        result = atoi(buf);
    fclose(fp);
    return result;
}

static void renderTimeChip(int remaining, TTF_Font *font_small)
{
    if (remaining < 0)
        return;
    int mins = (remaining + 59) / 60;
    char chip[32];
    snprintf(chip, sizeof(chip), "%d min", mins);
    SDL_Color color = mins <= 5 ? COLOR_ACCENT : COLOR_DIM;
    drawText(chip, (int)(g_display.width * 0.085),
             (int)(g_display.height * 0.045), font_small, color, 0);
}

static void renderConfirmRestart(const char *label, TTF_Font *font_title,
                                 TTF_Font *font_menu, TTF_Font *font_small)
{
    fillRect(0, 0, g_display.width, g_display.height, BG_COLOR);
    int cx = g_display.width / 2;
    drawText("Start over?", cx, (int)(g_display.height * 0.3), font_title,
             COLOR_ACCENT, g_display.width - 40);
    drawText(label, cx, (int)(g_display.height * 0.45), font_menu,
             COLOR_WHITE, g_display.width - 60);
    drawText("A: yes - from the beginning", cx,
             (int)(g_display.height * 0.65), font_small, COLOR_DIM,
             g_display.width - 40);
    drawText("B: no - keep my place", cx, (int)(g_display.height * 0.72),
             font_small, COLOR_DIM, g_display.width - 40);
}

static void renderTimesUp(TTF_Font *font_title, TTF_Font *font_small)
{
    fillRect(0, 0, g_display.width, g_display.height, BG_COLOR);
    int cx = g_display.width / 2;
    drawText("Time's up!", cx, (int)(g_display.height * 0.38), font_title,
             COLOR_ACCENT, g_display.width - 40);
    drawText("Great playing! See you next time.", cx,
             (int)(g_display.height * 0.54), font_small, COLOR_WHITE,
             g_display.width - 40);
}

static void renderMenu(int selected, int remaining, TTF_Font *font_title,
                       TTF_Font *font_menu, TTF_Font *font_small)
{
    fillRect(0, 0, g_display.width, g_display.height, BG_COLOR);
    int cx = g_display.width / 2;

    drawText("Parent Menu", cx, (int)(g_display.height * 0.14), font_title,
             COLOR_WHITE, g_display.width - 40);

    if (remaining >= 0) {
        char info[64];
        snprintf(info, sizeof(info), "Time left: %d min",
                 (remaining + 59) / 60);
        drawText(info, cx, (int)(g_display.height * 0.24), font_small,
                 COLOR_DIM, g_display.width - 40);
    }

    const char *items[MENU_COUNT];
    items[MENU_UNLOCK] = "Exit Kid Mode";
    items[MENU_ADDTIME] = "Add play time";
    items[MENU_BACK] = "Back";

    for (int i = 0; i < MENU_COUNT; i++) {
        int y = (int)(g_display.height * (0.38 + 0.12 * i));
        if (i == selected) {
            int row_h = (int)(g_display.height * 0.1);
            fillRect((int)(g_display.width * 0.14), y - row_h / 2,
                     (int)(g_display.width * 0.72), row_h, PIN_BOX_ACTIVE);
        }
        drawText(items[i], cx, y, font_menu,
                 i == selected ? COLOR_WHITE : COLOR_DIM,
                 (int)(g_display.width * 0.7));
    }
}

static void renderPin(const char *title, bool show_intro,
                      TTF_Font *font_title, TTF_Font *font_digit,
                      TTF_Font *font_small)
{
    fillRect(0, 0, g_display.width, g_display.height, BG_COLOR);
    int cx = g_display.width / 2;

    drawText(title, cx, (int)(g_display.height * 0.22), font_title,
             COLOR_WHITE, g_display.width - 40);

    if (show_intro) {
        drawText("Kids Mode shows only your favorited games", cx,
                 (int)(g_display.height * 0.3), font_small, COLOR_DIM,
                 g_display.width - 40);
        drawText("Timer, start-over, and kid-simple controls", cx,
                 (int)(g_display.height * 0.84), font_small, COLOR_DIM,
                 g_display.width - 40);
        drawText("Hold SELECT+START in Kids Mode for the parent menu", cx,
                 (int)(g_display.height * 0.9), font_small, COLOR_DIM,
                 g_display.width - 30);
    }

    int box_w = (int)(g_display.width * 0.11);
    int box_h = (int)(g_display.height * 0.19);
    int gap = box_w / 4;
    int total_w = PIN_LEN * box_w + (PIN_LEN - 1) * gap;
    int x0 = cx - total_w / 2;
    int box_cy = (int)(g_display.height * 0.47);

    for (int i = 0; i < PIN_LEN; i++) {
        int x = x0 + i * (box_w + gap);
        fillRect(x, box_cy - box_h / 2, box_w, box_h,
                 i == pin_cursor ? PIN_BOX_ACTIVE : PIN_BOX_COLOR);

        char digit[8];
        if (i == pin_cursor)
            snprintf(digit, sizeof(digit), "%d", pin_digits[i]);
        else
            snprintf(digit, sizeof(digit), "*");
        drawText(digit, x + box_w / 2, box_cy, font_digit,
                 i == pin_cursor ? COLOR_ACCENT : COLOR_WHITE, 0);
    }

    drawText("UP/DOWN: change    A: next digit", cx,
             (int)(g_display.height * 0.68), font_small, COLOR_DIM,
             g_display.width - 40);
    drawText("START: confirm    B: back", cx, (int)(g_display.height * 0.75),
             font_small, COLOR_DIM, g_display.width - 40);
}

static void renderHoldBar(uint32_t held_ms)
{
    if (held_ms < UNLOCK_BAR_SHOW_MS)
        return;
    int full_w = g_display.width;
    int w = (int)((double)full_w * ((double)held_ms / UNLOCK_HOLD_MS));
    if (w > full_w)
        w = full_w;
    fillRect(0, 0, w, 6, ACCENT_HEX);
}

static void renderPickTimer(const char *title, int minutes, bool no_off,
                            TTF_Font *font_title, TTF_Font *font_big,
                            TTF_Font *font_small)
{
    fillRect(0, 0, g_display.width, g_display.height, BG_COLOR);
    int cx = g_display.width / 2;

    drawText(title, cx, (int)(g_display.height * 0.2), font_title,
             COLOR_WHITE, g_display.width - 40);

    char value[32];
    if (minutes > 0)
        snprintf(value, sizeof(value), "%d min", minutes);
    else
        snprintf(value, sizeof(value), "OFF");
    drawText(value, cx, (int)(g_display.height * 0.45), font_big,
             COLOR_ACCENT, 0);

    drawText("<", (int)(g_display.width * 0.18), (int)(g_display.height * 0.45),
             font_title, COLOR_DIM, 0);
    drawText(">", (int)(g_display.width * 0.82), (int)(g_display.height * 0.45),
             font_title, COLOR_DIM, 0);

    drawText(no_off ? "LEFT/RIGHT: change    A: confirm"
                    : "LEFT/RIGHT: change    A: start",
             cx, (int)(g_display.height * 0.72), font_small, COLOR_DIM,
             g_display.width - 40);
    drawText(no_off ? "B: cancel" : "B: no timer", cx,
             (int)(g_display.height * 0.79), font_small, COLOR_DIM,
             g_display.width - 40);
}

static void flip(void)
{
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

int main(int argc, char *argv[])
{
    bool set_pin_mode = false;
    bool menu_mode = false;
    bool pick_timer_mode = false;
    bool picker_no_off = false;
    int menu_timer_minutes = 0;
    int menu_remaining = -1;
    char pin_title[STR_MAX] = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set-pin") == 0)
            set_pin_mode = true;
        else if (strcmp(argv[i], "--parent-menu") == 0)
            menu_mode = true;
        else if (strcmp(argv[i], "--pick-timer") == 0)
            pick_timer_mode = true;
        else if (strcmp(argv[i], "--no-off") == 0)
            picker_no_off = true;
        else if (strcmp(argv[i], "--timer") == 0 && i + 1 < argc)
            menu_timer_minutes = atoi(argv[++i]);
        else if (strcmp(argv[i], "--remaining") == 0 && i + 1 < argc)
            menu_remaining = atoi(argv[++i]);
        else if ((strcmp(argv[i], "-t") == 0 ||
                  strcmp(argv[i], "--title") == 0) &&
                 i + 1 < argc)
            strncpy(pin_title, argv[++i], STR_MAX - 1);
    }

    if (menu_timer_minutes < 0)
        menu_timer_minutes = 0;
    if (menu_timer_minutes > TIMER_MAX)
        menu_timer_minutes = TIMER_MAX;

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    log_setName("kidui");
    remove(RESULT_FILE); // no stale results from a previous run

    if (!SDL_InitDefault())
        return 1;

    TTF_Font *font_title = openFont(g_display.height / 13); // ~36px @480
    TTF_Font *font_arrow = openFont(g_display.height / 8);  // ~60px @480
    TTF_Font *font_digit = openFont(g_display.height / 9);  // ~53px @480
    TTF_Font *font_menu = openFont(g_display.height / 17);  // ~28px @480
    TTF_Font *font_small = openFont(g_display.height / 24); // ~20px @480

    Screen active_screen = SCREEN_CAROUSEL;
    int menu_selected = 0;
    int remaining = -1;

    loadThemeAccent();

    if (set_pin_mode) {
        active_screen = SCREEN_PIN;
    }
    else if (menu_mode) {
        active_screen = SCREEN_MENU;
    }
    else if (pick_timer_mode) {
        active_screen = SCREEN_PICKTIMER;
        // Arm flow defaults to no timer; add-time flow starts at one step
        menu_timer_minutes = picker_no_off ? TIMER_STEP : 0;
        if (strlen(pin_title) == 0)
            strncpy(pin_title, "Play timer", STR_MAX - 1);
    }
    else {
        loadFavorites();
        fprintf(stderr, "kidui: loaded %d favorites\n", games_count);
        remaining = readRemaining();
        if (remaining == 0)
            active_screen = SCREEN_TIMESUP;
        else if (games_count == 0)
            active_screen = SCREEN_EMPTY;
    }

    if (strlen(pin_title) == 0)
        strncpy(pin_title, "Enter PIN", STR_MAX - 1);

    KeyState keystate[320] = {(KeyState)0};
    int exit_code = 1;
    bool dirty = true;
    uint32_t hold_started = 0;
    uint32_t last_hold_ms = 0;
    uint32_t pin_last_input = SDL_GetTicks();
    uint32_t last_remaining_poll = SDL_GetTicks();

    while (!quit) {
        SDLKey changed_key = SDLK_UNKNOWN;
        uint32_t ticks = SDL_GetTicks();

        if (updateKeystate(keystate, &quit, true, &changed_key) &&
            keystate[changed_key] == PRESSED) {
            pin_last_input = ticks;

            if (active_screen == SCREEN_CAROUSEL && games_count > 0) {
                switch (changed_key) {
                case SW_BTN_RIGHT:
                case SW_BTN_DOWN:
                    current = (current + 1) % games_count;
                    dirty = true;
                    break;
                case SW_BTN_LEFT:
                case SW_BTN_UP:
                    current = (current + games_count - 1) % games_count;
                    dirty = true;
                    break;
                case SW_BTN_A:
                    writeResult("LAUNCH", games[current].launch,
                                games[current].rompath);
                    exit_code = 0;
                    quit = true;
                    break;
                case SW_BTN_X:
                    active_screen = SCREEN_CONFIRM_RESTART;
                    dirty = true;
                    break;
                default:
                    // Everything else is a no-op: no dead-ends for the kid
                    break;
                }
            }
            else if (active_screen == SCREEN_CONFIRM_RESTART) {
                switch (changed_key) {
                case SW_BTN_A:
                    writeResult("LAUNCH_FRESH", games[current].launch,
                                games[current].rompath);
                    exit_code = 0;
                    quit = true;
                    break;
                case SW_BTN_B:
                case SW_BTN_X:
                case SW_BTN_MENU:
                    active_screen = SCREEN_CAROUSEL;
                    dirty = true;
                    break;
                default:
                    break;
                }
            }
            else if (active_screen == SCREEN_PICKTIMER) {
                switch (changed_key) {
                case SW_BTN_RIGHT:
                case SW_BTN_UP:
                    menu_timer_minutes += TIMER_STEP;
                    if (menu_timer_minutes > TIMER_MAX)
                        menu_timer_minutes = TIMER_MAX;
                    dirty = true;
                    break;
                case SW_BTN_LEFT:
                case SW_BTN_DOWN:
                    menu_timer_minutes -= TIMER_STEP;
                    if (menu_timer_minutes < (picker_no_off ? TIMER_STEP : 0))
                        menu_timer_minutes = picker_no_off ? TIMER_STEP : 0;
                    dirty = true;
                    break;
                case SW_BTN_A:
                case SW_BTN_START: {
                    char minutes_str[16];
                    snprintf(minutes_str, sizeof(minutes_str), "%d",
                             menu_timer_minutes);
                    writeResult("TIMER", minutes_str, NULL);
                    exit_code = 5;
                    quit = true;
                    break;
                }
                case SW_BTN_B:
                    if (picker_no_off) {
                        // add-time flow: B cancels
                        exit_code = 1;
                        quit = true;
                    }
                    else {
                        // arm flow: B = the default, no timer
                        writeResult("TIMER", "0", NULL);
                        exit_code = 5;
                        quit = true;
                    }
                    break;
                default:
                    break;
                }
            }
            else if (active_screen == SCREEN_MENU) {
                switch (changed_key) {
                case SW_BTN_UP:
                    menu_selected =
                        (menu_selected + MENU_COUNT - 1) % MENU_COUNT;
                    dirty = true;
                    break;
                case SW_BTN_DOWN:
                    menu_selected = (menu_selected + 1) % MENU_COUNT;
                    dirty = true;
                    break;
                case SW_BTN_A:
                    if (menu_selected == MENU_UNLOCK) {
                        writeResult("MENU", "UNLOCK", NULL);
                        exit_code = 5;
                        quit = true;
                    }
                    else if (menu_selected == MENU_ADDTIME) {
                        writeResult("MENU", "ADDTIME", NULL);
                        exit_code = 5;
                        quit = true;
                    }
                    else {
                        exit_code = 1;
                        quit = true;
                    }
                    break;
                case SW_BTN_B:
                    exit_code = 1;
                    quit = true;
                    break;
                default:
                    break;
                }
            }
            else if (active_screen == SCREEN_PIN) {
                switch (changed_key) {
                case SW_BTN_UP:
                    pin_digits[pin_cursor] = (pin_digits[pin_cursor] + 1) % 10;
                    dirty = true;
                    break;
                case SW_BTN_DOWN:
                    pin_digits[pin_cursor] = (pin_digits[pin_cursor] + 9) % 10;
                    dirty = true;
                    break;
                case SW_BTN_RIGHT:
                    pin_cursor = (pin_cursor + 1) % PIN_LEN;
                    dirty = true;
                    break;
                case SW_BTN_LEFT:
                    pin_cursor = (pin_cursor + PIN_LEN - 1) % PIN_LEN;
                    dirty = true;
                    break;
                case SW_BTN_A:
                    // A confirms the current digit and moves right — easy to
                    // hit mid-entry, so it must never submit a partial PIN
                    if (pin_cursor < PIN_LEN - 1) {
                        pin_cursor++;
                        dirty = true;
                    }
                    break;
                case SW_BTN_START: {
                    char pin_str[8];
                    snprintf(pin_str, sizeof(pin_str), "%d%d%d%d",
                             pin_digits[0], pin_digits[1], pin_digits[2],
                             pin_digits[3]);
                    writeResult("PIN", pin_str, NULL);
                    exit_code = 3;
                    quit = true;
                    break;
                }
                case SW_BTN_B:
                    if (set_pin_mode) {
                        exit_code = 1;
                        quit = true;
                    }
                    else {
                        active_screen = remaining == 0    ? SCREEN_TIMESUP
                                        : games_count > 0 ? SCREEN_CAROUSEL
                                                          : SCREEN_EMPTY;
                        pin_digits[0] = pin_digits[1] = pin_digits[2] =
                            pin_digits[3] = 0;
                        pin_cursor = 0;
                        dirty = true;
                    }
                    break;
                default:
                    break;
                }
            }
        }

        // SELECT+START held: parent unlock gesture (any kid-facing screen)
        if (!set_pin_mode && !menu_mode && !pick_timer_mode &&
            active_screen != SCREEN_PIN) {
            bool combo_held = keystate[SW_BTN_SELECT] != RELEASED &&
                              keystate[SW_BTN_START] != RELEASED;
            if (combo_held) {
                if (hold_started == 0)
                    hold_started = ticks;
                uint32_t held_ms = ticks - hold_started;
                if (held_ms >= UNLOCK_HOLD_MS) {
                    active_screen = SCREEN_PIN;
                    pin_digits[0] = pin_digits[1] = pin_digits[2] =
                        pin_digits[3] = 0;
                    pin_cursor = 0;
                    hold_started = 0;
                    last_hold_ms = 0;
                    pin_last_input = ticks;
                }
                if (held_ms - last_hold_ms > 40) {
                    last_hold_ms = held_ms;
                    dirty = true;
                }
            }
            else if (hold_started != 0) {
                hold_started = 0;
                last_hold_ms = 0;
                dirty = true;
            }
        }

        // PIN screen idle timeout back to the kid screen (not in set-pin mode)
        if (!set_pin_mode && active_screen == SCREEN_PIN &&
            ticks - pin_last_input > PIN_IDLE_TIMEOUT_MS) {
            active_screen = remaining == 0    ? SCREEN_TIMESUP
                            : games_count > 0 ? SCREEN_CAROUSEL
                                              : SCREEN_EMPTY;
            pin_digits[0] = pin_digits[1] = pin_digits[2] = pin_digits[3] = 0;
            pin_cursor = 0;
            dirty = true;
        }

        // Poll the play-timer file and switch screens on expiry/refill
        if (!set_pin_mode && !menu_mode && !pick_timer_mode &&
            ticks - last_remaining_poll > REMAINING_POLL_MS) {
            last_remaining_poll = ticks;
            int prev_remaining = remaining;
            remaining = readRemaining();

            if (active_screen != SCREEN_PIN) {
                if (remaining == 0 && active_screen != SCREEN_TIMESUP) {
                    active_screen = SCREEN_TIMESUP;
                    dirty = true;
                }
                else if (remaining != 0 && active_screen == SCREEN_TIMESUP) {
                    active_screen =
                        games_count > 0 ? SCREEN_CAROUSEL : SCREEN_EMPTY;
                    dirty = true;
                }
            }

            // Redraw the chip when the displayed minute count changes
            if (active_screen == SCREEN_CAROUSEL &&
                (prev_remaining + 59) / 60 != (remaining + 59) / 60)
                dirty = true;
        }

        if (quit)
            break;

        if (dirty) {
            switch (active_screen) {
            case SCREEN_CAROUSEL:
                renderCarousel(font_title, font_arrow, font_small);
                renderTimeChip(remaining, font_small);
                break;
            case SCREEN_EMPTY:
                renderEmpty(font_title, font_small);
                break;
            case SCREEN_PIN:
                renderPin(pin_title, set_pin_mode, font_title, font_digit,
                          font_small);
                break;
            case SCREEN_TIMESUP:
                renderTimesUp(font_title, font_menu);
                break;
            case SCREEN_MENU:
                renderMenu(menu_selected, menu_remaining, font_title,
                           font_menu, font_small);
                break;
            case SCREEN_PICKTIMER:
                renderPickTimer(pin_title, menu_timer_minutes, picker_no_off,
                                font_title, font_digit, font_small);
                break;
            case SCREEN_CONFIRM_RESTART:
                renderConfirmRestart(games[current].label, font_title,
                                     font_menu, font_small);
                break;
            }
            if (hold_started != 0)
                renderHoldBar(ticks - hold_started);
            flip();
            dirty = false;
        }

        msleep(10);
    }

    if (artwork != NULL)
        SDL_FreeSurface(artwork);
    if (font_title != NULL)
        TTF_CloseFont(font_title);
    if (font_arrow != NULL)
        TTF_CloseFont(font_arrow);
    if (font_digit != NULL)
        TTF_CloseFont(font_digit);
    if (font_menu != NULL)
        TTF_CloseFont(font_menu);
    if (font_small != NULL)
        TTF_CloseFont(font_small);

    // NB: deliberately no final clear+flip here — an extra page flip on the
    // device can leave the visible framebuffer page out of sync with the
    // next process (MainUI painting an invisible page after unlock).
    TTF_Quit();
    SDL_Quit();

    return exit_code;
}
