// kidui - unified games and videos carousel for Onion OS
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
#include <SDL/SDL_rotozoom.h>
#include <SDL/SDL_ttf.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "components/JsonGameEntry.h"
#include "components/list.h"
#include "system/battery.h"
#include "system/display.h"
#include "system/keymap_sw.h"
#include "theme/background.h"
#include "theme/theme.h"
#include "utils/keystate.h"
#include "utils/log.h"
#include "utils/msleep.h"
#include "utils/sdl_init.h"
#include "utils/str.h"

#define MAX_GAMES 500
#define MAX_FOLDER_DEPTH 16
#define PIN_LEN 4
#define UNLOCK_HOLD_MS 3000
#define UNLOCK_BAR_SHOW_MS 800
#define PIN_IDLE_TIMEOUT_MS 30000
#define REMAINING_POLL_MS 2000
#define SELECTION_WRITE_DELAY_MS 300
#define TIMESUP_OFF_MS (5 * 60 * 1000)
#define CAROUSEL_DIM_DELAY_MS 5000
#define CAROUSEL_OFF_DELAY_MS 15000
#define CAROUSEL_DIM_RAW 3
#define CAROUSEL_DIMMED_FLAG "/tmp/kidsmode_carousel_dimmed"
#define REMAINING_FILE "/tmp/kidsmode_remaining"
#define RESULT_FILE "/tmp/kidsmode_ui_result"
#define DEFAULT_VIDEOS_DIR "/mnt/SDCARD/Media/KidsMode/Main"
#define FAVORITES_PATH "/mnt/SDCARD/Roms/favourite.json"
#define ICON_X_PATH "/mnt/SDCARD/App/KidsMode/icon-X-54.png"
#define SCREEN_REFLECTION_PATH \
    "/mnt/SDCARD/App/KidsMode/screen-reflection.png"

typedef enum { SCREEN_CAROUSEL,
               SCREEN_PIN,
               SCREEN_EMPTY,
               SCREEN_TIMESUP,
               SCREEN_MENU,
               SCREEN_CATEGORIES,
               SCREEN_PICKTIMER,
               SCREEN_CONFIRM_RESTART } Screen;

#define MENU_UNLOCK 0
#define MENU_ADDTIME 1
#define MENU_NOTIMER 2
#define MENU_LOCKFLOOR 3
#define MENU_CATEGORIES 4
#define MENU_BACK 5
#define CATEGORY_MOVIES 0
#define CATEGORY_MUSIC 1
#define CATEGORY_CARTOONS 2
#define CATEGORY_SERIES 3
#define CATEGORY_STORIES 4
#define CATEGORY_BACK 5
#define LOCKFLOOR_RESULT_FILE "/tmp/kidsmode_lockfloor_result"
#define CATEGORIES_RESULT_FILE "/tmp/kidsmode_categories_result"
#define FLOOR_STATE_FILE "/tmp/kidsmode_floor"
#define SELECTION_STATE_FILE "/tmp/kidsmode_selection"
#define GAME_SELECTION_STATE_FILE "/tmp/kidsmode_game_selection"
#define VIDEO_SELECTION_STATE_FILE "/tmp/kidsmode_video_selection"
#define FOLDER_STATE_FILE "/tmp/kidsmode_folder"
#define FOLDER_HISTORY_FILE "/tmp/kidsmode_folder_history"
#define DEFAULT_FOLDER_SELECTIONS_DIR \
    "/mnt/SDCARD/Saves/KidsMode/Main/series_selections"
#define DEFAULT_VIDEO_THUMBNAIL_CACHE_DIR \
    "/mnt/SDCARD/Saves/KidsMode/Main/artwork_cache"
#define VIDEO_THUMBNAIL_RENDER_VERSION 1
#define TIMER_STEP 5
#define TIMER_MAX 120

// Big kid-facing text sizes (the theme's own sizes are used for header,
// list rows and hints via resource_getFont)
#define GAME_LABEL_FONT_SIZE 30
#define EPISODE_MIN_FONT_SIZE 14
#define EPISODE_MAX_LINES 6
#define EPISODE_MAX_WORDS 128
#define BIG_VALUE_FONT_SIZE 48
#define RESTART_TITLE_FONT_OFFSET 4
// Longer helper sentences use the theme's LIST font (the readable upright
// face Onion pairs with its display font in the Apps menu) at a controlled
// size — theme hint fonts are display faces sized for short labels
#define INFO_FONT_SIZE 22

static bool quit = false;
static bool dirty = true; // set by any render function that needs to keep
                          // animating (e.g. a scrolling title) on the next
                          // loop tick, even with no new input
static KeyState keystate[320] = {(KeyState)0};

// Carousel backlight timer. This deliberately controls PWM brightness only:
// it never clears, redraws or flips a framebuffer page. POWER remains owned
// entirely by Onion, so normal system sleep/wake is not replaced by kidui.
static uint32_t carousel_last_activity;
static uint32_t carousel_last_backlight_check;
static uint32_t carousel_last_loop_tick;
static int carousel_backlight_stage; // 0 = lit, 1 = dim, 2 = off
static long carousel_saved_brightness = -1;
static bool carousel_was_active;
static bool carousel_system_screen_off;

static void setCarouselDimmedFlag(bool enabled)
{
    if (!enabled) {
        unlink(CAROUSEL_DIMMED_FLAG);
        return;
    }
    FILE *fp = fopen(CAROUSEL_DIMMED_FLAG, "w");
    if (fp != NULL)
        fclose(fp);
}

static void restoreCarouselBacklight(void)
{
    if (carousel_backlight_stage != 0 && carousel_saved_brightness > 0)
        display_setBrightnessRaw((uint32_t)carousel_saved_brightness);
    carousel_backlight_stage = 0;
    carousel_system_screen_off = false;
    setCarouselDimmedFlag(false);
}

static void stopCarouselDimmer(void)
{
    restoreCarouselBacklight();
    carousel_was_active = false;
    carousel_last_loop_tick = 0;
}

static void updateCarouselDimmer(uint32_t ticks, bool carousel_active)
{
    if (!carousel_active) {
        if (carousel_was_active || carousel_backlight_stage != 0)
            stopCarouselDimmer();
        return;
    }

    if (!carousel_was_active) {
        carousel_was_active = true;
        carousel_last_activity = ticks;
        long current = display_getBrightnessRaw();
        if (current > 0)
            carousel_saved_brightness = current;
    }

    // keymon suspends kidui during a real system sleep, so kidui may never
    // observe the brief zero-brightness interval itself. A long gap between
    // two loop iterations is therefore also treated as a wake: preserve the
    // brightness restored by Onion and restart the inactivity timer.
    if (carousel_last_loop_tick != 0 &&
        ticks - carousel_last_loop_tick > 1000) {
        long current = display_getBrightnessRaw();
        if (current > 0) {
            carousel_saved_brightness = current;
            carousel_backlight_stage = 0;
            carousel_system_screen_off = false;
            setCarouselDimmedFlag(false);
        }
        carousel_last_activity = ticks;
    }
    carousel_last_loop_tick = ticks;

    // Detect Onion's own display-off/display-on transition without touching
    // its POWER handling. While Onion reports zero, park our timer. Once it
    // restores the configured brightness, begin a fresh 5/15-second cycle.
    if (ticks - carousel_last_backlight_check >= 250) {
        carousel_last_backlight_check = ticks;
        long current = display_getBrightnessRaw();

        if (carousel_system_screen_off) {
            if (current > 0) {
                carousel_system_screen_off = false;
                carousel_backlight_stage = 0;
                carousel_saved_brightness = current;
                carousel_last_activity = ticks;
                setCarouselDimmedFlag(false);
            }
            else {
                carousel_last_activity = ticks;
            }
            return;
        }

        if (carousel_backlight_stage == 0 && current == 0) {
            carousel_system_screen_off = true;
            carousel_last_activity = ticks;
            return;
        }

        if (carousel_backlight_stage == 1) {
            if (current == 0) {
                carousel_system_screen_off = true;
                carousel_last_activity = ticks;
                return;
            }
            if (current > CAROUSEL_DIM_RAW) {
                carousel_backlight_stage = 0;
                carousel_saved_brightness = current;
                carousel_last_activity = ticks;
                setCarouselDimmedFlag(false);
            }
        }
        else if (carousel_backlight_stage == 2 && current > 0) {
            carousel_backlight_stage = 0;
            carousel_saved_brightness = current;
            carousel_last_activity = ticks;
            setCarouselDimmedFlag(false);
        }
    }

    if (carousel_saved_brightness <= 0)
        return;

    uint32_t idle = ticks - carousel_last_activity;
    if (carousel_backlight_stage == 0 && idle >= CAROUSEL_DIM_DELAY_MS) {
        long current = display_getBrightnessRaw();
        if (current > 0)
            carousel_saved_brightness = current;
        display_setBrightnessRaw(CAROUSEL_DIM_RAW);
        carousel_backlight_stage = 1;
        setCarouselDimmedFlag(true);
    }
    if (carousel_backlight_stage == 1 && idle >= CAROUSEL_OFF_DELAY_MS) {
        display_setBrightnessRaw(0);
        carousel_backlight_stage = 2;
        setCarouselDimmedFlag(true);
    }
}

typedef struct {
    JsonGameEntry item;
    bool is_folder;
    bool hide_label;
} VideoEntry;

static VideoEntry games[MAX_GAMES];
static int games_count = 0;
static int current = 0;
static char current_folder[STR_MAX] = "";

typedef enum { FLOOR_GAMES = 0, FLOOR_VIDEOS = 1 } ContentFloor;
static ContentFloor current_floor = FLOOR_GAMES;
static int game_selection = 0;
static int video_selection = 0;
static char videos_dir[STR_MAX] = DEFAULT_VIDEOS_DIR;
static char folder_selections_dir[STR_MAX] = DEFAULT_FOLDER_SELECTIONS_DIR;
static char folder_selections_index[STR_MAX] = "";
static char video_thumbnail_cache_dir[STR_MAX] =
    DEFAULT_VIDEO_THUMBNAIL_CACHE_DIR;
static char game_select_path[STR_MAX] = "";
static char video_select_path[STR_MAX] = "";
static int content_offset_y = 0;
static bool floor_locked = false;
static bool show_stories = true;
static bool show_movies = true;
static bool show_series = true;
static bool show_music = true;
static bool show_cartoons = true;
static bool selection_state_dirty;
static uint32_t selection_changed_at;

#define MAX_FOLDER_MEMORY 128
#define MAX_VIDEO_DIR_CACHE 512
#define MAX_ARTWORK_DIR_CACHE 64
#define VIDEO_ARTWORK_CACHE_SIZE 12
#define VIDEO_LIST_CACHE_SIZE 6
typedef struct {
    char folder[STR_MAX];
    char selection[STR_MAX];
} FolderSelectionMemory;
typedef struct {
    char path[STR_MAX];
    bool has_media;
} VideoDirCacheEntry;
typedef struct {
    char label[STR_MAX];
    char path[STR_MAX];
    int priority;
} ArtworkIndexEntry;
typedef struct {
    char folder[STR_MAX];
    ArtworkIndexEntry *entries;
    int count;
    int capacity;
} ArtworkDirIndex;
typedef struct {
    char folder[STR_MAX];
    VideoEntry *entries;
    int count;
    unsigned long age;
} VideoListCacheEntry;
static FolderSelectionMemory folder_memory[MAX_FOLDER_MEMORY];
static int folder_memory_count;
static VideoDirCacheEntry video_dir_cache[MAX_VIDEO_DIR_CACHE];
static int video_dir_cache_count;
static ArtworkDirIndex artwork_dir_cache[MAX_ARTWORK_DIR_CACHE];
static int artwork_dir_cache_count;
static VideoListCacheEntry video_list_cache[VIDEO_LIST_CACHE_SIZE];
static unsigned long video_list_cache_age;

static SDL_Surface *artwork = NULL;
// Keep the last decoded image for each floor. Switching floors normally
// returns to the same selection, so decoding and scaling it again would add
// a visible pause before every swipe.
static SDL_Surface *artwork_cache[2] = {NULL, NULL};
static char artwork_cache_path[2][STR_MAX] = {{0}, {0}};
typedef struct {
    char path[STR_MAX];
    SDL_Surface *surface;
    unsigned long age;
} VideoArtworkCacheEntry;
static VideoArtworkCacheEntry video_artwork_cache[VIDEO_ARTWORK_CACHE_SIZE];
static unsigned long video_artwork_cache_age;
static bool thumbnail_cache_dir_ready;
static SDL_Surface *crt_fallback = NULL;
static SDL_Surface *screen_reflection = NULL;
static bool screen_reflection_checked = false;
static SDL_Surface *icon_x = NULL; // optional theme icon-X-54.png, loaded
                                   // once on first use; NULL if the theme
                                   // doesn't have one (checked, not
                                   // missing-file-error)
static bool icon_x_checked = false;
static SDL_Surface *arrow_up = NULL;
static SDL_Surface *arrow_down = NULL;
static bool vertical_arrows_checked = false;
static int artwork_index = -1;

// Cached title layout: a game title too long for one line is split into
// two balanced lines instead (see splitTwoLines below). Recomputed only
// when the selected game changes.
static int title_for_index = -1;
static bool title_two_lines = false;
static char title_line1[STR_MAX] = "";
static char title_line2[STR_MAX] = "";
static int episode_title_for_index = -1;
static int episode_title_line_count = 0;
static TTF_Font *episode_title_font = NULL;
static char episode_title_lines[EPISODE_MAX_LINES][STR_MAX] = {{0}};

static int pin_digits[PIN_LEN] = {0, 0, 0, 0};
static int pin_cursor = 0;
static char pin_notice[STR_MAX] = ""; // short message under the PIN boxes

static TTF_Font *font_gamelabel = NULL; // theme list font, large + bold
// Smaller versions of the same face are loaded only for exceptionally long
// episode titles. Most titles keep the normal large carousel font.
static TTF_Font *font_episode_sizes[GAME_LABEL_FONT_SIZE + 1] = {NULL};
static TTF_Font *font_bigvalue = NULL;  // theme title font, large
static TTF_Font *font_restart_title = NULL; // restart dialog, medium-large
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

static TTF_Font *getEpisodeFont(int size)
{
    if (size >= GAME_LABEL_FONT_SIZE)
        return getFontGameLabel();
    if (size < EPISODE_MIN_FONT_SIZE)
        size = EPISODE_MIN_FONT_SIZE;
    if (font_episode_sizes[size] == NULL) {
        font_episode_sizes[size] =
            theme_loadFont(theme()->path, theme()->list.font, size);
        if (font_episode_sizes[size] != NULL)
            TTF_SetFontStyle(font_episode_sizes[size], TTF_STYLE_BOLD);
    }
    return font_episode_sizes[size];
}

static TTF_Font *getFontBigValue(void)
{
    if (font_bigvalue == NULL)
        font_bigvalue = theme_loadFont(theme()->path, theme()->title.font,
                                       BIG_VALUE_FONT_SIZE);
    return font_bigvalue;
}

static TTF_Font *getFontRestartTitle(void)
{
    if (font_restart_title == NULL)
        font_restart_title =
            theme_loadFont(theme()->path, theme()->title.font,
                           theme()->title.size + RESTART_TITLE_FONT_OFFSET);
    return font_restart_title;
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
static const SDL_Color COLOR_RESTART_RED = {235, 64, 64};
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

    // Precompute horizontal sampling once. The former implementation used
    // floating-point divisions and sixteen floating multiplies per output
    // pixel, which made the first display of a large poster noticeably slow
    // on the Cortex-A7. Fixed-point bilinear interpolation produces the same
    // smooth result with integer arithmetic.
    int *sample_x = malloc((size_t)dst_w * 3 * sizeof(*sample_x));
    if (sample_x == NULL) {
        SDL_FreeSurface(src32);
        SDL_FreeSurface(dst);
        return NULL;
    }
    int *x0_map = sample_x;
    int *x1_map = sample_x + dst_w;
    int *wx_map = sample_x + dst_w * 2;
    for (int x = 0; x < dst_w; x++) {
        long long position =
            ((long long)(2 * x + 1) * sw * 256) / (2 * dst_w) - 128;
        if (position <= 0) {
            x0_map[x] = x1_map[x] = 0;
            wx_map[x] = 0;
        }
        else {
            int x0 = (int)(position / 256);
            if (x0 >= sw - 1) {
                x0_map[x] = x1_map[x] = sw - 1;
                wx_map[x] = 0;
            }
            else {
                x0_map[x] = x0;
                x1_map[x] = x0 + 1;
                wx_map[x] = (int)(position - (long long)x0 * 256);
            }
        }
    }

    for (int y = 0; y < dst_h; y++) {
        long long position =
            ((long long)(2 * y + 1) * sh * 256) / (2 * dst_h) - 128;
        int y0, y1, wy;
        if (position <= 0) {
            y0 = y1 = 0;
            wy = 0;
        }
        else {
            y0 = (int)(position / 256);
            if (y0 >= sh - 1) {
                y0 = y1 = sh - 1;
                wy = 0;
            }
            else {
                y1 = y0 + 1;
                wy = (int)(position - (long long)y0 * 256);
            }
        }
        int inv_y = 256 - wy;
        for (int x = 0; x < dst_w; x++) {
            int x0 = x0_map[x], x1 = x1_map[x], wx = wx_map[x];
            int inv_x = 256 - wx;
            uint32_t p00 = sp[y0 * spitch + x0];
            uint32_t p01 = sp[y0 * spitch + x1];
            uint32_t p10 = sp[y1 * spitch + x0];
            uint32_t p11 = sp[y1 * spitch + x1];
            uint32_t result = 0;
            for (int shift = 0; shift <= 24; shift += 8) {
                unsigned top = ((p00 >> shift) & 0xFF) * inv_x +
                               ((p01 >> shift) & 0xFF) * wx;
                unsigned bottom = ((p10 >> shift) & 0xFF) * inv_x +
                                  ((p11 >> shift) & 0xFF) * wx;
                unsigned value = (top * inv_y + bottom * wy + 32768) >> 16;
                result |= (value & 0xFF) << shift;
            }
            dp[y * dpitch + x] = result;
        }
    }
    free(sample_x);

    SDL_FreeSurface(src32);
    return dst;
}

static uint32_t readSurfacePixel(SDL_Surface *surface, int x, int y)
{
    int bpp = surface->format->BytesPerPixel;
    uint8_t *pixel = (uint8_t *)surface->pixels + y * surface->pitch + x * bpp;
    switch (bpp) {
    case 1:
        return *pixel;
    case 2:
        return *(uint16_t *)pixel;
    case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        return pixel[0] << 16 | pixel[1] << 8 | pixel[2];
#else
        return pixel[0] | pixel[1] << 8 | pixel[2] << 16;
#endif
    default:
        return *(uint32_t *)pixel;
    }
}

static void writeSurfacePixel(SDL_Surface *surface, int x, int y,
                              uint32_t value)
{
    int bpp = surface->format->BytesPerPixel;
    uint8_t *pixel = (uint8_t *)surface->pixels + y * surface->pitch + x * bpp;
    switch (bpp) {
    case 1:
        *pixel = (uint8_t)value;
        break;
    case 2:
        *(uint16_t *)pixel = (uint16_t)value;
        break;
    case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        pixel[0] = (value >> 16) & 0xFF;
        pixel[1] = (value >> 8) & 0xFF;
        pixel[2] = value & 0xFF;
#else
        pixel[0] = value & 0xFF;
        pixel[1] = (value >> 8) & 0xFF;
        pixel[2] = (value >> 16) & 0xFF;
#endif
        break;
    default:
        *(uint32_t *)pixel = value;
        break;
    }
}

// Alpha-safe bilinear scaler for overlays. scaleSurface's fast conversion is
// ideal for opaque artwork, but SDL 1.2 can turn fully transparent source
// pixels opaque during that preliminary blit on the Miyoo build. Read RGBA
// explicitly here so the reflection can never cover the video image in black.
static SDL_Surface *scaleAlphaSurface(SDL_Surface *src, int dst_w, int dst_h)
{
    if (src == NULL || dst_w < 1 || dst_h < 1)
        return NULL;
    SDL_Surface *dst = SDL_CreateRGBSurface(
        SDL_SWSURFACE, dst_w, dst_h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF,
        0xFF000000);
    if (dst == NULL)
        return NULL;
    if (SDL_MUSTLOCK(src) && SDL_LockSurface(src) != 0) {
        SDL_FreeSurface(dst);
        return NULL;
    }

    uint32_t *out = (uint32_t *)dst->pixels;
    int out_pitch = dst->pitch / 4;
    for (int y = 0; y < dst_h; y++) {
        double fy = ((double)y + 0.5) * src->h / dst_h - 0.5;
        int y0 = (int)fy;
        if (y0 < 0)
            y0 = 0;
        int y1 = y0 + 1 < src->h ? y0 + 1 : src->h - 1;
        double wy = fy - y0;
        if (wy < 0)
            wy = 0;
        for (int x = 0; x < dst_w; x++) {
            double fx = ((double)x + 0.5) * src->w / dst_w - 0.5;
            int x0 = (int)fx;
            if (x0 < 0)
                x0 = 0;
            int x1 = x0 + 1 < src->w ? x0 + 1 : src->w - 1;
            double wx = fx - x0;
            if (wx < 0)
                wx = 0;

            uint8_t r[4], g[4], b[4], a[4];
            SDL_GetRGBA(readSurfacePixel(src, x0, y0), src->format,
                        &r[0], &g[0], &b[0], &a[0]);
            SDL_GetRGBA(readSurfacePixel(src, x1, y0), src->format,
                        &r[1], &g[1], &b[1], &a[1]);
            SDL_GetRGBA(readSurfacePixel(src, x0, y1), src->format,
                        &r[2], &g[2], &b[2], &a[2]);
            SDL_GetRGBA(readSurfacePixel(src, x1, y1), src->format,
                        &r[3], &g[3], &b[3], &a[3]);

            uint8_t *channels[] = {r, g, b, a};
            uint32_t packed[4];
            for (int c = 0; c < 4; c++) {
                double value = channels[c][0] * (1 - wx) * (1 - wy) +
                               channels[c][1] * wx * (1 - wy) +
                               channels[c][2] * (1 - wx) * wy +
                               channels[c][3] * wx * wy;
                packed[c] = (uint32_t)(value + 0.5);
            }
            out[y * out_pitch + x] = (packed[3] << 24) | (packed[0] << 16) |
                                     (packed[1] << 8) | packed[2];
        }
    }
    if (SDL_MUSTLOCK(src))
        SDL_UnlockSurface(src);
    return dst;
}

// icon-X-54.png ships inside the app itself
// (App/KidsMode/icon-X-54.png)
// rather than living in the active theme's folder — no dependency on the
// person having added anything to their theme, and the file travels with
// the app on every install/update.
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

// ScreenScraper Mix V1 bakes the same glass highlight into every square.
// Our tiny source asset is a greyscale matte on black: after resizing, turn
// its luminance into alpha (black = transparent, grey = reflected light).
// This avoids SDL 1.2 ever interpreting a transparent PNG as an opaque tile.
static SDL_Surface *loadScreenReflection(int size)
{
    if (screen_reflection_checked)
        return screen_reflection;
    screen_reflection_checked = true;

    SDL_Surface *raw = IMG_Load(SCREEN_REFLECTION_PATH);
    if (raw == NULL)
        return NULL;
    SDL_Surface *scaled = scaleAlphaSurface(raw, size, size);
    SDL_FreeSurface(raw);
    if (scaled == NULL)
        return NULL;

    // scaleAlphaSurface produces ARGB8888. This matte was extracted from the
    // pixels common to several ScreenScraper Mix V1 images, so its luminance
    // already is the reference reflection's exact opacity.
    uint32_t *pixels = (uint32_t *)scaled->pixels;
    int pitch = scaled->pitch / 4;
    for (int y = 0; y < scaled->h; y++) {
        for (int x = 0; x < scaled->w; x++) {
            uint32_t pixel = pixels[y * pitch + x];
            uint32_t red = (pixel >> 16) & 0xFF;
            uint32_t green = (pixel >> 8) & 0xFF;
            uint32_t blue = pixel & 0xFF;
            uint32_t alpha =
                (red * 30 + green * 59 + blue * 11 + 50) / 100;
            if (alpha < 2)
                alpha = 0;
            pixels[y * pitch + x] = (alpha << 24) | 0x00FFFFFF;
        }
    }

    // Keep the known ARGB8888 mask. Do not pass it through SDL's alpha
    // conversion/blitter: that path is inconsistent on the Miyoo SDL build.
    // Unlike IMG blits, the manual screen blend below already uses logical
    // screen coordinates, so rotating this mask would put the shine in the
    // opposite corner.
    screen_reflection = scaled;
    return screen_reflection;
}

// Blend the white reflection into the already-rendered screen ourselves.
// This deliberately bypasses SDL_BlitSurface: on-device it can ignore the
// mask's per-pixel alpha and turn the whole square white.
static void blendReflection(SDL_Surface *destination,
                            SDL_Surface *reflection, int dst_x, int dst_y)
{
    if (destination == NULL || reflection == NULL ||
        reflection->format->BytesPerPixel != 4)
        return;
    bool destination_locked = SDL_MUSTLOCK(destination);
    bool reflection_locked = SDL_MUSTLOCK(reflection);
    if (destination_locked && SDL_LockSurface(destination) != 0)
        return;
    if (reflection_locked && SDL_LockSurface(reflection) != 0) {
        if (destination_locked)
            SDL_UnlockSurface(destination);
        return;
    }

    uint32_t *mask = (uint32_t *)reflection->pixels;
    int mask_pitch = reflection->pitch / 4;
    for (int y = 0; y < reflection->h; y++) {
        int destination_y = dst_y + y;
        if (destination_y < 0 || destination_y >= destination->h)
            continue;
        for (int x = 0; x < reflection->w; x++) {
            int destination_x = dst_x + x;
            if (destination_x < 0 || destination_x >= destination->w)
                continue;
            uint32_t alpha = (mask[y * mask_pitch + x] >> 24) & 0xFF;
            if (alpha == 0)
                continue;

            uint8_t red, green, blue;
            SDL_GetRGB(readSurfacePixel(destination, destination_x,
                                        destination_y),
                       destination->format, &red, &green, &blue);
            // The reference samples use a neutral white reflection. The
            // apparent tint in some covers comes from the artwork below it.
            // Screen-blend towards white so the exact matte remains visible
            // without muddying or darkening the artwork.
            red += ((255 - red) * 255 * alpha + 32512) / 65025;
            green += ((255 - green) * 255 * alpha + 32512) / 65025;
            blue += ((255 - blue) * 255 * alpha + 32512) / 65025;
            writeSurfacePixel(destination, destination_x, destination_y,
                              SDL_MapRGB(destination->format, red, green,
                                         blue));
        }
    }

    if (reflection_locked)
        SDL_UnlockSurface(reflection);
    if (destination_locked)
        SDL_UnlockSurface(destination);
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

static bool hasMediaExtension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return false;
    return strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".mkv") == 0 ||
           strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".mov") == 0 ||
           strcasecmp(dot, ".m4v") == 0 || strcasecmp(dot, ".webm") == 0 ||
           strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".m4a") == 0 ||
           strcasecmp(dot, ".aac") == 0 || strcasecmp(dot, ".flac") == 0 ||
           strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".opus") == 0 ||
           strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".wma") == 0;
}

static void resetEntries(void)
{
    games_count = 0;
    current = 0;
    // artwork is owned by artwork_cache. Detach it while replacing the
    // entry list, but keep both floor caches alive for fast vertical swipes.
    artwork = NULL;
    artwork_index = -1;
    title_for_index = -1;
    episode_title_for_index = -1;
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
        if (entry.launch[0] == '\0' || entry.rompath[0] == '\0')
            continue;
        if (strstr(entry.launch, "/App/KidsMode/") != NULL ||
            access(entry.rompath, F_OK) != 0)
            continue;
        if (entry.label[0] == '\0')
            snprintf(entry.label, sizeof(entry.label), "???");
        games[games_count].item = entry;
        games[games_count].is_folder = false;
        games[games_count].hide_label = false;
        games_count++;
    }
    fclose(fp);
}

static int compareVideos(const void *a, const void *b)
{
    const VideoEntry *va = (const VideoEntry *)a;
    const VideoEntry *vb = (const VideoEntry *)b;
    if (!current_folder[0] && va->is_folder && vb->is_folder) {
        // Keep the five supplied media categories in a deliberate order.
        // Additional user folders remain alphabetically sorted afterwards.
        static const char *categories[] = {
            "Movies", "Music", "Cartoons", "Series", "Stories"};
        int order_a = 100;
        int order_b = 100;
        for (int i = 0; i < 5; i++) {
            if (strcasecmp(va->item.label, categories[i]) == 0)
                order_a = i;
            if (strcasecmp(vb->item.label, categories[i]) == 0)
                order_b = i;
        }
        if (order_a != order_b)
            return order_a - order_b;
    }
    return strcasecmp(va->item.label, vb->item.label);
}

static const char *visibleFolderName(const char *name)
{
    if (name != NULL && name[0] == '_' && name[1] != '\0')
        return name + 1;
    return name;
}

static void loadRuntimePaths(void)
{
    const char *value = getenv("KIDSMODE_MEDIA_DIR");
    if (value != NULL && value[0] != '\0')
        snprintf(videos_dir, sizeof(videos_dir), "%s", value);
    value = getenv("KIDSMODE_FOLDER_SELECTIONS_DIR");
    if (value != NULL && value[0] != '\0')
        snprintf(folder_selections_dir, sizeof(folder_selections_dir), "%s",
                 value);
    value = getenv("KIDSMODE_ARTWORK_CACHE_DIR");
    if (value != NULL && value[0] != '\0')
        snprintf(video_thumbnail_cache_dir,
                 sizeof(video_thumbnail_cache_dir), "%s", value);
    snprintf(folder_selections_index, sizeof(folder_selections_index),
             "%s/selections.tsv", folder_selections_dir);
}

static const char *folderBrowsePath(void)
{
    return current_folder[0] ? current_folder : videos_dir;
}

static void rememberFolderInMemory(const char *folder, const char *selection,
                                   bool record_change)
{
    if (folder == NULL || selection == NULL || folder[0] == '\0' ||
        selection[0] == '\0')
        return;
    size_t folder_len = strlen(folder);
    if (strncmp(selection, folder, folder_len) != 0 ||
        selection[folder_len] != '/' ||
        strchr(selection + folder_len + 1, '/') != NULL)
        return;
    int slot = -1;
    for (int i = 0; i < folder_memory_count; i++) {
        if (strcmp(folder_memory[i].folder, folder) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (folder_memory_count >= MAX_FOLDER_MEMORY)
            return;
        slot = folder_memory_count++;
        snprintf(folder_memory[slot].folder,
                 sizeof(folder_memory[slot].folder), "%s", folder);
    }
    snprintf(folder_memory[slot].selection,
             sizeof(folder_memory[slot].selection), "%s", selection);
    if (record_change) {
        FILE *fp = fopen(FOLDER_HISTORY_FILE, "a");
        if (fp != NULL) {
            fprintf(fp, "%s\t%s\n", folder, selection);
            fclose(fp);
        }
    }
}

static const char *rememberedFolderSelection(const char *folder)
{
    for (int i = 0; i < folder_memory_count; i++)
        if (strcmp(folder_memory[i].folder, folder) == 0)
            return folder_memory[i].selection;
    return NULL;
}

static void loadPersistedFolderSelections(void)
{
    FILE *index = fopen(folder_selections_index, "r");
    if (index != NULL) {
        char line[STR_MAX * 2 + 2];
        while (fgets(line, sizeof(line), index) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            char *tab = strchr(line, '\t');
            if (tab == NULL)
                continue;
            *tab = '\0';
            rememberFolderInMemory(line, tab + 1, false);
        }
        fclose(index);
        return;
    }

    // One-time migration from the older one-file-per-folder layout.
    DIR *dir = opendir(folder_selections_dir);
    if (dir == NULL)
        return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.' ||
            strcmp(de->d_name, "selections.tsv") == 0)
            continue;
        char path[STR_MAX];
        snprintf(path, sizeof(path), "%s/%s", folder_selections_dir,
                 de->d_name);
        FILE *fp = fopen(path, "r");
        if (fp == NULL)
            continue;
        char selection[STR_MAX] = "";
        if (fgets(selection, sizeof(selection), fp) != NULL) {
            selection[strcspn(selection, "\r\n")] = '\0';
            char folder[STR_MAX];
            snprintf(folder, sizeof(folder), "%s", selection);
            char *slash = strrchr(folder, '/');
            if (slash != NULL) {
                *slash = '\0';
                rememberFolderInMemory(folder, selection, false);
            }
        }
        fclose(fp);
    }
    closedir(dir);

    char tmp_index[STR_MAX];
    snprintf(tmp_index, sizeof(tmp_index), "%s.tmp", folder_selections_index);
    index = fopen(tmp_index, "w");
    if (index != NULL) {
        for (int i = 0; i < folder_memory_count; i++)
            fprintf(index, "%s\t%s\n", folder_memory[i].folder,
                    folder_memory[i].selection);
        fclose(index);
        remove(folder_selections_index);
        rename(tmp_index, folder_selections_index);
    }
}

static bool entryType(const char *path, const struct dirent *entry,
                      bool *is_regular, bool *is_directory)
{
    *is_regular = false;
    *is_directory = false;
#ifdef DT_REG
    if (entry->d_type == DT_REG) {
        *is_regular = true;
        return true;
    }
    if (entry->d_type == DT_DIR) {
        *is_directory = true;
        return true;
    }
    if (entry->d_type != DT_UNKNOWN)
        return true;
#endif
    struct stat st;
    if (lstat(path, &st) != 0)
        return false;
    *is_regular = S_ISREG(st.st_mode);
    *is_directory = S_ISDIR(st.st_mode);
    return true;
}

static bool directoryHasVideos(const char *path, int depth)
{
    for (int i = 0; i < video_dir_cache_count; i++)
        if (strcmp(video_dir_cache[i].path, path) == 0)
            return video_dir_cache[i].has_media;
    if (depth > MAX_FOLDER_DEPTH)
        return false;
    DIR *dir = opendir(path);
    if (dir == NULL)
        return false;
    struct dirent *de;
    bool found = false;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[STR_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        bool is_regular, is_directory;
        if (!entryType(child, de, &is_regular, &is_directory))
            continue;
        if (de->d_name[0] == '.')
            continue;
        if (is_regular && hasMediaExtension(de->d_name) &&
            strcasecmp(de->d_name, "FFplay controls.mp4") != 0) {
            found = true;
            break;
        }
        if (is_directory && strcasecmp(de->d_name, "Imgs") != 0 &&
            directoryHasVideos(child, depth + 1)) {
            found = true;
            break;
        }
    }
    closedir(dir);
    if (video_dir_cache_count < MAX_VIDEO_DIR_CACHE) {
        snprintf(video_dir_cache[video_dir_cache_count].path,
                 sizeof(video_dir_cache[video_dir_cache_count].path), "%s",
                 path);
        video_dir_cache[video_dir_cache_count].has_media = found;
        video_dir_cache_count++;
    }
    return found;
}

static int artworkExtensionPriority(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return -1;
    if (strcasecmp(dot, ".bmp") == 0)
        return 0;
    if (strcasecmp(dot, ".png") == 0)
        return 1;
    if (strcasecmp(dot, ".jpg") == 0)
        return 2;
    if (strcasecmp(dot, ".jpeg") == 0)
        return 3;
    return -1;
}

static ArtworkDirIndex *artworkIndexForFolder(const char *folder)
{
    for (int i = 0; i < artwork_dir_cache_count; i++)
        if (strcmp(artwork_dir_cache[i].folder, folder) == 0)
            return &artwork_dir_cache[i];
    if (artwork_dir_cache_count >= MAX_ARTWORK_DIR_CACHE)
        return NULL;

    ArtworkDirIndex *index = &artwork_dir_cache[artwork_dir_cache_count++];
    memset(index, 0, sizeof(*index));
    snprintf(index->folder, sizeof(index->folder), "%s", folder);

    char imgs_dir[STR_MAX];
    snprintf(imgs_dir, sizeof(imgs_dir), "%s/Imgs", folder);
    DIR *dir = opendir(imgs_dir);
    if (dir == NULL)
        return index; // Cache the missing Imgs directory too.

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        int priority = artworkExtensionPriority(de->d_name);
        if (priority < 0)
            continue;
        char label[STR_MAX];
        snprintf(label, sizeof(label), "%s", de->d_name);
        char *dot = strrchr(label, '.');
        if (dot == NULL)
            continue;
        *dot = '\0';

        int existing = -1;
        for (int i = 0; i < index->count; i++)
            if (strcmp(index->entries[i].label, label) == 0) {
                existing = i;
                break;
            }
        if (existing >= 0 && index->entries[existing].priority <= priority)
            continue;
        if (existing < 0) {
            if (index->count >= index->capacity) {
                int capacity = index->capacity == 0 ? 16 : index->capacity * 2;
                ArtworkIndexEntry *grown = realloc(
                    index->entries, (size_t)capacity * sizeof(*grown));
                if (grown == NULL)
                    break;
                index->entries = grown;
                index->capacity = capacity;
            }
            existing = index->count++;
        }
        snprintf(index->entries[existing].label,
                 sizeof(index->entries[existing].label), "%s", label);
        snprintf(index->entries[existing].path,
                 sizeof(index->entries[existing].path), "%s/%s", imgs_dir,
                 de->d_name);
        index->entries[existing].priority = priority;
    }
    closedir(dir);
    return index;
}

static bool findArtworkInFolder(const char *folder, const char *label,
                                char *out, size_t out_size)
{
    if (folder == NULL || folder[0] == '\0' || label == NULL ||
        label[0] == '\0')
        return false;
    ArtworkDirIndex *index = artworkIndexForFolder(folder);
    if (index == NULL)
        return false;
    for (int i = 0; i < index->count; i++)
        if (strcmp(index->entries[i].label, label) == 0) {
            snprintf(out, out_size, "%s", index->entries[i].path);
            return true;
        }
    return false;
}

static bool rootCategoryVisible(const char *name)
{
    if (strcasecmp(name, "Stories") == 0)
        return show_stories;
    if (strcasecmp(name, "Movies") == 0)
        return show_movies;
    if (strcasecmp(name, "Series") == 0)
        return show_series;
    if (strcasecmp(name, "Music") == 0)
        return show_music;
    if (strcasecmp(name, "Cartoons") == 0)
        return show_cartoons;
    return true;
}

static bool findNearestFolderArtwork(const char *folder, char *out,
                                     size_t out_size)
{
    if (folder == NULL || folder[0] == '\0')
        return false;

    char cursor[STR_MAX];
    snprintf(cursor, sizeof(cursor), "%s", folder);
    while (strcmp(cursor, videos_dir) != 0) {
        char *slash = strrchr(cursor, '/');
        if (slash == NULL || slash[1] == '\0')
            break;
        const char *folder_label = visibleFolderName(slash + 1);

        // Accept both supported cover layouts:
        //   Folder/Imgs/Folder.png
        //   Parent/Imgs/Folder.png
        if (findArtworkInFolder(cursor, folder_label, out, out_size))
            return true;
        char parent[STR_MAX];
        size_t parent_len = (size_t)(slash - cursor);
        if (parent_len >= sizeof(parent))
            break;
        memcpy(parent, cursor, parent_len);
        parent[parent_len] = '\0';
        if (findArtworkInFolder(parent, folder_label, out, out_size))
            return true;

        *slash = '\0';
        if (strncmp(cursor, videos_dir, strlen(videos_dir)) != 0)
            break;
    }
    return false;
}

static void findArtwork(const char *browse_dir, const char *item_path,
                        const char *label, bool is_folder, char *out,
                        size_t out_size)
{
    out[0] = '\0';

    // A folder can carry its own cover in Folder/Imgs/Folder.png. This
    // keeps category and series artwork beside the media it describes.
    if (is_folder &&
        findArtworkInFolder(item_path, label, out, out_size))
        return;

    // Item-specific artwork always comes from the Imgs directory beside
    // that item: Films/Imgs/Movie.jpg, Series/Show/Imgs/Episode.png, etc.
    if (findArtworkInFolder(browse_dir, label, out, out_size))
        return;

    // Keep older layouts working while local exact-name artwork takes
    // priority.
    if (strcmp(browse_dir, videos_dir) != 0 &&
        findArtworkInFolder(videos_dir, label, out, out_size))
        return;

    // An item without its own image reuses the nearest available folder
    // cover. Search upwards until the category at the media root, so content
    // can fall back to Films.png, Series.png, Songs.png, etc. Exact artwork
    // above always wins.
    if (findNearestFolderArtwork(browse_dir, out, out_size))
        return;
}

static bool loadCachedVideoList(const char *folder)
{
    for (int i = 0; i < VIDEO_LIST_CACHE_SIZE; i++) {
        if (video_list_cache[i].entries != NULL &&
            strcmp(video_list_cache[i].folder, folder) == 0) {
            games_count = video_list_cache[i].count;
            memcpy(games, video_list_cache[i].entries,
                   (size_t)games_count * sizeof(games[0]));
            video_list_cache[i].age = ++video_list_cache_age;
            return true;
        }
    }
    return false;
}

static void storeVideoList(const char *folder)
{
    if (games_count <= 0)
        return;
    int slot = -1;
    unsigned long oldest = 0;
    for (int i = 0; i < VIDEO_LIST_CACHE_SIZE; i++) {
        if (video_list_cache[i].entries == NULL) {
            slot = i;
            break;
        }
        if (slot < 0 || video_list_cache[i].age < oldest) {
            slot = i;
            oldest = video_list_cache[i].age;
        }
    }
    if (slot < 0)
        return;
    VideoEntry *copy = malloc((size_t)games_count * sizeof(*copy));
    if (copy == NULL)
        return;
    memcpy(copy, games, (size_t)games_count * sizeof(*copy));
    free(video_list_cache[slot].entries);
    snprintf(video_list_cache[slot].folder,
             sizeof(video_list_cache[slot].folder), "%s", folder);
    video_list_cache[slot].entries = copy;
    video_list_cache[slot].count = games_count;
    video_list_cache[slot].age = ++video_list_cache_age;
}

static void loadVideos(void)
{
    const char *browse_dir = current_folder[0] ? current_folder : videos_dir;
    if (loadCachedVideoList(browse_dir))
        return;
    DIR *dir = opendir(browse_dir);
    if (dir == NULL)
        return;
    struct dirent *de;
    while (games_count < MAX_GAMES && (de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char fullpath[STR_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", browse_dir, de->d_name);
        bool is_regular, is_directory;
        if (!entryType(fullpath, de, &is_regular, &is_directory))
            continue;
        if (!current_folder[0] && is_directory &&
            !rootCategoryVisible(de->d_name))
            continue;
        bool captionless_folder =
            is_directory && de->d_name[0] == '_' &&
            de->d_name[1] != '\0';
        if (de->d_name[0] == '.')
            continue;
        bool is_media = is_regular && hasMediaExtension(de->d_name);
        bool is_folder = false;
        if (is_directory && strcasecmp(de->d_name, "Imgs") != 0)
            is_folder = directoryHasVideos(fullpath, 1);
        if (!is_media && !is_folder)
            continue;
        if (is_media && strcasecmp(de->d_name, "FFplay controls.mp4") == 0)
            continue;
        JsonGameEntry entry;
        memset(&entry, 0, sizeof(entry));
        snprintf(entry.rompath, sizeof(entry.rompath), "%s", fullpath);
        snprintf(entry.label, sizeof(entry.label), "%s",
                 captionless_folder ? de->d_name + 1 : de->d_name);
        if (is_media) {
            char *dot = strrchr(entry.label, '.');
            if (dot != NULL)
                *dot = '\0';
        }
        findArtwork(browse_dir, fullpath, entry.label, is_folder,
                    entry.imgpath, sizeof(entry.imgpath));
        games[games_count].item = entry;
        games[games_count].is_folder = is_folder;
        games[games_count].hide_label = captionless_folder;
        games_count++;
    }
    closedir(dir);
    qsort(games, games_count, sizeof(games[0]), compareVideos);
    storeVideoList(browse_dir);
}

static void loadCurrentFloor(void)
{
    int wanted = current_floor == FLOOR_GAMES ? game_selection : video_selection;
    const char *wanted_path = current_floor == FLOOR_GAMES
                                  ? game_select_path
                                  : video_select_path;
    resetEntries();
    if (current_floor == FLOOR_GAMES)
        loadFavorites();
    else
        loadVideos();
    if (games_count > 0) {
        if (wanted < 0)
            wanted = 0;
        if (wanted >= games_count)
            wanted = games_count - 1;
        current = wanted;
        if (wanted_path[0] != '\0') {
            for (int i = 0; i < games_count; i++) {
                if (strcmp(games[i].item.rompath, wanted_path) == 0) {
                    current = i;
                    break;
                }
            }
        }
    }
}

static void rememberSelection(void)
{
    if (current_floor == FLOOR_GAMES)
        game_selection = current;
    else
        video_selection = current;
    if (games_count > 0) {
        snprintf(current_floor == FLOOR_GAMES ? game_select_path
                                              : video_select_path,
                 STR_MAX, "%s", games[current].item.rompath);
    }
    selection_state_dirty = true;
    selection_changed_at = SDL_GetTicks();
}

static void writeFloorState(void);
static void writeSelectionState(void);
static void writeFolderState(void);

static void rememberCurrentVideoFolder(void)
{
    if (current_floor != FLOOR_VIDEOS || games_count <= 0)
        return;
    rememberFolderInMemory(folderBrowsePath(), games[current].item.rompath,
                           true);
}

static bool enterCurrentVideoFolder(void)
{
    if (current_floor != FLOOR_VIDEOS || games_count <= 0 ||
        !games[current].is_folder)
        return false;
    char next_folder[STR_MAX];
    snprintf(next_folder, sizeof(next_folder), "%s",
             games[current].item.rompath);
    rememberCurrentVideoFolder();
    snprintf(current_folder, sizeof(current_folder), "%s", next_folder);
    const char *saved = rememberedFolderSelection(current_folder);
    video_selection = 0;
    snprintf(video_select_path, sizeof(video_select_path), "%s",
             saved != NULL ? saved : "");
    loadCurrentFloor();
    rememberSelection();
    writeFloorState();
    writeSelectionState();
    writeFolderState();
    dirty = true;
    return true;
}

static bool leaveCurrentVideoFolder(void)
{
    if (current_floor != FLOOR_VIDEOS || current_folder[0] == '\0')
        return false;
    rememberCurrentVideoFolder();
    char leaving_folder[STR_MAX];
    snprintf(leaving_folder, sizeof(leaving_folder), "%s", current_folder);
    char *slash = strrchr(current_folder, '/');
    if (slash == NULL)
        current_folder[0] = '\0';
    else {
        *slash = '\0';
        if (strcmp(current_folder, videos_dir) == 0)
            current_folder[0] = '\0';
    }
    video_selection = 0;
    snprintf(video_select_path, sizeof(video_select_path), "%s",
             leaving_folder);
    loadCurrentFloor();
    rememberSelection();
    writeFloorState();
    writeSelectionState();
    writeFolderState();
    dirty = true;
    return true;
}

static void writeFloorState(void)
{
    FILE *fp = fopen(FLOOR_STATE_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "%s\n", current_floor == FLOOR_VIDEOS ? "videos" : "games");
        fclose(fp);
    }
}

static void writeSelectionState(void)
{
    FILE *fp = fopen(SELECTION_STATE_FILE, "w");
    if (fp != NULL) {
        if (games_count > 0)
            fprintf(fp, "%s\n", games[current].item.rompath);
        else
            fprintf(fp, "\n");
        fclose(fp);
    }

    // The launcher process can visit both floors before it exits. Persist
    // both exact paths, not only the currently visible one, so browsing
    // Videos, returning to Games and launching a game does not lose the
    // last video selection (and vice versa).
    fp = fopen(GAME_SELECTION_STATE_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "%s\n", game_select_path);
        fclose(fp);
    }
    fp = fopen(VIDEO_SELECTION_STATE_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "%s\n", video_select_path);
        fclose(fp);
    }
    selection_state_dirty = false;
}

static void writeFolderState(void)
{
    FILE *fp = fopen(FOLDER_STATE_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "%s\n", current_folder);
        fclose(fp);
    }
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

// SDL_ttf's synthetic TTF_STYLE_ITALIC is ignored by the older SDL build on
// some Miyoo installations. Slant the rendered pixels directly so the item
// title is visibly italic on every theme and firmware version.
static void drawSlantedText(const char *text, int center_x, int center_y,
                            TTF_Font *font, SDL_Color color, int max_width)
{
    if (font == NULL || text == NULL || text[0] == '\0')
        return;

    char buf[STR_MAX];
    snprintf(buf, sizeof(buf), "%s", text);
    int estimated_shear = (TTF_FontHeight(font) + 3) / 4;
    if (max_width > 0) {
        int w = 0, h = 0;
        TTF_SizeUTF8(font, buf, &w, &h);
        while (w + estimated_shear > max_width && strlen(buf) > 4) {
            buf[strlen(buf) - 4] = '\0';
            strcat(buf, "...");
            TTF_SizeUTF8(font, buf, &w, &h);
        }
    }

    SDL_Surface *plain = TTF_RenderUTF8_Blended(font, buf, color);
    if (plain == NULL)
        return;
    int shear = (plain->h + 3) / 4;
    SDL_Surface *slanted = SDL_CreateRGBSurface(
        SDL_SWSURFACE, plain->w + shear, plain->h, 32,
        plain->format->Rmask, plain->format->Gmask, plain->format->Bmask,
        plain->format->Amask);
    if (slanted == NULL) {
        SDL_FreeSurface(plain);
        drawText(buf, center_x, center_y, font, color, max_width);
        return;
    }
    SDL_FillRect(slanted, NULL,
                 SDL_MapRGBA(slanted->format, 0, 0, 0, 0));
    if (SDL_LockSurface(plain) == 0) {
        if (SDL_LockSurface(slanted) == 0) {
            for (int y = 0; y < plain->h; y++) {
                int shift = plain->h > 1
                                ? ((plain->h - 1 - y) * shear) /
                                      (plain->h - 1)
                                : 0;
                uint8_t *source_row = (uint8_t *)plain->pixels +
                                      y * plain->pitch;
                uint8_t *target_row = (uint8_t *)slanted->pixels +
                                      y * slanted->pitch + shift * 4;
                memcpy(target_row, source_row, (size_t)plain->w * 4);
            }
            SDL_UnlockSurface(slanted);
        }
        SDL_UnlockSurface(plain);
    }
    SDL_SetAlpha(slanted, SDL_SRCALPHA, 255);
    SDL_Rect pos = {center_x - slanted->w / 2,
                    center_y - slanted->h / 2};
    SDL_BlitSurface(slanted, NULL, screen, &pos);
    SDL_FreeSurface(slanted);
    SDL_FreeSurface(plain);
}

static void drawGlowingText(const char *text, int center_x, int center_y,
                            TTF_Font *font, SDL_Color color, int max_width)
{
    SDL_Color glow = {(Uint8)(color.r / 3), (Uint8)(color.g / 3),
                      (Uint8)(color.b / 3)};
    const int offsets[][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2},
                              {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        drawText(text, center_x + offsets[i][0], center_y + offsets[i][1],
                 font, glow, max_width);
    drawText(text, center_x, center_y, font, color, max_width);
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

static void copyTrimmedRange(const char *start, size_t length, char *out,
                             size_t out_size)
{
    while (length > 0 && *start == ' ') {
        start++;
        length--;
    }
    while (length > 0 && start[length - 1] == ' ')
        length--;
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, start, length);
    out[length] = '\0';
}

static int textWidth(TTF_Font *font, const char *text)
{
    int width = 0, height = 0;
    if (font != NULL && text != NULL)
        TTF_SizeUTF8(font, text, &width, &height);
    return width;
}

// Lay an episode title out on one to six balanced lines. Dynamic programming
// finds the fewest lines that fit and then the least ragged arrangement for
// that line count. The caller only tries a smaller font when six lines are
// not enough (or their combined height cannot fit inside the card).
static int layoutEpisodeTitle(const char *text, TTF_Font *font, int max_width,
                              char lines[EPISODE_MAX_LINES][STR_MAX])
{
    const char *word_starts[EPISODE_MAX_WORDS];
    size_t word_lengths[EPISODE_MAX_WORDS];
    int word_widths[EPISODE_MAX_WORDS];
    int word_count = 0;
    const char *cursor = text;

    for (int i = 0; i < EPISODE_MAX_LINES; i++)
        lines[i][0] = '\0';

    while (*cursor != '\0' && word_count < EPISODE_MAX_WORDS) {
        while (*cursor == ' ')
            cursor++;
        if (*cursor == '\0')
            break;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            cursor++;
        word_starts[word_count] = start;
        word_lengths[word_count] = (size_t)(cursor - start);
        word_count++;
    }
    if (word_count == 0) {
        snprintf(lines[0], STR_MAX, "%s", text);
        return 1;
    }

    // A title can never reach this in normal use, but preserve all remaining
    // text if a deliberately extreme filename contains more than 128 words.
    if (*cursor != '\0')
        word_lengths[word_count - 1] = strlen(word_starts[word_count - 1]);

    int prefix_widths[EPISODE_MAX_WORDS + 1] = {0};
    for (int i = 0; i < word_count; i++) {
        char word[STR_MAX];
        copyTrimmedRange(word_starts[i], word_lengths[i], word, sizeof(word));
        word_widths[i] = textWidth(font, word);
        prefix_widths[i + 1] = prefix_widths[i] + word_widths[i];
    }
    int space_width = textWidth(font, " ");

    const long long infinity = LLONG_MAX / 4;
    long long costs[EPISODE_MAX_LINES + 1][EPISODE_MAX_WORDS + 1];
    int previous[EPISODE_MAX_LINES + 1][EPISODE_MAX_WORDS + 1];
    for (int line = 0; line <= EPISODE_MAX_LINES; line++) {
        for (int end = 0; end <= word_count; end++) {
            costs[line][end] = infinity;
            previous[line][end] = -1;
        }
    }
    costs[0][0] = 0;

    int selected_lines = 0;
    for (int line = 1; line <= EPISODE_MAX_LINES; line++) {
        for (int end = line; end <= word_count; end++) {
            for (int start = line - 1; start < end; start++) {
                if (costs[line - 1][start] == infinity)
                    continue;
                int width = prefix_widths[end] - prefix_widths[start] +
                            (end - start - 1) * space_width;
                if (width > max_width)
                    continue;
                long long slack = max_width - width;
                long long cost = costs[line - 1][start] + slack * slack;
                if (cost < costs[line][end]) {
                    costs[line][end] = cost;
                    previous[line][end] = start;
                }
            }
        }
        if (costs[line][word_count] != infinity) {
            selected_lines = line;
            break;
        }
    }
    if (selected_lines == 0) {
        // A single long token (for example an underscore-separated episode
        // name) cannot be wrapped at spaces. Fall back to UTF-8-safe character
        // wrapping, still using all six lines before asking for a smaller
        // font. This also ensures such a title never disappears completely.
        const char *line_start = text;
        int line_count = 0;
        while (*line_start != '\0') {
            while (*line_start == ' ')
                line_start++;
            if (*line_start == '\0')
                break;
            if (line_count >= EPISODE_MAX_LINES)
                return 0;

            const char *scan = line_start;
            const char *best_end = NULL;
            const char *last_space = NULL;
            while (*scan != '\0') {
                const char *next = scan + 1;
                while ((*next & 0xC0) == 0x80)
                    next++;
                char candidate[STR_MAX];
                copyTrimmedRange(line_start, (size_t)(next - line_start),
                                 candidate, sizeof(candidate));
                if (textWidth(font, candidate) > max_width)
                    break;
                best_end = next;
                if (*scan == ' ')
                    last_space = scan;
                scan = next;
            }
            if (best_end == NULL)
                return 0;

            const char *line_end = best_end;
            if (*best_end != '\0' && last_space != NULL &&
                last_space > line_start)
                line_end = last_space;
            copyTrimmedRange(line_start, (size_t)(line_end - line_start),
                             lines[line_count], STR_MAX);
            line_count++;
            line_start = line_end;
        }
        return line_count;
    }

    int boundaries[EPISODE_MAX_LINES + 1];
    boundaries[selected_lines] = word_count;
    for (int line = selected_lines; line > 0; line--)
        boundaries[line - 1] = previous[line][boundaries[line]];

    for (int line = 0; line < selected_lines; line++) {
        int first_word = boundaries[line];
        int last_word = boundaries[line + 1] - 1;
        const char *start = word_starts[first_word];
        const char *end = word_starts[last_word] + word_lengths[last_word];
        copyTrimmedRange(start, (size_t)(end - start), lines[line], STR_MAX);
    }
    return selected_lines;
}

static void drawOutlinedText(const char *text, int center_x, int center_y,
                             TTF_Font *font, SDL_Color color)
{
    SDL_Color outline_color = {0, 0, 0};
    int outline = (int)(1.0 * g_scale + 0.5);
    if (outline < 1)
        outline = 1;
    const int directions[][2] = {{-1, 0}, {1, 0},  {0, -1}, {0, 1},
                                  {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); i++)
        drawText(text, center_x + directions[i][0] * outline,
                 center_y + directions[i][1] * outline, font, outline_color,
                 0);
    drawText(text, center_x, center_y, font, color, 0);
}

static void drawAdaptiveCardTitle(const char *text, int center_x,
                                  int art_center_y, int tile_size,
                                  SDL_Color color, bool glow)
{
    int max_width = tile_size - (int)(28.0 * g_scale);
    int max_height = tile_size - (int)(24.0 * g_scale);
    if (episode_title_for_index != current) {
        episode_title_for_index = current;
        episode_title_font = NULL;
        episode_title_line_count = 1;
        for (int size = GAME_LABEL_FONT_SIZE; size >= EPISODE_MIN_FONT_SIZE;
             size -= 2) {
            TTF_Font *candidate = getEpisodeFont(size);
            if (candidate == NULL)
                continue;
            episode_title_font = candidate;
            episode_title_line_count = layoutEpisodeTitle(
                text, candidate, max_width, episode_title_lines);
            if (episode_title_line_count == 0)
                continue;
            int widest = 0;
            for (int i = 0; i < episode_title_line_count; i++) {
                int width = textWidth(candidate, episode_title_lines[i]);
                if (width > widest)
                    widest = width;
            }
            if (widest <= max_width &&
                episode_title_line_count * TTF_FontLineSkip(candidate) <=
                    max_height)
                break;
        }
    }
    if (episode_title_font == NULL)
        return;

    int line_height = TTF_FontLineSkip(episode_title_font);
    int block_center_y = art_center_y;
    int first_y = block_center_y -
                  (episode_title_line_count - 1) * line_height / 2;
    for (int i = 0; i < episode_title_line_count; i++) {
        if (glow)
            drawGlowingText(episode_title_lines[i], center_x,
                            first_y + i * line_height, episode_title_font,
                            color, max_width);
        else
            drawOutlinedText(episode_title_lines[i], center_x,
                             first_y + i * line_height, episode_title_font,
                             color);
    }
}

static SDL_Surface *createCrtSurface(int width, int height)
{
    SDL_Surface *surface = SDL_CreateRGBSurface(
        SDL_SWSURFACE, width, height, 32, 0x000000ff, 0x0000ff00,
        0x00ff0000, 0xff000000);
    if (surface == NULL)
        return NULL;
    SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 3, 2, 8, 255));
    return surface;
}

static SDL_Surface *cachedVideoArtwork(const char *path)
{
    for (int i = 0; i < VIDEO_ARTWORK_CACHE_SIZE; i++) {
        if (video_artwork_cache[i].surface != NULL &&
            strcmp(video_artwork_cache[i].path, path) == 0) {
            video_artwork_cache[i].age = ++video_artwork_cache_age;
            return video_artwork_cache[i].surface;
        }
    }
    return NULL;
}

static SDL_Surface *storeVideoArtwork(const char *path, SDL_Surface *surface)
{
    int slot = -1;
    unsigned long oldest = 0;
    for (int i = 0; i < VIDEO_ARTWORK_CACHE_SIZE; i++) {
        if (video_artwork_cache[i].surface == NULL) {
            slot = i;
            break;
        }
        if (slot < 0 || video_artwork_cache[i].age < oldest) {
            slot = i;
            oldest = video_artwork_cache[i].age;
        }
    }
    if (slot < 0)
        return surface;
    if (video_artwork_cache[slot].surface != NULL)
        SDL_FreeSurface(video_artwork_cache[slot].surface);
    snprintf(video_artwork_cache[slot].path,
             sizeof(video_artwork_cache[slot].path), "%s", path);
    video_artwork_cache[slot].surface = surface;
    video_artwork_cache[slot].age = ++video_artwork_cache_age;
    return surface;
}

static unsigned long artworkPathHash(const char *path)
{
    unsigned long hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
        hash &= 0xffffffffu;
    }
    return hash;
}

static void ensureVideoThumbnailCacheDir(void)
{
    if (thumbnail_cache_dir_ready)
        return;
    mkdir("/mnt/SDCARD/Saves/KidsMode", 0777);
    mkdir(video_thumbnail_cache_dir, 0777);
    thumbnail_cache_dir_ready = true;
}

static bool videoThumbnailCachePaths(const char *source, int width, int height,
                                     char *bitmap, size_t bitmap_size,
                                     char *metadata, size_t metadata_size)
{
    int written = snprintf(bitmap, bitmap_size, "%s/cover-%08lx-%dx%d.bmp",
                           video_thumbnail_cache_dir,
                           artworkPathHash(source), width, height);
    if (written < 0 || written >= (int)bitmap_size)
        return false;
    written = snprintf(metadata, metadata_size, "%s.meta", bitmap);
    return written >= 0 && written < (int)metadata_size;
}

static bool videoThumbnailIsCurrent(const char *source, const char *bitmap,
                                    const char *metadata)
{
    if (access(bitmap, R_OK) != 0)
        return false;
    struct stat source_state;
    if (stat(source, &source_state) != 0)
        return false;
    FILE *fp = fopen(metadata, "r");
    if (fp == NULL)
        return false;
    int render_version = 0;
    long long source_size = -1;
    long long source_mtime = -1;
    int fields = fscanf(fp, "%d %lld %lld", &render_version, &source_size,
                        &source_mtime);
    fclose(fp);
    return fields == 3 &&
           render_version == VIDEO_THUMBNAIL_RENDER_VERSION &&
           source_size == (long long)source_state.st_size &&
           source_mtime == (long long)source_state.st_mtime;
}

static void saveVideoThumbnail(SDL_Surface *surface, const char *path,
                               const char *metadata, const char *source)
{
    if (surface == NULL || path == NULL || path[0] == '\0' ||
        metadata == NULL || metadata[0] == '\0')
        return;
    ensureVideoThumbnailCacheDir();
    char temporary[STR_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
        (int)sizeof(temporary))
        return;
    remove(temporary);
    if (SDL_SaveBMP(surface, temporary) == 0) {
        remove(path);
        if (rename(temporary, path) != 0) {
            remove(temporary);
            return;
        }
        struct stat source_state;
        if (stat(source, &source_state) != 0)
            return;
        char metadata_temporary[STR_MAX];
        if (snprintf(metadata_temporary, sizeof(metadata_temporary),
                     "%s.tmp", metadata) >= (int)sizeof(metadata_temporary))
            return;
        FILE *fp = fopen(metadata_temporary, "w");
        if (fp != NULL) {
            fprintf(fp, "%d %lld %lld\n", VIDEO_THUMBNAIL_RENDER_VERSION,
                    (long long)source_state.st_size,
                    (long long)source_state.st_mtime);
            fclose(fp);
            remove(metadata);
            if (rename(metadata_temporary, metadata) != 0)
                remove(metadata_temporary);
        }
    }
}

static void loadArtwork(void)
{
    if (artwork_index == current && artwork != NULL)
        return;
    artwork_index = current;
    artwork = NULL;
    if (games_count == 0)
        return;

    int cache_slot = (int)current_floor;
    const char *imgpath = games[current].item.imgpath;
    if (current_floor == FLOOR_VIDEOS && imgpath[0] != '\0') {
        artwork = cachedVideoArtwork(imgpath);
        if (artwork != NULL)
            return;
    }
    else {
        if (artwork_cache[cache_slot] != NULL && imgpath[0] != '\0' &&
            strcmp(artwork_cache_path[cache_slot], imgpath) == 0) {
            artwork = artwork_cache[cache_slot];
            return;
        }
        if (artwork_cache[cache_slot] != NULL) {
            SDL_FreeSurface(artwork_cache[cache_slot]);
            artwork_cache[cache_slot] = NULL;
        }
        artwork_cache_path[cache_slot][0] = '\0';
    }
    if (imgpath[0] == '\0')
        return;
    int target_h = (int)(g_display.height * 0.58);
    int target_w = target_h;
    char thumbnail_path[STR_MAX] = "";
    char thumbnail_metadata[STR_MAX] = "";
    if (current_floor == FLOOR_VIDEOS)
        ensureVideoThumbnailCacheDir();
    bool thumbnail_ready = current_floor == FLOOR_VIDEOS &&
        videoThumbnailCachePaths(imgpath, target_w, target_h, thumbnail_path,
                                 sizeof(thumbnail_path), thumbnail_metadata,
                                 sizeof(thumbnail_metadata)) &&
        videoThumbnailIsCurrent(imgpath, thumbnail_path, thumbnail_metadata);
    SDL_Surface *raw = IMG_Load(thumbnail_ready ? thumbnail_path : imgpath);
    if (raw == NULL && thumbnail_ready) {
        // A partially written/corrupt cache must never hide the real poster.
        remove(thumbnail_path);
        remove(thumbnail_metadata);
        thumbnail_ready = false;
        raw = IMG_Load(imgpath);
    }
    if (raw == NULL)
        return;

    // Keep the original Kids Mode artwork behaviour on the GAMES floor:
    // preserve each image's aspect ratio and fit it inside the historical
    // art box.
    if (current_floor == FLOOR_GAMES) {
        double max_w = g_display.width * 0.62;
        double max_h = g_display.height * 0.58;
        double scale_w = max_w / raw->w;
        double scale_h = max_h / raw->h;
        double scale = scale_w < scale_h ? scale_w : scale_h;
        SDL_Surface *scaled = scaleSurface(
            raw, (int)(raw->w * scale + 0.5),
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
        artwork_cache[cache_slot] = artwork;
        snprintf(artwork_cache_path[cache_slot], STR_MAX, "%s", imgpath);
        return;
    }

    if (thumbnail_ready && raw->w == target_w && raw->h == target_h) {
        artwork = SDL_DisplayFormatAlpha(raw);
        if (artwork == NULL)
            artwork = raw;
        else
            SDL_FreeSurface(raw);
        storeVideoArtwork(imgpath, artwork);
        return;
    }

    // ScreenScraper Mix V1 artwork is square once fitted to the historical
    // Kids Mode art height. Give videos that same square footprint. Fit the
    // complete image without cropping or stretching, filling the unused area
    // with black (normally pillar-boxing around a portrait poster).
    double scale_w = (double)target_w / raw->w;
    double scale_h = (double)target_h / raw->h;
    double scale = scale_w < scale_h ? scale_w : scale_h;
    int scaled_w = (int)(raw->w * scale + 0.5);
    int scaled_h = (int)(raw->h * scale + 0.5);
    if (scaled_w < 1)
        scaled_w = 1;
    if (scaled_h < 1)
        scaled_h = 1;
    SDL_Surface *scaled = scaleSurface(raw, scaled_w, scaled_h);
    SDL_FreeSurface(raw);
    if (scaled == NULL)
        return;

    SDL_Surface *framed = SDL_CreateRGBSurface(
        SDL_SWSURFACE, target_w, target_h, 32, 0x000000ff, 0x0000ff00,
        0x00ff0000, 0xff000000);
    if (framed == NULL) {
        SDL_FreeSurface(scaled);
        return;
    }
    SDL_FillRect(framed, NULL, SDL_MapRGBA(framed->format, 0, 0, 0, 255));
    SDL_Rect image_pos = {(target_w - scaled->w) / 2,
                          (target_h - scaled->h) / 2, 0, 0};
    SDL_BlitSurface(scaled, NULL, framed, &image_pos);
    SDL_FreeSurface(scaled);
    // Bake the CRT reflection into the prepared tile once. Previously the
    // launcher blended roughly 77k pixels again on every carousel redraw.
    // Blend before the Miyoo image correction so artwork and reflection are
    // rotated together; blending afterwards displayed only the reflection
    // upside down on the physical panel.
    SDL_Surface *reflection = loadScreenReflection(target_w);
    if (reflection != NULL)
        blendReflection(framed, reflection, 0, 0);
#ifdef PLATFORM_MIYOOMINI
    rotate180InPlace(framed);
#endif
    if (thumbnail_path[0] != '\0')
        saveVideoThumbnail(framed, thumbnail_path, thumbnail_metadata,
                           imgpath);
    artwork = SDL_DisplayFormatAlpha(framed);
    if (artwork == NULL)
        artwork = framed;
    else
        SDL_FreeSurface(framed);
    storeVideoArtwork(imgpath, artwork);
}

static SDL_Surface *loadCrtFallback(int width, int height)
{
    if (crt_fallback != NULL)
        return crt_fallback;
    SDL_Surface *surface = createCrtSurface(width, height);
    if (surface == NULL)
        return NULL;
#ifdef PLATFORM_MIYOOMINI
    rotate180InPlace(surface);
#endif
    crt_fallback = SDL_DisplayFormatAlpha(surface);
    if (crt_fallback == NULL)
        crt_fallback = surface;
    else
        SDL_FreeSurface(surface);
    return crt_fallback;
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
    // Always clear the complete software surface first. Some theme
    // backgrounds contain transparent pixels; blitting them directly over
    // the previous folder left parts of its artwork visible as a ghost image.
    fillRect(0, 0, g_display.width, g_display.height, FALLBACK_BG);
    SDL_Surface *bg = theme_background();
    if (bg != NULL)
        SDL_BlitSurface(bg, NULL, screen, NULL);
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
    bool battery_peek = keystate[SW_BTN_Y] != RELEASED;
    if (remaining < 0 && !battery_peek)
        return;

    if (battery_peek) {
        // Y held: show the theme's own battery gauge (icon + %), at
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

static void renderFloorIndicator(void)
{
    if (!floor_locked) {
        if (!vertical_arrows_checked) {
            SDL_Surface *right = resource_getSurface(RIGHT_ARROW);
            if (right != NULL) {
                arrow_up = rotozoomSurface(right, 90.0, 1.0, 1);
                arrow_down = rotozoomSurface(right, -90.0, 1.0, 1);
            }
            vertical_arrows_checked = true;
        }
        if (arrow_up != NULL || arrow_down != NULL) {
            int x = g_display.width / 2;
            if (current_floor == FLOOR_GAMES && arrow_up != NULL) {
                SDL_Rect pos = {x - arrow_up->w / 2,
                                (int)(30.0 * g_scale) - arrow_up->h / 2};
                SDL_BlitSurface(arrow_up, NULL, screen, &pos);
            }
            if (current_floor == FLOOR_VIDEOS && arrow_down != NULL) {
                // Keep the floor arrow visually attached to the title. The
                // second title line is centred at y=400; y=422 puts the
                // arrow just below it instead of mixing it with the footer
                // hints (PLAY / RESTART), which are centred at y=450.
                SDL_Rect pos = {x - arrow_down->w / 2,
                                (int)(422.0 * g_scale) - arrow_down->h / 2};
                SDL_BlitSurface(arrow_down, NULL, screen, &pos);
            }
        }
    }
}

static void renderCarousel(int remaining)
{
    renderBase();
    loadArtwork();

    int cx = g_display.width / 2;
    int art_cy = (int)(g_display.height * 0.40) + content_offset_y;
    int fixed_art_cy = (int)(g_display.height * 0.40);

    if (artwork != NULL) {
        SDL_Rect pos = {cx - artwork->w / 2, art_cy - artwork->h / 2};
        SDL_BlitSurface(artwork, NULL, screen, &pos);
    }
    else {
        if (current_floor == FLOOR_GAMES) {
            int tile_w = (int)(g_display.width * 0.55);
            int tile_h = (int)(g_display.height * 0.50);
            fillRect(cx - tile_w / 2, art_cy - tile_h / 2, tile_w, tile_h,
                     PIN_BOX_COLOR);
            drawText("?", cx, art_cy, getFontBigValue(),
                     theme()->hint.color, 0);
        }
        else {
        // Missing artwork: use a clean black card. For a file inside a
        // folder, this represents the folder's automatic cover; the selected
        // file name remains in the caption below.
        const char *fallback_label = games[current].item.label;
        if (current_folder[0] && !games[current].is_folder) {
            const char *slash = strrchr(current_folder, '/');
            fallback_label = visibleFolderName(
                slash != NULL ? slash + 1 : current_folder);
        }
        SDL_Color fallback_color = theme()->grid.selectedcolor;
        int tile_h = (int)(g_display.height * 0.58);
        int tile_w = tile_h;
        SDL_Surface *crt = loadCrtFallback(tile_w, tile_h);
        if (crt != NULL) {
            SDL_Rect crt_pos = {cx - tile_w / 2, art_cy - tile_h / 2};
            SDL_BlitSurface(crt, NULL, screen, &crt_pos);
        }
        else {
            fillRect(cx - tile_w / 2, art_cy - tile_h / 2, tile_w, tile_h,
                     0x000000);
        }

        drawAdaptiveCardTitle(fallback_label, cx, art_cy, tile_w,
                              fallback_color, true);
        }
    }

    // Games already contain this highlight in their ScreenScraper artwork.
    // Add it only to video/folder squares (including the no-image fallback).
    if (current_floor == FLOOR_VIDEOS && artwork == NULL) {
        int reflection_size = (int)(g_display.height * 0.58);
        SDL_Surface *reflection = loadScreenReflection(reflection_size);
        if (reflection != NULL) {
            blendReflection(screen, reflection, cx - reflection->w / 2,
                            art_cy - reflection->h / 2);
        }
        // Supplied artwork, including an inherited folder cover, stays
        // untouched. The selected file or folder name is shown below it.
    }

    // Game title in the theme's list font (big + bold). Short titles now
    // sit on the top line instead of the bottom one; a title too long to
    // fit still splits into two balanced-width lines (top + bottom,
    // unchanged) instead of scrolling or truncating.
    if (!(current_floor == FLOOR_VIDEOS && games[current].hide_label)) {
        int avail_w = g_display.width - (int)(90.0 * g_scale);
        int bottom_y = (int)(400.0 * g_scale) + content_offset_y;
        int line_h = TTF_FontLineSkip(getFontGameLabel());
        int top_y = bottom_y - line_h;
        // Folder captions use only the folder name. Files always keep their
        // own movie/episode name below the image, including when they inherit
        // a cover from the current folder or one of its parents.
        const char *display_title = games[current].item.label;

        // Recompute only when the selection changes — TTF measuring/
        // rendering isn't free, no need to redo it every frame.
        if (title_for_index != current) {
            title_for_index = current;
            int w = 0, h = 0;
            TTF_SizeUTF8(getFontGameLabel(), display_title, &w, &h);
            if (w <= avail_w) {
                title_two_lines = false;
                snprintf(title_line1, sizeof(title_line1), "%s",
                         display_title);
            }
            else {
                title_two_lines = true;
                splitTwoLines(display_title, getFontGameLabel(),
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

    // Browse arrows stay fixed while the central content slides.
    if (games_count > 1) {
        SDL_Surface *arrow_left = resource_getSurface(LEFT_ARROW);
        SDL_Surface *arrow_right = resource_getSurface(RIGHT_ARROW);
        if (arrow_left != NULL) {
            SDL_Rect pos = {(int)(10.0 * g_scale),
                            fixed_art_cy - arrow_left->h / 2};
            SDL_BlitSurface(arrow_left, NULL, screen, &pos);
        }
        if (arrow_right != NULL) {
            SDL_Rect pos = {g_display.width - (int)(10.0 * g_scale) -
                                arrow_right->w,
                            fixed_art_cy - arrow_right->h / 2};
            SDL_BlitSurface(arrow_right, NULL, screen, &pos);
        }
    }

    // Native footer: folders open with A; videos play with A; games launch.
    theme_renderFooter(screen);
    theme_renderStandardHint(
        screen, games[current].is_folder ? "OPEN"
                : current_floor == FLOOR_GAMES ? "PLAY"
                                               : "PLAY",
        games[current].is_folder && current_folder[0] ? "BACK" : NULL);
    // No X-button icon ships with Onion's theme (only BUTTON_A/BUTTON_B),
    // so we draw a small badge ourselves. To guarantee it lands in the
    // right spot regardless of theme (icon size and "PLAY" label width
    // both vary per theme/font), we replicate theme_renderStandardHint's
    // own offset math exactly rather than guessing a fixed pixel position
    // — same formula Onion itself uses to place a second (B) hint after
    // the first.
    if (!games[current].is_folder) {
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

        // Inside any media folder, B returns one level. Keep the kid-facing
        // order A:PLAY, X:RESTART, B:BACK.
        if (current_floor == FLOOR_VIDEOS && current_folder[0]) {
            int restart_w = 0, restart_h = 0;
            TTF_SizeUTF8(resource_getFont(HINT), "RESTART", &restart_w,
                         &restart_h);
            int back_x = badge_cx + badge_r + 5 + restart_w +
                         (int)(30.0 * g_scale);
            SDL_Surface *button_b = resource_getSurface(BUTTON_B);
            if (button_b != NULL) {
                SDL_Rect pos = {back_x, badge_cy - button_b->h / 2};
                SDL_BlitSurface(button_b, NULL, screen, &pos);
                back_x += button_b->w + 5;
            }
            drawTextAlign("BACK", back_x, badge_cy, resource_getFont(HINT),
                          theme()->hint.color, 0, TEXT_LEFT);
        }
    }
    if (games_count > 1)
        theme_renderFooterStatus(screen, current + 1, games_count);

    renderTimeChip(remaining);
    renderFloorIndicator();
}

static void renderEmpty(void)
{
    renderBase();
    int cx = g_display.width / 2;
    const bool videos = current_floor == FLOOR_VIDEOS;
    drawText(videos ? "No media yet!" : "No favorite games yet!", cx,
             (int)(g_display.height * 0.42),
             getFontBigValue(), theme()->list.color, g_display.width - 40);
    drawText(videos ? "Add media to Media/KidsMode"
                    : "Add games to Onion Favorites",
             cx,
             (int)(g_display.height * 0.58), getFontInfo(),
             theme()->list.color, g_display.width - 40);
    theme_renderFooter(screen);
    renderFloorIndicator();
}

static void renderConfirmRestart(const char *label, int remaining)
{
    // Dialog pops over the carousel, exactly like Onion's own prompts
    renderCarousel(remaining);

    TTF_Font *title_font = resource_getFont(TITLE);
    int dialog_w = (int)(DIALOG_WIDTH * g_scale);
    char label_line1[STR_MAX] = "";
    char label_line2[STR_MAX] = "";
    bool label_two_lines = false;
    int w = 0, h = 0;
    TTF_SizeUTF8(title_font, label, &w, &h);
    if (w <= dialog_w) {
        snprintf(label_line1, sizeof(label_line1), "%s", label);
    }
    else {
        label_two_lines = true;
        splitTwoLines(label, title_font, label_line1, sizeof(label_line1),
                      label_line2, sizeof(label_line2));
    }

    char message[STR_MAX];
    if (current_floor == FLOOR_GAMES)
        snprintf(message, sizeof(message),
                 label_two_lines
                     ? " \n \n\nStart from the beginning?\nIn-game saves are kept."
                     : " \n\nStart from the beginning?\nIn-game saves are kept.");
    else
        snprintf(message, sizeof(message),
                 label_two_lines
                     ? " \n \n\nStart from the beginning?\nSaved progress will be reset."
                     : " \n\nStart from the beginning?\nSaved progress will be reset.");
    // Let the native dialog draw its panel, message and button hints, then
    // render this short confirmation title with the larger display face.
    // The normal Onion dialog title is deliberately compact and made this
    // important destructive action too easy to overlook.
    theme_renderDialog(screen, " ", message, true);
    SDL_Surface *pop_bg = resource_getSurface(POP_BG);
    // The native +25 position hugs the upper edge. +50 centres this heading
    // in the clear space above the selected content's title.
    int title_y = (g_display.height - pop_bg->h) / 2 +
                  (int)(50.0 * g_scale);
    drawText("Start over?", g_display.width / 2, title_y,
             getFontRestartTitle(), COLOR_RESTART_RED, dialog_w);

    // Reserve the label's original lines in the native textbox above, then
    // repaint only those lines with an italic font. This keeps Onion's exact
    // message spacing and works identically for games, video and audio.
    int line_height = (int)(1.2 * TTF_FontLineSkip(title_font));
    int paragraph_spacing = (int)(0.5 * TTF_FontLineSkip(title_font));
    int visible_lines = label_two_lines ? 4 : 3;
    int textbox_height = visible_lines * line_height + paragraph_spacing;
    int first_label_y = (g_display.height - pop_bg->h) / 2 +
                        (int)(160.0 * g_scale) - textbox_height / 2 +
                        line_height / 2 - (int)(8.0 * g_scale);
    drawSlantedText(label_line1, g_display.width / 2, first_label_y,
                    title_font, theme()->grid.color, dialog_w);
    if (label_two_lines)
        drawSlantedText(label_line2, g_display.width / 2,
                        first_label_y + line_height, title_font,
                        theme()->grid.color, dialog_w);
}

static void renderTimesUp(void)
{
    renderBase();
    theme_renderHeader(screen, "Kids Mode", false);

    int cx = g_display.width / 2;
    drawText("Time's up!", cx, (int)(g_display.height * 0.4),
             getFontBigValue(), accentColor(), g_display.width - 40);
    drawText("See you next time.", cx, (int)(g_display.height * 0.55),
             getFontInfo(), theme()->list.color, g_display.width - 40);

    theme_renderFooter(screen);
}

static void formatAddMinutes(void *self, char *out_label)
{
    ListItem *item = (ListItem *)self;
    sprintf(out_label, "+%d min", item->value * TIMER_STEP);
}

static void onLockFloorToggle(void *self)
{
    ListItem *item = (ListItem *)self;
    FILE *fp = fopen(LOCKFLOOR_RESULT_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "%d\n", item->value);
        fclose(fp);
    }
}

static void writeCategoryToggle(const char *name, int value)
{
    FILE *fp = fopen(CATEGORIES_RESULT_FILE, "a");
    if (fp != NULL) {
        fprintf(fp, "%s=%d\n", name, value ? 1 : 0);
        fclose(fp);
    }
}

static void onStoriesToggle(void *self)
{
    writeCategoryToggle("stories", ((ListItem *)self)->value);
}

static void onMoviesToggle(void *self)
{
    writeCategoryToggle("movies", ((ListItem *)self)->value);
}

static void onSeriesToggle(void *self)
{
    writeCategoryToggle("series", ((ListItem *)self)->value);
}

static void onMusicToggle(void *self)
{
    writeCategoryToggle("music", ((ListItem *)self)->value);
}

static void onCartoonsToggle(void *self)
{
    writeCategoryToggle("cartoons", ((ListItem *)self)->value);
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

static void renderCategories(List *list)
{
    renderBase();
    theme_renderHeader(screen, "Visible media folders", false);
    theme_renderHeaderBattery(screen, batteryPercentage());
    theme_renderList(screen, list);
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
        drawText("Kids Mode shows favorite games", cx,
                 (int)(88.0 * g_scale), getFontInfo(), theme()->hint.color,
                 g_display.width - 40);
        drawText("and videos with kid-simple controls.", cx,
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
        drawText("Hold SELECT+START for the parent menu", cx,
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
    // The Miyoo framebuffer has two pages. A previous FFplay audio screen is
    // drawn in panel-native orientation and can otherwise resurface inverted
    // on a later page change. Prime both pages with the first complete kidui
    // frame instead of flipping FFplay's surface while it is being destroyed.
    // Two identical kidui frames are safe and visually indistinguishable.
    static bool framebuffer_primed = false;
    if (!framebuffer_primed) {
        SDL_BlitSurface(screen, NULL, video, NULL);
        SDL_Flip(video);
        framebuffer_primed = true;
    }
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

static SDL_Surface *copyScreenSurface(void)
{
    SDL_Surface *copy = SDL_CreateRGBSurface(
        SDL_SWSURFACE, g_display.width, g_display.height, 32,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (copy != NULL)
        SDL_BlitSurface(screen, NULL, copy, NULL);
    return copy;
}

static void blitMovingRegion(SDL_Surface *frame, SDL_Rect region, int offset)
{
    if (frame == NULL)
        return;
    int src_y = region.y;
    int dst_y = region.y + offset;
    int height = region.h;
    int bottom = region.y + region.h;
    if (dst_y < region.y) {
        int cut = region.y - dst_y;
        src_y += cut;
        height -= cut;
        dst_y = region.y;
    }
    if (dst_y + height > bottom)
        height = bottom - dst_y;
    if (height <= 0)
        return;
    SDL_Rect src = {region.x, src_y, region.w, height};
    SDL_Rect dst = {region.x, dst_y, region.w, height};
    SDL_BlitSurface(frame, &src, screen, &dst);
}

static bool switchFloor(ContentFloor target, int remaining)
{
    if (target == current_floor)
        return games_count > 0;
    if (current_floor == FLOOR_VIDEOS)
        rememberCurrentVideoFolder();
    rememberSelection();
    int direction = target == FLOOR_VIDEOS ? 1 : -1;

    // Render each floor only once. The animation then moves cached pixels,
    // avoiding font, image and theme work on every frame (far smoother on
    // the Miyoo's small CPU).
    SDL_Surface *old_frame = NULL;
    if (games_count > 0) {
        content_offset_y = 0;
        renderCarousel(remaining);
        old_frame = copyScreenSurface();
    }
    current_floor = target;
    writeFloorState();
    writeFolderState();
    loadCurrentFloor();
    rememberSelection();
    writeSelectionState();
    if (games_count == 0) {
        content_offset_y = 0;
        if (old_frame != NULL)
            SDL_FreeSurface(old_frame);
        dirty = true;
        return false;
    }

    content_offset_y = 0;
    renderCarousel(remaining);
    SDL_Surface *new_frame = copyScreenSurface();

    // A third cached frame contains only fixed chrome: moving the content
    // far outside the viewport leaves arrows, timer and footer in place.
    content_offset_y = g_display.height * 2;
    renderCarousel(remaining);
    SDL_Surface *chrome_frame = copyScreenSurface();
    content_offset_y = 0;

    if (new_frame == NULL || chrome_frame == NULL) {
        if (old_frame != NULL)
            SDL_FreeSurface(old_frame);
        if (new_frame != NULL)
            SDL_FreeSurface(new_frame);
        if (chrome_frame != NULL)
            SDL_FreeSurface(chrome_frame);
        dirty = true;
        return true;
    }

    SDL_Rect region = {(int)(45.0 * g_scale), (int)(55.0 * g_scale),
                       g_display.width - (int)(90.0 * g_scale),
                       (int)(365.0 * g_scale)};
    if (region.y + region.h > g_display.height)
        region.h = g_display.height - region.y;
    // Eight short steps stay visibly smooth while avoiding the sluggish
    // pause of the original longer transition on Miyoo hardware.
    const int frames = 8;
    for (int i = 1; i <= frames; i++) {
        int progress = region.h * i / frames;
        SDL_BlitSurface(chrome_frame, NULL, screen, NULL);
        blitMovingRegion(old_frame, region, direction * progress);
        blitMovingRegion(new_frame, region,
                         direction * (progress - region.h));
        flip();
        msleep(2);
    }
    if (old_frame != NULL)
        SDL_FreeSurface(old_frame);
    SDL_FreeSurface(new_frame);
    SDL_FreeSurface(chrome_frame);
    dirty = true;
    return true;
}

int main(int argc, char *argv[])
{
    loadRuntimePaths();
    bool set_pin_mode = false;
    bool menu_mode = false;
    bool pick_timer_mode = false;
    bool picker_no_off = false;
    bool start_on_pin = false;
    int menu_timer_minutes = 0;
    int menu_remaining = -1;
    int menu_lock_floor = 0;
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
        else if (strcmp(argv[i], "--lock-floor") == 0 && i + 1 < argc)
            menu_lock_floor = atoi(argv[++i]);
        else if (strcmp(argv[i], "--show-stories") == 0 && i + 1 < argc)
            show_stories = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--show-movies") == 0 && i + 1 < argc)
            show_movies = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--show-series") == 0 && i + 1 < argc)
            show_series = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--show-music") == 0 && i + 1 < argc)
            show_music = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--show-cartoons") == 0 && i + 1 < argc)
            show_cartoons = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--floor-locked") == 0)
            floor_locked = true;
        else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc)
            strncpy(select_rompath, argv[++i], STR_MAX - 1);
        else if (strcmp(argv[i], "--game-select") == 0 && i + 1 < argc)
            strncpy(game_select_path, argv[++i], STR_MAX - 1);
        else if (strcmp(argv[i], "--video-select") == 0 && i + 1 < argc)
            strncpy(video_select_path, argv[++i], STR_MAX - 1);
        else if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc)
            strncpy(current_folder, argv[++i], STR_MAX - 1);
        else if (strcmp(argv[i], "--floor") == 0 && i + 1 < argc)
            current_floor = strcasecmp(argv[++i], "VIDEOS") == 0
                                ? FLOOR_VIDEOS
                                : FLOOR_GAMES;
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
    writeFloorState();

    Screen active_screen = SCREEN_CAROUSEL;
    int remaining = -1;

    // Parent menu list (native Onion list component)
    List menu_list = list_create(6, LIST_SMALL);
    list_addItem(&menu_list,
                 (ListItem){.label = "Exit Kids Mode",
                            .item_type = ACTION});
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
    // Use separate fixed strings rather than a conditional label inside the
    // initializer: this is safer with Onion's copied ListItem representation
    // and the shorter wording leaves ample room for the toggle on every theme.
    if (current_floor == FLOOR_VIDEOS)
        list_addItem(&menu_list,
                     (ListItem){.label = "Videos only",
                                .item_type = TOGGLE,
                                .value = menu_lock_floor ? 1 : 0,
                                .action = onLockFloorToggle});
    else
        list_addItem(&menu_list,
                     (ListItem){.label = "Games only",
                                .item_type = TOGGLE,
                                .value = menu_lock_floor ? 1 : 0,
                                .action = onLockFloorToggle});
    list_addItem(&menu_list,
                 (ListItem){.label = "Media folders",
                            .item_type = ACTION});
    list_addItem(&menu_list,
                 (ListItem){.label = "Back", .item_type = ACTION});

    List category_list = list_create(6, LIST_SMALL);
    list_addItem(&category_list,
                 (ListItem){.label = "Movies",
                            .item_type = TOGGLE,
                            .value = show_movies ? 1 : 0,
                            .action = onMoviesToggle});
    list_addItem(&category_list,
                 (ListItem){.label = "Music",
                            .item_type = TOGGLE,
                            .value = show_music ? 1 : 0,
                            .action = onMusicToggle});
    list_addItem(&category_list,
                 (ListItem){.label = "Cartoons",
                            .item_type = TOGGLE,
                            .value = show_cartoons ? 1 : 0,
                            .action = onCartoonsToggle});
    list_addItem(&category_list,
                 (ListItem){.label = "Series",
                            .item_type = TOGGLE,
                            .value = show_series ? 1 : 0,
                            .action = onSeriesToggle});
    list_addItem(&category_list,
                 (ListItem){.label = "Stories",
                            .item_type = TOGGLE,
                            .value = show_stories ? 1 : 0,
                            .action = onStoriesToggle});
    list_addItem(&category_list,
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
        loadPersistedFolderSelections();
        if (select_rompath[0] != '\0')
            snprintf(current_floor == FLOOR_GAMES ? game_select_path
                                                  : video_select_path,
                     STR_MAX, "%s", select_rompath);
        if (current_floor == FLOOR_VIDEOS && video_select_path[0] != '\0')
            rememberFolderInMemory(folderBrowsePath(), video_select_path,
                                   false);
        loadCurrentFloor();
        fprintf(stderr, "kidui: loaded %d entries on %s floor\n",
                games_count,
                current_floor == FLOOR_VIDEOS ? "VIDEOS" : "GAMES");
        rememberSelection();
        writeSelectionState();
        writeFolderState();
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

        bool key_changed = updateKeystate(keystate, &quit, true, &changed_key);

        // Make the visual transition to Onion sleep immediate and uniform.
        // Only the PWM is changed here: no clear, render or framebuffer flip.
        // The POWER event itself continues untouched to Onion's keymon.
        if (key_changed && active_screen == SCREEN_CAROUSEL &&
            changed_key == SW_BTN_POWER &&
            keystate[changed_key] == PRESSED &&
            carousel_backlight_stage == 0) {
            long current = display_getBrightnessRaw();
            if (carousel_saved_brightness <= 0 && current > 0)
                carousel_saved_brightness = current;
            display_setBrightnessRaw(0);
            carousel_backlight_stage = 2;
            carousel_last_activity = ticks;
        }

        // When the carousel is dimmed or black, the first ordinary button
        // only restores the backlight. POWER is never consumed here: Onion
        // keeps its normal short-press sleep and wake behaviour.
        if (key_changed && active_screen == SCREEN_CAROUSEL &&
            carousel_backlight_stage != 0 &&
            keystate[changed_key] == PRESSED &&
            changed_key != SW_BTN_POWER) {
            restoreCarouselBacklight();
            carousel_last_activity = ticks;
            keystate[changed_key] = RELEASED;
            key_changed = false;
        }
        if (key_changed && changed_key == SW_BTN_Y)
            dirty = true;
        if (key_changed && keystate[changed_key] == PRESSED) {
            pin_last_input = ticks;
            if (active_screen == SCREEN_CAROUSEL)
                carousel_last_activity = ticks;

            if (active_screen == SCREEN_CAROUSEL && games_count > 0) {
                switch (changed_key) {
                case SW_BTN_RIGHT:
                    current = (current + 1) % games_count;
                    rememberSelection();
                    dirty = true;
                    break;
                case SW_BTN_LEFT:
                    current = (current + games_count - 1) % games_count;
                    rememberSelection();
                    dirty = true;
                    break;
                case SW_BTN_UP:
                    if (!floor_locked && current_floor == FLOOR_GAMES)
                        active_screen = switchFloor(FLOOR_VIDEOS, remaining)
                                            ? SCREEN_CAROUSEL
                                            : SCREEN_EMPTY;
                    break;
                case SW_BTN_DOWN:
                    if (!floor_locked && current_floor == FLOOR_VIDEOS)
                        active_screen = switchFloor(FLOOR_GAMES, remaining)
                                            ? SCREEN_CAROUSEL
                                            : SCREEN_EMPTY;
                    break;
                case SW_BTN_A:
                    if (current_floor == FLOOR_GAMES)
                        writeResult("LAUNCH", games[current].item.launch,
                                    games[current].item.rompath);
                    else if (games[current].is_folder) {
                        enterCurrentVideoFolder();
                        break;
                    }
                    else
                        writeResult("PLAY", games[current].item.rompath,
                                    games[current].item.imgpath);
                    exit_code = 0;
                    quit = true;
                    break;
                case SW_BTN_X:
                    if (!games[current].is_folder) {
                        active_screen = SCREEN_CONFIRM_RESTART;
                        dirty = true;
                    }
                    break;
                case SW_BTN_B:
                    if (current_floor == FLOOR_VIDEOS && current_folder[0])
                        leaveCurrentVideoFolder();
                    break;
                case SW_BTN_MENU:
                    // Carousel: MENU is intentionally ignored. Parent exit
                    // is only SELECT+START -> PIN -> parent menu.
                    break;
                default:
                    // Everything else is a no-op: no dead-ends for the kid
                    break;
                }
            }
            else if (active_screen == SCREEN_EMPTY) {
                if (changed_key == SW_BTN_B && current_floor == FLOOR_VIDEOS &&
                    current_folder[0]) {
                    leaveCurrentVideoFolder();
                }
                else if (!floor_locked && changed_key == SW_BTN_UP &&
                    current_floor == FLOOR_GAMES)
                    active_screen = switchFloor(FLOOR_VIDEOS, remaining)
                                        ? SCREEN_CAROUSEL
                                        : SCREEN_EMPTY;
                else if (!floor_locked && changed_key == SW_BTN_DOWN &&
                         current_floor == FLOOR_VIDEOS)
                    active_screen = switchFloor(FLOOR_GAMES, remaining)
                                        ? SCREEN_CAROUSEL
                                        : SCREEN_EMPTY;
            }
            else if (active_screen == SCREEN_CONFIRM_RESTART) {
                switch (changed_key) {
                case SW_BTN_A:
                    if (current_floor == FLOOR_GAMES)
                        writeResult("LAUNCH_FRESH", games[current].item.launch,
                                    games[current].item.rompath);
                    else
                        writeResult("RESTART", games[current].item.rompath,
                                    games[current].item.imgpath);
                    exit_code = 0;
                    quit = true;
                    break;
                case SW_BTN_B:
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
                    else if (menu_list.active_pos == MENU_LOCKFLOOR) {
                        list_activateItem(&menu_list);
                        dirty = true;
                    }
                    else if (menu_list.active_pos == MENU_CATEGORIES) {
                        active_screen = SCREEN_CATEGORIES;
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
            else if (active_screen == SCREEN_CATEGORIES) {
                switch (changed_key) {
                case SW_BTN_UP:
                    list_keyUp(&category_list, false);
                    dirty = true;
                    break;
                case SW_BTN_DOWN:
                    list_keyDown(&category_list, false);
                    dirty = true;
                    break;
                case SW_BTN_LEFT:
                    if (list_keyLeft(&category_list, false))
                        dirty = true;
                    break;
                case SW_BTN_RIGHT:
                    if (list_keyRight(&category_list, false))
                        dirty = true;
                    break;
                case SW_BTN_A:
                case SW_BTN_START:
                    if (category_list.active_pos == CATEGORY_BACK) {
                        active_screen = SCREEN_MENU;
                        dirty = true;
                    }
                    else {
                        list_activateItem(&category_list);
                        dirty = true;
                    }
                    break;
                case SW_BTN_B:
                    active_screen = SCREEN_MENU;
                    dirty = true;
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

        if (selection_state_dirty &&
            ticks - selection_changed_at >= SELECTION_WRITE_DELAY_MS)
            writeSelectionState();

        updateCarouselDimmer(ticks, active_screen == SCREEN_CAROUSEL);

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
            case SCREEN_CATEGORIES:
                renderCategories(&category_list);
                break;
            case SCREEN_PICKTIMER:
                renderPickTimer(pin_title, menu_timer_minutes, picker_no_off);
                break;
            case SCREEN_CONFIRM_RESTART:
                renderConfirmRestart(games[current].item.label, remaining);
                break;
            }
            if (hold_started != 0)
                renderHoldBar(ticks - hold_started);
            flip();
            dirty = false;
        }
        msleep(10);
    }

    stopCarouselDimmer();
    if (selection_state_dirty)
        writeSelectionState();
    artwork = NULL;
    for (int i = 0; i < 2; i++) {
        if (artwork_cache[i] != NULL)
            SDL_FreeSurface(artwork_cache[i]);
    }
    for (int i = 0; i < VIDEO_ARTWORK_CACHE_SIZE; i++)
        if (video_artwork_cache[i].surface != NULL)
            SDL_FreeSurface(video_artwork_cache[i].surface);
    for (int i = 0; i < artwork_dir_cache_count; i++)
        free(artwork_dir_cache[i].entries);
    for (int i = 0; i < VIDEO_LIST_CACHE_SIZE; i++)
        free(video_list_cache[i].entries);
    if (crt_fallback != NULL)
        SDL_FreeSurface(crt_fallback);
    if (screen_reflection != NULL)
        SDL_FreeSurface(screen_reflection);
    if (icon_x != NULL)
        SDL_FreeSurface(icon_x);
    if (arrow_up != NULL)
        SDL_FreeSurface(arrow_up);
    if (arrow_down != NULL)
        SDL_FreeSurface(arrow_down);
    if (font_gamelabel != NULL)
        TTF_CloseFont(font_gamelabel);
    for (int size = EPISODE_MIN_FONT_SIZE; size < GAME_LABEL_FONT_SIZE;
         size++) {
        if (font_episode_sizes[size] != NULL)
            TTF_CloseFont(font_episode_sizes[size]);
    }
    if (font_bigvalue != NULL)
        TTF_CloseFont(font_bigvalue);
    if (font_restart_title != NULL)
        TTF_CloseFont(font_restart_title);
    if (font_info != NULL)
        TTF_CloseFont(font_info);
    list_free(&menu_list);
    list_free(&category_list);
    resources_free();

    // NB: deliberately no final clear+flip here — an extra page flip on the
    // device can leave the visible framebuffer page out of sync with the
    // next process (MainUI painting an invisible page after unlock).
    TTF_Quit();
    SDL_Quit();

    return exit_code;
}
