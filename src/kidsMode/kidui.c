// kidui - Kid Mode fullscreen favorites carousel for Onion OS
//
// Shows the device's favorites (/mnt/SDCARD/Roms/favourite.json) one game
// at a time: big box art, big label, left/right to browse, A to play.
// Holding SELECT+START for 3 seconds opens a 4-digit PIN entry.
//
// All screens render through Onion's own theme engine (common/theme/*):
// the active theme's fonts, colors, background, header/footer bars, list
// rows and button hints — so Kids Mode looks native next to MainUI/Tweaks.
//
// Output protocol (written to /tmp/kidmode_ui_result, consumed by
// kid_mode_loop.sh; stdout is NOT used for results because the device's
// SDL/driver stack prints noise there):
//   exit 0:  "LAUNCH" \n <launch path> \n <rom path>        (resume)
//            "LAUNCH_FRESH" \n <launch path> \n <rom path>  (start over)
//   exit 3:  "PIN" \n <4 digits>
//   exit 5:  "MENU" \n "UNLOCK"
//            "MENU" \n "ADDTIME" \n <minutes>   (inline add-time selector)
//            "MENU" \n "NOTIMER"                (turn the play timer off)
//            "TIMER" \n <minutes>               (--pick-timer mode)
//   exit 7:  "POWEROFF"  (Time's up screen sat idle for 5 minutes)
//   exit 1:  canceled / error / nothing selected (result file removed)
//
// PIN screens: UP/DOWN changes the digit, LEFT/RIGHT moves, A confirms
// (START is a silent alias). --notice "..." shows a short message under the
// PIN boxes (e.g. "Wrong PIN - try again"); it clears when the screen is
// left. --start-pin opens the carousel directly on its PIN screen, so a
// failed attempt can retry in place instead of bouncing to the kid screen.
//
// Modes:
//   kidui [--start-pin] [-t "..."] [--notice "..."]
//                                  carousel (default)
//   kidui --set-pin -t "..." [--notice "..."]
//                                  PIN entry only (for initial PIN setup)
//   kidui --parent-menu --remaining S
//                                  post-PIN parent menu (S = seconds left,
//                                  -1 = timer off). "Add play time" is an
//                                  Onion-style value selector: LEFT/RIGHT
//                                  picks 5-120 min, A/START applies, and the
//                                  info line previews the new remaining time.
//   kidui --pick-timer [--no-off] -t "..."
//                                  minutes picker; with --no-off B cancels
//                                  (exit 1) instead of choosing 0
//
// Play timer: kid_mode_loop.sh's ticker writes the remaining seconds to
// /tmp/kidmode_remaining. The carousel shows it as a small chip and flips
// to a friendly "Time's up!" screen at zero (SELECT+START still works).
// If that screen is left alone for 5 minutes, kidui exits with code 7 and
// the loop powers the device off cleanly.

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
#include "components/list.h"
#include "system/battery.h"
#include "system/keymap_sw.h"
#include "theme/background.h"
#include "theme/theme.h"
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
#define TIMESUP_OFF_MS (5 * 60 * 1000)
#define REMAINING_FILE "/tmp/kidmode_remaining"
#define RESULT_FILE "/tmp/kidmode_ui_result"

typedef enum { SCREEN_CAROUSEL,
               SCREEN_PIN,
               SCREEN_EMPTY,
               SCREEN_TIMESUP,
               SCREEN_MENU,
               SCREEN_PICKTIMER,
               SCREEN_CONFIRM_RESTART } Screen;

#define MENU_UNLOCK 0
#define MENU_ADDTIME 1
#define MENU_NOTIMER 2
#define MENU_AUTORESUME 3
#define MENU_BACK 4
#define AUTORESUME_RESULT_FILE "/tmp/kidmode_autoresume_result"
#define TIMER_STEP 5
#define TIMER_MAX 120

// Big kid-facing text sizes (the theme's own sizes are used for header,
// list rows and hints via resource_getFont)
#define GAME_LABEL_FONT_SIZE 30
#define BIG_VALUE_FONT_SIZE 48
// Longer helper sentences use the theme's LIST font (the readable upright
// face Onion pairs with its display font in the Apps menu) at a controlled
// size — theme hint fonts are display faces sized for short labels
#define INFO_FONT_SIZE 22

static bool quit = false;
static bool dirty = true; // set by any render function that needs to keep
                          // animating (e.g. a scrolling title) on the next
                          // loop tick, even with no new input
static KeyState keystate[320] = {(KeyState)0};

static JsonGameEntry games[MAX_GAMES];
static int games_count = 0;
static int current = 0;

static SDL_Surface *artwork = NULL;
static SDL_Surface *icon_x = NULL; // optional theme icon-X-54.png, loaded
                                   // once on first use; NULL if the theme
                                   // doesn't have one (checked, not
                                   // missing-file-error)
static bool icon_x_checked = false;
static int artwork_index = -1;

// Cached title layout: a game title too long for one line is split into
// two balanced lines instead (see splitTwoLines below). Recomputed only
// when the selected game changes.
static int title_for_index = -1;
static bool title_two_lines = false;
static char title_line1[STR_MAX] = "";
static char title_line2[STR_MAX] = "";

static int pin_digits[PIN_LEN] = {0, 0, 0, 0};
static int pin_cursor = 0;
static char pin_notice[STR_MAX] = ""; // short message under the PIN boxes

static TTF_Font *font_gamelabel = NULL; // theme list font, large + bold
static TTF_Font *font_bigvalue = NULL;  // theme title font, large
static TTF_Font *font_info = NULL;      // theme list font, sentence-sized

// Fonts are loaded lazily (on first actual use) rather than all three
// unconditionally at startup: each screen (PIN, timer picker, menu,
// carousel...) only needs one or two of them, and every kidui invocation
// is a separate process — loading all three every single time, even for
// screens that use none of them, was pure waste that added up across the
// several process launches a single arm sequence goes through.
static TTF_Font *getFontGameLabel(void)
{
    if (font_gamelabel == NULL) {
        font_gamelabel = theme_loadFont(theme()->path, theme()->list.font,
                                        GAME_LABEL_FONT_SIZE);
        if (font_gamelabel != NULL)
            TTF_SetFontStyle(font_gamelabel, TTF_STYLE_BOLD);
    }
    return font_gamelabel;
}

static TTF_Font *getFontBigValue(void)
{
    if (font_bigvalue == NULL)
        font_bigvalue = theme_loadFont(theme()->path, theme()->title.font,
                                       BIG_VALUE_FONT_SIZE);
    return font_bigvalue;
}

static TTF_Font *getFontInfo(void)
{
    if (font_info == NULL)
        font_info =
            theme_loadFont(theme()->path, theme()->list.font, INFO_FONT_SIZE);
    return font_info;
}

// Solid panels drawn over the theme background (PIN boxes, art fallback).
// Fixed dark slate so white text stays readable on any theme.
static const SDL_Color COLOR_WHITE = {255, 255, 255};
static const uint32_t FALLBACK_BG = 0x1A1B26; // if the theme background fails
static const uint32_t PIN_BOX_COLOR = 0x2E3350;
static const uint32_t PIN_BOX_ACTIVE = 0x4A5480;

// Accent = the active theme's "current page" color (what MainUI uses to
// highlight the active tab number)
static SDL_Color accentColor(void)
{
    return theme()->currentpage.color;
}

static uint32_t accentHex(void)
{
    SDL_Color c = accentColor();
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static int s_battery = -1;

static int batteryPercentage(void)
{
    if (s_battery < 0)
        s_battery = battery_getPercentage();
    return s_battery;
}

// On the Miyoo, image files come out of the loader 180°-rotated relative
// to text rendering — Onion's own theme_backgroundLoad() corrects this by
// rotating the loaded background (see common/theme/background.h). Do the
// same for box art. Rects and TTF text must NOT be rotated.
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

// icon-X-54.png ships inside the app itself (App/KidsMode/icon-X-54.png)
// rather than living in the active theme's folder — no dependency on the
// person having added anything to their theme, and the file travels with
// the app on every install/update.
#define ICON_X_PATH "/mnt/SDCARD/App/KidsMode/icon-X-54.png"

static SDL_Surface *loadIconX(void)
{
    SDL_Surface *img = IMG_Load(ICON_X_PATH);
    if (img == NULL)
        return NULL;
    if (img->format->BitsPerPixel != 32) {
        SDL_Surface *converted = SDL_CreateRGBSurface(
            SDL_SWSURFACE, img->w, img->h, 32, 0x000000ff, 0x0000ff00,
            0x00ff0000, 0xff000000);
        SDL_BlitSurface(img, NULL, converted, NULL);
        SDL_FreeSurface(img);
        img = converted;
    }
    if (g_scale != 1.0) {
        SDL_Surface *scaled = scaleSurface(
            img, (int)(img->w * g_scale + 0.5), (int)(img->h * g_scale + 0.5));
        SDL_FreeSurface(img);
        img = scaled;
    }
    return img;
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

typedef enum { TEXT_LEFT,
               TEXT_CENTER,
               TEXT_RIGHT } TextAlignMode;

static void drawTextAlign(const char *text, int x, int center_y,
                          TTF_Font *font, SDL_Color color, int max_width,
                          TextAlignMode align)
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

    SDL_Rect pos = {x, center_y - surface->h / 2};
    if (align == TEXT_CENTER)
        pos.x = x - surface->w / 2;
    else if (align == TEXT_RIGHT)
        pos.x = x - surface->w;
    SDL_BlitSurface(surface, NULL, screen, &pos);
    SDL_FreeSurface(surface);
}

static void drawText(const char *text, int center_x, int center_y,
                     TTF_Font *font, SDL_Color color, int max_width)
{
    drawTextAlign(text, center_x, center_y, font, color, max_width,
                  TEXT_CENTER);
}

// Splits a too-long title into exactly two lines, breaking at whichever
// space lands closest to balancing the two lines' rendered widths (not
// just character count) — used by both the carousel and the "Start over?"
// dialog so long titles wrap the same way in both places. Falls back to a
// plain middle-of-string split if there's no space to break at (a single
// very long word).
static void splitTwoLines(const char *text, TTF_Font *font, char *out1,
                          size_t out1_size, char *out2, size_t out2_size)
{
    size_t len = strlen(text);
    size_t best_split = 0;
    int best_diff = -1;
    bool found = false;

    for (size_t i = 0; i < len; i++) {
        if (text[i] != ' ')
            continue;

        char part1[STR_MAX], part2[STR_MAX];
        size_t l1 = i < sizeof(part1) - 1 ? i : sizeof(part1) - 1;
        memcpy(part1, text, l1);
        part1[l1] = '\0';
        snprintf(part2, sizeof(part2), "%s", text + i + 1);

        int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        TTF_SizeUTF8(font, part1, &w1, &h1);
        TTF_SizeUTF8(font, part2, &w2, &h2);
        int diff = w1 > w2 ? w1 - w2 : w2 - w1;

        if (!found || diff < best_diff) {
            found = true;
            best_diff = diff;
            best_split = i;
        }
    }

    if (found) {
        size_t l1 = best_split < out1_size - 1 ? best_split : out1_size - 1;
        memcpy(out1, text, l1);
        out1[l1] = '\0';
        snprintf(out2, out2_size, "%s", text + best_split + 1);
    }
    else {
        // No space anywhere (one long word) — split at the middle char
        size_t mid = len / 2;
        size_t l1 = mid < out1_size - 1 ? mid : out1_size - 1;
        memcpy(out1, text, l1);
        out1[l1] = '\0';
        snprintf(out2, out2_size, "%s", text + mid);
    }
}

static void loadArtwork(void)
{
    // Only skip reloading if we already have artwork loaded for this exact
    // slot. If a previous attempt found nothing (e.g. the scraper hadn't
    // finished writing the image yet when this favorite was added), retry
    // on every visit instead of caching that miss for the rest of the run —
    // otherwise a newly-scraped image never appears until kidui restarts.
    if (artwork_index == current && artwork != NULL)
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
    double max_h = g_display.height * 0.58;
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

// Active theme background, like every native Onion screen (guarded: a
// broken theme must not crash the kid launcher)
static void renderBase(void)
{
    SDL_Surface *bg = theme_background();
    if (bg != NULL)
        SDL_BlitSurface(bg, NULL, screen, NULL);
    else
        fillRect(0, 0, g_display.width, g_display.height, FALLBACK_BG);
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

// Small "12 min" chip in the top-right corner (where MainUI keeps its
// battery), switching to the accent color for the last 5 minutes
static void renderTimeChip(int remaining)
{
    bool battery_peek = keystate[SW_BTN_Y] != RELEASED &&
                        keystate[SW_BTN_B] != RELEASED;
    if (remaining < 0 && !battery_peek)
        return;

    if (battery_peek) {
        // Y+B held: show the theme's own battery gauge (icon + %), at
        // the exact same coordinates Onion's own MainUI header uses
        // (theme_renderHeaderBattery: centered at 596*scale, 30*scale) —
        // not a guessed position — whether or not a timer is running.
        // Read live rather than a cached value, since a play session can
        // run long enough for it to actually change. (Continued redraw
        // while held is re-armed in main()'s loop, same reasoning as the
        // scrolling title above.)
        int pct = battery_isCharging() ? 500 : battery_getPercentage();
        SDL_Surface *batt = theme_batterySurface(pct);
        if (batt != NULL) {
            SDL_Rect pos = {(int)(596.0 * g_scale) - batt->w / 2,
                            (int)(30.0 * g_scale) - batt->h / 2};
            SDL_BlitSurface(batt, NULL, screen, &pos);
            SDL_FreeSurface(batt);
        }
        return;
    }

    int mins = (remaining + 59) / 60;
    char chip[32];
    snprintf(chip, sizeof(chip), "%d min", mins);
    SDL_Color color = mins <= 5 ? accentColor() : theme()->hint.color;
    drawTextAlign(chip, (int)(620.0 * g_scale), (int)(30.0 * g_scale),
                  resource_getFont(HINT), color, 0, TEXT_RIGHT);
}

static void renderCarousel(int remaining)
{
    renderBase();
    loadArtwork();

    int cx = g_display.width / 2;
    int art_cy = (int)(g_display.height * 0.40);

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
        drawText("?", cx, art_cy, getFontBigValue(), theme()->hint.color, 0);
    }

    // Game title in the theme's list font (big + bold). Short titles now
    // sit on the top line instead of the bottom one; a title too long to
    // fit still splits into two balanced-width lines (top + bottom,
    // unchanged) instead of scrolling or truncating.
    {
        int avail_w = g_display.width - (int)(90.0 * g_scale);
        int bottom_y = (int)(400.0 * g_scale);
        int line_h = TTF_FontLineSkip(getFontGameLabel());
        int top_y = bottom_y - line_h;

        // Recompute only when the selection changes — TTF measuring/
        // rendering isn't free, no need to redo it every frame.
        if (title_for_index != current) {
            title_for_index = current;
            int w = 0, h = 0;
            TTF_SizeUTF8(getFontGameLabel(), games[current].label, &w, &h);
            if (w <= avail_w) {
                title_two_lines = false;
                snprintf(title_line1, sizeof(title_line1), "%s",
                        games[current].label);
            }
            else {
                title_two_lines = true;
                splitTwoLines(games[current].label, getFontGameLabel(),
                             title_line1, sizeof(title_line1), title_line2,
                             sizeof(title_line2));
            }
        }

        if (!title_two_lines) {
            drawText(title_line1, cx, top_y, getFontGameLabel(),
                     theme()->list.color, avail_w);
        }
        else {
            drawText(title_line1, cx, top_y, getFontGameLabel(),
                     theme()->list.color, avail_w);
            drawText(title_line2, cx, bottom_y, getFontGameLabel(),
                     theme()->list.color, avail_w);
        }
    }

    // Browse arrows (theme's own list arrows; browsing wraps around)
    if (games_count > 1) {
        SDL_Surface *arrow_left = resource_getSurface(LEFT_ARROW);
        SDL_Surface *arrow_right = resource_getSurface(RIGHT_ARROW);
        if (arrow_left != NULL) {
            SDL_Rect pos = {(int)(10.0 * g_scale), art_cy - arrow_left->h / 2};
            SDL_BlitSurface(arrow_left, NULL, screen, &pos);
        }
        if (arrow_right != NULL) {
            SDL_Rect pos = {g_display.width - (int)(10.0 * g_scale) -
                                arrow_right->w,
                            art_cy - arrow_right->h / 2};
            SDL_BlitSurface(arrow_right, NULL, screen, &pos);
        }
    }

    // Native footer: A = PLAY plus the "2/8" position indicator
    theme_renderFooter(screen);
    theme_renderStandardHint(screen, "PLAY", NULL);
    // No X-button icon ships with Onion's theme (only BUTTON_A/BUTTON_B),
    // so we draw a small badge ourselves. To guarantee it lands in the
    // right spot regardless of theme (icon size and "PLAY" label width
    // both vary per theme/font), we replicate theme_renderStandardHint's
    // own offset math exactly rather than guessing a fixed pixel position
    // — same formula Onion itself uses to place a second (B) hint after
    // the first.
    {
        int offsetX = (int)(20.0 * g_scale);
        SDL_Surface *button_a = resource_getSurface(BUTTON_A);
        if (button_a)
            offsetX += button_a->w + 5;
        int play_w = 0, play_h = 0;
        TTF_SizeUTF8(resource_getFont(HINT), "PLAY", &play_w, &play_h);
        offsetX += play_w + (int)(30.0 * g_scale);

        if (!icon_x_checked) {
            icon_x = loadIconX();
            icon_x_checked = true;
        }

        int badge_r = 0, badge_cx = offsetX;
        int badge_cy = (int)(450.0 * g_scale);

        if (icon_x != NULL) {
            badge_r = icon_x->w / 2;
            badge_cx = offsetX + badge_r;
            SDL_Rect pos = {offsetX, badge_cy - icon_x->h / 2};
            SDL_BlitSurface(icon_x, NULL, screen, &pos);
        }

        // Same single unscaled +5px gap Onion's own footer uses between
        // an icon and its label (was accidentally adding an extra scaled
        // gap on top of that, throwing off the rhythm vs "A : PLAY").
        drawTextAlign("RESTART", badge_cx + badge_r + 5, badge_cy,
                      resource_getFont(HINT), theme()->hint.color, 0,
                      TEXT_LEFT);
    }
    if (games_count > 1)
        theme_renderFooterStatus(screen, current + 1, games_count);

    renderTimeChip(remaining);
}

static void renderEmpty(void)
{
    renderBase();
    theme_renderHeader(screen, "Kids Mode", false);
    int cx = g_display.width / 2;
    drawText("No games yet!", cx, (int)(g_display.height * 0.42),
             getFontBigValue(), theme()->list.color, g_display.width - 40);
    drawText("Ask a grown-up to add favorites", cx,
             (int)(g_display.height * 0.58), getFontInfo(),
             theme()->list.color, g_display.width - 40);
    theme_renderFooter(screen);
}

static void renderConfirmRestart(const char *label, int remaining)
{
    // Dialog pops over the carousel, exactly like Onion's own prompts
    renderCarousel(remaining);

    TTF_Font *title_font = resource_getFont(TITLE);
    int dialog_w = (int)(DIALOG_WIDTH * g_scale);
    char wrapped_label[STR_MAX];
    int w = 0, h = 0;
    TTF_SizeUTF8(title_font, label, &w, &h);
    if (w <= dialog_w) {
        snprintf(wrapped_label, sizeof(wrapped_label), "%s", label);
    }
    else {
        char l1[STR_MAX], l2[STR_MAX];
        splitTwoLines(label, title_font, l1, sizeof(l1), l2, sizeof(l2));
        snprintf(wrapped_label, sizeof(wrapped_label), "%s\n%s", l1, l2);
    }

    char message[STR_MAX];
    snprintf(message, sizeof(message),
             "%s\n\nStart from the beginning?\nIn-game saves are kept.",
             wrapped_label);
    theme_renderDialog(screen, "Start over?", message, true);
}

static void renderTimesUp(void)
{
    renderBase();
    theme_renderHeader(screen, "Time's up!", false);

    int cx = g_display.width / 2;
    drawText("Great playing!", cx, (int)(g_display.height * 0.4),
             getFontBigValue(), accentColor(), g_display.width - 40);
    drawText("See you next time.", cx, (int)(g_display.height * 0.55),
             getFontInfo(), theme()->list.color, g_display.width - 40);

    theme_renderFooter(screen);
}

// Called automatically (by list_keyLeft/list_keyRight/list_activateItem)
// the instant the toggle's value changes — written immediately rather
// than only on some specific exit path, so it's picked up correctly
// whether the parent leaves via Back, B, or any other menu action.
static void onAutoresumeToggle(void *self)
{
    ListItem *item = (ListItem *)self;
    FILE *fp = fopen(AUTORESUME_RESULT_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "%d\n", item->value);
        fclose(fp);
    }
}

static void formatAddMinutes(void *self, char *out_label)
{
    ListItem *item = (ListItem *)self;
    sprintf(out_label, "+%d min", item->value * TIMER_STEP);
}

// The parent menu is a real Onion list: full-width rows, the theme's list
// font and selection background, and an Apps-menu-style value selector on
// the "Add play time" row.
static void renderMenu(List *list, int remaining)
{
    renderBase();
    theme_renderHeader(screen, "Kids Mode - Parent Menu", false);
    theme_renderHeaderBattery(screen, batteryPercentage());
    theme_renderList(screen, list);

    // Status line: current remaining time, and — while the add-time row is
    // selected — what it becomes when applied. Same font as the menu rows,
    // tucked bottom-right above the footer.
    char info[STR_MAX] = "";
    int rem_min = remaining >= 0 ? (remaining + 59) / 60 : -1;
    if (list->active_pos == MENU_ADDTIME) {
        int add_min = list->items[MENU_ADDTIME].value * TIMER_STEP;
        if (rem_min >= 0)
            snprintf(info, sizeof(info),
                     "Time left: %d min (%d min after adding)", rem_min,
                     rem_min + add_min);
        else
            snprintf(info, sizeof(info), "No timer (%d min after adding)",
                     add_min);
    }
    else if (list->active_pos == MENU_NOTIMER && rem_min >= 0) {
        snprintf(info, sizeof(info), "Time left: %d min (no limit after)",
                 rem_min);
    }
    else {
        if (rem_min >= 0)
            snprintf(info, sizeof(info), "Time left: %d min", rem_min);
        else
            snprintf(info, sizeof(info), "No timer set");
    }
    drawTextAlign(info, (int)(620.0 * g_scale), (int)(395.0 * g_scale),
                  resource_getFont(LIST), theme()->list.color,
                  g_display.width - 40, TEXT_RIGHT);

    theme_renderFooter(screen);
    theme_renderStandardHint(screen, "OK", "BACK");
    theme_renderFooterStatus(screen, list->active_pos + 1, list->item_count);
}

static void renderPin(const char *title, bool show_intro)
{
    renderBase();
    theme_renderHeader(screen, title, false);
    theme_renderHeaderBattery(screen, batteryPercentage());

    int cx = g_display.width / 2;

    if (show_intro) {
        drawText("Kids Mode shows only your favorited games,", cx,
                 (int)(88.0 * g_scale), getFontInfo(), theme()->hint.color,
                 g_display.width - 40);
        drawText("with a play timer and kid-simple controls.", cx,
                 (int)(114.0 * g_scale), getFontInfo(), theme()->hint.color,
                 g_display.width - 40);
    }

    int box_w = (int)(g_display.width * 0.11);
    int box_h = (int)(g_display.height * 0.19);
    int gap = box_w / 4;
    int total_w = PIN_LEN * box_w + (PIN_LEN - 1) * gap;
    int x0 = cx - total_w / 2;
    int box_cy = (int)(g_display.height * 0.45);

    for (int i = 0; i < PIN_LEN; i++) {
        int x = x0 + i * (box_w + gap);
        fillRect(x, box_cy - box_h / 2, box_w, box_h,
                 i == pin_cursor ? PIN_BOX_ACTIVE : PIN_BOX_COLOR);

        char digit[8];
        if (i == pin_cursor)
            snprintf(digit, sizeof(digit), "%d", pin_digits[i]);
        else
            snprintf(digit, sizeof(digit), "*");
        drawText(digit, x + box_w / 2, box_cy, getFontBigValue(),
                 i == pin_cursor ? accentColor() : COLOR_WHITE, 0);
    }

    if (strlen(pin_notice) > 0)
        drawText(pin_notice, cx, (int)(g_display.height * 0.585), getFontInfo(),
                 accentColor(), g_display.width - 40);

    drawText("UP / DOWN changes - LEFT / RIGHT moves", cx,
             (int)(g_display.height * 0.645), getFontInfo(), theme()->hint.color,
             g_display.width - 40);

    if (show_intro)
        drawText("Hold SELECT+START in Kids Mode for the parent menu", cx,
                 (int)(g_display.height * 0.725), getFontInfo(),
                 theme()->hint.color, g_display.width - 30);

    theme_renderFooter(screen);
    theme_renderStandardHint(screen, "OK", "BACK");
}

static void renderHoldBar(uint32_t held_ms)
{
    if (held_ms < UNLOCK_BAR_SHOW_MS)
        return;
    int full_w = g_display.width;
    int w = (int)((double)full_w * ((double)held_ms / UNLOCK_HOLD_MS));
    if (w > full_w)
        w = full_w;
    fillRect(0, 0, w, 6, accentHex());
}

static void renderPickTimer(const char *title, int minutes, bool no_off)
{
    renderBase();
    theme_renderHeader(screen, title, false);
    theme_renderHeaderBattery(screen, batteryPercentage());

    int cx = g_display.width / 2;
    int value_cy = (int)(g_display.height * 0.42);

    char value[32];
    if (minutes > 0)
        snprintf(value, sizeof(value), "%d min", minutes);
    else
        snprintf(value, sizeof(value), "OFF");
    drawText(value, cx, value_cy, getFontBigValue(), accentColor(), 0);

    SDL_Surface *arrow_left = resource_getSurface(LEFT_ARROW);
    SDL_Surface *arrow_right = resource_getSurface(RIGHT_ARROW);
    if (arrow_left != NULL) {
        SDL_Rect pos = {(int)(g_display.width * 0.24),
                        value_cy - arrow_left->h / 2};
        SDL_BlitSurface(arrow_left, NULL, screen, &pos);
    }
    if (arrow_right != NULL) {
        SDL_Rect pos = {(int)(g_display.width * 0.76) - arrow_right->w,
                        value_cy - arrow_right->h / 2};
        SDL_BlitSurface(arrow_right, NULL, screen, &pos);
    }

    drawText(no_off ? "How much play time to add?"
                    : "Play time for this session",
             cx, (int)(g_display.height * 0.62), getFontInfo(),
             theme()->list.color, g_display.width - 40);

    theme_renderFooter(screen);
    theme_renderStandardHint(screen, no_off ? "CONFIRM" : "START",
                             no_off ? "CANCEL" : "NO TIMER");
}

static void flip(void)
{
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
#ifdef KIDUI_SCREENSHOT_DIR
    // Dev/preview builds only (never defined on device): dump every
    // rendered frame so screens can be inspected without hardware
    static int frame_no = 0;
    char shot_path[512];
    snprintf(shot_path, sizeof(shot_path), KIDUI_SCREENSHOT_DIR "/frame%03d.bmp",
             ++frame_no);
    SDL_SaveBMP(screen, shot_path);
#endif
}

int main(int argc, char *argv[])
{
    bool set_pin_mode = false;
    bool menu_mode = false;
    bool pick_timer_mode = false;
    bool picker_no_off = false;
    bool start_on_pin = false;
    int menu_timer_minutes = 0;
    int menu_remaining = -1;
    int menu_autoresume = 0;
    char pin_title[STR_MAX] = "";
    char select_rompath[STR_MAX] = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set-pin") == 0)
            set_pin_mode = true;
        else if (strcmp(argv[i], "--parent-menu") == 0)
            menu_mode = true;
        else if (strcmp(argv[i], "--pick-timer") == 0)
            pick_timer_mode = true;
        else if (strcmp(argv[i], "--no-off") == 0)
            picker_no_off = true;
        else if (strcmp(argv[i], "--start-pin") == 0)
            start_on_pin = true;
        else if (strcmp(argv[i], "--notice") == 0 && i + 1 < argc)
            strncpy(pin_notice, argv[++i], STR_MAX - 1);
        else if (strcmp(argv[i], "--timer") == 0 && i + 1 < argc)
            menu_timer_minutes = atoi(argv[++i]);
        else if (strcmp(argv[i], "--remaining") == 0 && i + 1 < argc)
            menu_remaining = atoi(argv[++i]);
        else if (strcmp(argv[i], "--autoresume") == 0 && i + 1 < argc)
            menu_autoresume = atoi(argv[++i]);
        else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc)
            strncpy(select_rompath, argv[++i], STR_MAX - 1);
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

    Screen active_screen = SCREEN_CAROUSEL;
    int remaining = -1;

    // Parent menu list (native Onion list component)
    List menu_list = list_create(5, LIST_SMALL);
    list_addItem(&menu_list,
                 (ListItem){.label = "Exit Kids Mode", .item_type = ACTION});
    list_addItem(&menu_list, (ListItem){.label = "Add play time",
                                        .item_type = MULTIVALUE,
                                        .value_min = 1,
                                        .value_max = TIMER_MAX / TIMER_STEP,
                                        .value = 1,
                                        .value_formatter = formatAddMinutes});
    // Faded and skipped while no timer is running — nothing to turn off
    list_addItem(&menu_list, (ListItem){.label = "Turn off timer",
                                        .item_type = ACTION,
                                        .disabled = menu_remaining < 0});
    list_addItem(&menu_list, (ListItem){.label = "Auto-resume last game",
                                        .item_type = TOGGLE,
                                        .value = menu_autoresume ? 1 : 0,
                                        .action = onAutoresumeToggle});
    list_addItem(&menu_list,
                 (ListItem){.label = "Back", .item_type = ACTION});

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
        if (strlen(select_rompath) > 0) {
            for (int i = 0; i < games_count; i++) {
                if (strcmp(games[i].rompath, select_rompath) == 0) {
                    current = i;
                    break;
                }
            }
        }
        remaining = readRemaining();
        if (remaining == 0)
            active_screen = SCREEN_TIMESUP;
        else if (games_count == 0)
            active_screen = SCREEN_EMPTY;
        // Wrong-PIN retry: reopen straight on the PIN screen (B backs out
        // to the kid screen decided above)
        if (start_on_pin)
            active_screen = SCREEN_PIN;
    }

    if (strlen(pin_title) == 0)
        strncpy(pin_title, "Enter PIN", STR_MAX - 1);

    int exit_code = 1;
    dirty = true;
    uint32_t hold_started = 0;
    uint32_t last_hold_ms = 0;
    uint32_t pin_last_input = SDL_GetTicks();
    uint32_t last_remaining_poll = SDL_GetTicks();
    uint32_t timesup_since = 0; // ticks when the Time's up screen appeared

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
                    list_keyUp(&menu_list, false);
                    dirty = true;
                    break;
                case SW_BTN_DOWN:
                    list_keyDown(&menu_list, false);
                    dirty = true;
                    break;
                case SW_BTN_LEFT:
                    // Value selector on the add-time row (Apps-menu style)
                    if (list_keyLeft(&menu_list, false))
                        dirty = true;
                    break;
                case SW_BTN_RIGHT:
                    if (list_keyRight(&menu_list, false))
                        dirty = true;
                    break;
                case SW_BTN_A:
                case SW_BTN_START:
                    if (menu_list.active_pos == MENU_UNLOCK) {
                        writeResult("MENU", "UNLOCK", NULL);
                        exit_code = 5;
                        quit = true;
                    }
                    else if (menu_list.active_pos == MENU_ADDTIME) {
                        char minutes_str[16];
                        snprintf(minutes_str, sizeof(minutes_str), "%d",
                                 menu_list.items[MENU_ADDTIME].value *
                                     TIMER_STEP);
                        writeResult("MENU", "ADDTIME", minutes_str);
                        exit_code = 5;
                        quit = true;
                    }
                    else if (menu_list.active_pos == MENU_NOTIMER) {
                        writeResult("MENU", "NOTIMER", NULL);
                        exit_code = 5;
                        quit = true;
                    }
                    else if (menu_list.active_pos == MENU_AUTORESUME) {
                        // Flip in place and keep the menu open, same as
                        // Onion's own Tweaks toggles — no need to leave
                        // the menu just to change one setting.
                        list_activateItem(&menu_list);
                        dirty = true;
                    }
                    else if (menu_list.active_pos == MENU_BACK) {
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
                case SW_BTN_START: {
                    // A confirms, like everywhere else in Onion (START kept
                    // as a silent alias for old muscle memory)
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
                        pin_notice[0] = '\0';
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
            pin_notice[0] = '\0';
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

        // Nobody turned the device off after "Time's up!": power off after
        // 5 idle minutes so the battery isn't drained overnight. The
        // SELECT+START parent gesture still interrupts this (PIN screen
        // pauses the timer; it restarts fresh on return).
        if (active_screen == SCREEN_TIMESUP) {
            if (timesup_since == 0)
                timesup_since = ticks;
            if (ticks - timesup_since >= TIMESUP_OFF_MS) {
                writeResult("POWEROFF", NULL, NULL);
                exit_code = 7;
                quit = true;
            }
        }
        else {
            timesup_since = 0;
        }

        if (quit)
            break;

        if (dirty) {
            switch (active_screen) {
            case SCREEN_CAROUSEL:
                renderCarousel(remaining);
                break;
            case SCREEN_EMPTY:
                renderEmpty();
                break;
            case SCREEN_PIN:
                renderPin(pin_title, set_pin_mode);
                break;
            case SCREEN_TIMESUP:
                renderTimesUp();
                break;
            case SCREEN_MENU:
                renderMenu(&menu_list, menu_remaining);
                break;
            case SCREEN_PICKTIMER:
                renderPickTimer(pin_title, menu_timer_minutes, picker_no_off);
                break;
            case SCREEN_CONFIRM_RESTART:
                renderConfirmRestart(games[current].label, remaining);
                break;
            }
            if (hold_started != 0)
                renderHoldBar(ticks - hold_started);
            flip();
            dirty = false;
        }
        // dirty=false above would otherwise cancel the battery chip's
        // need to keep checking whether Y+B is still held — re-arm it
        // here, after that reset, so the next loop tick redraws and the
        // display swaps back the instant the combo is released.
        if (keystate[SW_BTN_Y] != RELEASED && keystate[SW_BTN_B] != RELEASED &&
            (active_screen == SCREEN_CAROUSEL ||
             active_screen == SCREEN_CONFIRM_RESTART))
            dirty = true;

        msleep(10);
    }

    if (artwork != NULL)
        SDL_FreeSurface(artwork);
    if (icon_x != NULL)
        SDL_FreeSurface(icon_x);
    if (font_gamelabel != NULL)
        TTF_CloseFont(font_gamelabel);
    if (font_bigvalue != NULL)
        TTF_CloseFont(font_bigvalue);
    if (font_info != NULL)
        TTF_CloseFont(font_info);
    list_free(&menu_list);
    resources_free();

    // NB: deliberately no final clear+flip here — an extra page flip on the
    // device can leave the visible framebuffer page out of sync with the
    // next process (MainUI painting an invisible page after unlock).
    TTF_Quit();
    SDL_Quit();

    return exit_code;
}
