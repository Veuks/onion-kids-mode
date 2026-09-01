/* Kids Mode controls and OSD for the SDL2 Miyoo player.
 *
 * This code is compiled directly into KidsPlay.  It deliberately avoids an
 * LD_PRELOAD input shim: playback, position tracking and drawing all use the
 * same FFplay clock and the same SDL2 texture that is presented with vsync.
 */

typedef struct _TTF_Font TTF_Font;
extern int TTF_Init(void);
extern void TTF_Quit(void);
extern TTF_Font *TTF_OpenFont(const char *, int);
extern void TTF_CloseFont(TTF_Font *);
extern void TTF_SetFontOutline(TTF_Font *, int);
extern SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *, const char *, SDL_Color);
extern int TTF_SizeUTF8(TTF_Font *, const char *, int *, int *);
extern SDL_Surface *IMG_Load(const char *);

#define KP_W 640
#define KP_H 480
#define KP_DIM_DELAY 30000
#define KP_OFF_DELAY 45000
#define KP_DIM_RAW 3
#define KP_TIMER_FILE "/tmp/kidsmode_remaining"
#define KP_MENU_MARKER "/tmp/kidsmode_video_menu_exit"
#define KP_POWER_SCREENSHOT "/tmp/kidsmode_power_screenshot"
#define KP_MEDIA_DIMMED "/tmp/kidsmode_media_dimmed"
#define KP_MEDIA_PLAYING "/tmp/kidsmode_media_playing"

typedef struct KidsPlayUi {
    int initialized;
    int audio_only;
    int menu_down;
    int menu_used;
    int x_down;
    int y_down;
    int screenshot_latched;
    int battery_down;
    int paused;
    int seek_key;
    int seek_forward;
    int seek_vertical;
    uint32_t menu_at;
    uint32_t seek_at;
    uint32_t seek_last;
    uint32_t progress_until;
    uint32_t seek_notice_until;
    uint32_t last_save;
    uint32_t last_status;
    uint32_t last_activity;
    uint32_t last_backlight_check;
    int backlight_stage;
    int external_backlight_off;
    long saved_brightness;
    long menu_brightness;
    long duration;
    long position;
    long timer_remaining;
    int battery;
    int charging;
    int battery_style_fixed;
    int battery_text_align;
    int battery_offset_x;
    int battery_offset_y;
    int clean_ready;
    int clean_pitch;
    uint8_t *clean_pixels;
    size_t clean_capacity;
    SDL_Texture *audio_texture;
    SDL_Surface *audio_canvas;
    SDL_Surface *audio_base;
    SDL_Surface *artwork;
    SDL_Surface *battery_icons[6];
    SDL_Surface *text_elapsed;
    SDL_Surface *text_remaining;
    SDL_Surface *text_battery;
    SDL_Surface *text_notice;
    SDL_Surface *text_timer;
    SDL_Surface *text_title;
    TTF_Font *font_small;
    TTF_Font *font_title;
    TTF_Font *font_timer;
    char notice[16];
    char position_file[512];
    char checkpoint_file[512];
    char screenshot_file[512];
    char artwork_file[512];
    char media_title[512];
    char brightness_file[512];
    char brightness_state_file[512];
    char theme_path[512];
    char font_path[512];
    char timer_font_path[512];
    char cached_elapsed[32];
    char cached_remaining[32];
    char cached_battery[16];
    char cached_notice[16];
    char cached_timer[32];
    char cached_title[512];
} KidsPlayUi;

static KidsPlayUi kp;

static void kp_copy_env(char *dst, size_t size, const char *name)
{
    const char *value = getenv(name);
    if (!value)
        value = "";
    av_strlcpy(dst, value, size);
}

static long kp_read_number(const char *path)
{
    char line[80];
    char *end = NULL;
    FILE *fp;
    long value;
    if (!path || !path[0] || !(fp = fopen(path, "r")))
        return -1;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    value = strtol(line, &end, 10);
    return end == line ? -1 : value;
}

static void kp_write_number(const char *path, long value)
{
    FILE *fp;
    if (!path || !path[0] || !(fp = fopen(path, "w")))
        return;
    fprintf(fp, "%ld\n", FFMAX(value, 0));
    fclose(fp);
}

static void kp_touch(const char *path, int present)
{
    if (!present) {
        unlink(path);
        return;
    }
    FILE *fp = fopen(path, "w");
    if (fp) {
        fputs("1\n", fp);
        fclose(fp);
    }
}

static int kp_write_brightness(long value)
{
    FILE *fp;
    if (!kp.brightness_file[0] || !(fp = fopen(kp.brightness_file, "w")))
        return 0;
    fprintf(fp, "%ld\n", value);
    fclose(fp);
    return 1;
}

static void kp_save_brightness(long value)
{
    if (value <= 0)
        return;
    kp.saved_brightness = value;
    kp_write_number(kp.brightness_state_file, value);
}

static void kp_restore_backlight(void)
{
    long value = kp.saved_brightness > 0 ? kp.saved_brightness : 64;
    kp_write_brightness(value);
    kp.backlight_stage = 0;
    kp.external_backlight_off = 0;
    kp_touch(KP_MEDIA_DIMMED, 0);
}

static int kp_battery_percentage(void)
{
    long value = kp_read_number("/tmp/percBat");
    if (value >= 0 && value <= 100)
        return value;
    FILE *fp = fopen("/tmp/.axp_result", "r");
    if (fp) {
        char buf[192] = "";
        if (fgets(buf, sizeof(buf), fp)) {
            char *field = strstr(buf, "\"battery\"");
            char *colon = field ? strchr(field, ':') : NULL;
            if (colon) {
                value = strtol(colon + 1, NULL, 10);
                if (value >= 0 && value <= 100) {
                    fclose(fp);
                    return value;
                }
            }
        }
        fclose(fp);
    }
    return kp.battery >= 0 ? kp.battery : 0;
}

static int kp_battery_charging(void)
{
    char buf[192] = "";
    if (kp_read_number("/tmp/percBat") == 500)
        return 1;
    FILE *fp = fopen("/tmp/.axp_result", "r");
    if (!fp)
        return 0;
    if (fgets(buf, sizeof(buf), fp)) {
        char *field = strstr(buf, "\"charging\"");
        char *colon = field ? strchr(field, ':') : NULL;
        if ((colon && atoi(colon + 1)) || strstr(buf, "\"battery\":500")) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static void kp_update_status(VideoState *is, int force)
{
    uint32_t now = SDL_GetTicks();
    if (!force && now - kp.last_status < 1000)
        return;
    kp.last_status = now;
    kp.battery = kp_battery_percentage();
    kp.charging = kp_battery_charging();
    kp.timer_remaining = kp_read_number(KP_TIMER_FILE);
    if (is && is->ic && is->ic->duration > 0)
        kp.duration = is->ic->duration / AV_TIME_BASE;
}

static void kp_init(VideoState *is)
{
    const char *kind;
    const char *start;
    const char *fixed;
    const char *align;
    int size, timer_size;
    if (kp.initialized)
        return;
    memset(&kp, 0, sizeof(kp));
    kp.battery = -1;
    kp.saved_brightness = -1;
    kp.menu_brightness = -1;
    kp_copy_env(kp.position_file, sizeof(kp.position_file), "VC_POSITION_FILE");
    kp_copy_env(kp.checkpoint_file, sizeof(kp.checkpoint_file), "VC_CHECKPOINT_FILE");
    kp_copy_env(kp.screenshot_file, sizeof(kp.screenshot_file), "VC_SCREENSHOT_FILE");
    kp_copy_env(kp.artwork_file, sizeof(kp.artwork_file), "VC_ARTWORK_FILE");
    kp_copy_env(kp.media_title, sizeof(kp.media_title), "VC_MEDIA_TITLE");
    kp_copy_env(kp.brightness_file, sizeof(kp.brightness_file), "VC_BRIGHTNESS_FILE");
    kp_copy_env(kp.brightness_state_file, sizeof(kp.brightness_state_file), "VC_BRIGHTNESS_STATE_FILE");
    kp_copy_env(kp.theme_path, sizeof(kp.theme_path), "VC_THEME_PATH");
    kp_copy_env(kp.font_path, sizeof(kp.font_path), "VC_OSD_FONT");
    kp_copy_env(kp.timer_font_path, sizeof(kp.timer_font_path), "VC_TIMER_FONT");
    fixed = getenv("VC_BATTERY_FIXED");
    align = getenv("VC_BATTERY_TEXT_ALIGN");
    kp.battery_style_fixed = fixed && !strcmp(fixed, "true");
    kp.battery_text_align = align && !av_strcasecmp(align, "right") ? 1 :
                            (align && !av_strcasecmp(align, "center") ? 0 : -1);
    kp.battery_offset_x = getenv("VC_BATTERY_OFFSET_X") ?
                              atoi(getenv("VC_BATTERY_OFFSET_X")) : 0;
    kp.battery_offset_y = getenv("VC_BATTERY_OFFSET_Y") ?
                              atoi(getenv("VC_BATTERY_OFFSET_Y")) : 0;
    kind = getenv("VC_MEDIA_KIND");
    kp.audio_only = kind && !strcmp(kind, "audio");
    start = getenv("VC_START_SECONDS");
    kp.position = start ? strtol(start, NULL, 10) : 0;
    kp.saved_brightness = kp_read_number(kp.brightness_state_file);
    if (kp.saved_brightness <= 0)
        kp.saved_brightness = kp_read_number(kp.brightness_file);
    size = getenv("VC_OSD_FONT_SIZE") ? atoi(getenv("VC_OSD_FONT_SIZE")) : 24;
    if (size < 12 || size > 48)
        size = 24;
    timer_size = getenv("VC_TIMER_FONT_SIZE") ?
                     atoi(getenv("VC_TIMER_FONT_SIZE")) : size;
    if (timer_size < 12 || timer_size > 48)
        timer_size = size;
    if (TTF_Init() == 0 && kp.font_path[0]) {
        kp.font_small = TTF_OpenFont(kp.font_path, size);
        kp.font_title = TTF_OpenFont(kp.font_path, FFMIN(size + 4, 48));
        if (kp.timer_font_path[0])
            kp.font_timer = TTF_OpenFont(kp.timer_font_path, timer_size);
        if (!kp.font_timer)
            kp.font_timer = TTF_OpenFont(kp.font_path, timer_size);
    }
    kp.last_activity = kp.last_save = SDL_GetTicks();
    kp_touch(KP_MEDIA_PLAYING, 1);
    kp_update_status(is, 1);
    kp.initialized = 1;
}

static void kp_save_position(VideoState *is, int checkpoint)
{
    double clock = is ? get_master_clock(is) : NAN;
    if (!isnan(clock) && clock >= 0)
        kp.position = (long)clock;
    kp_write_number(kp.position_file, kp.position);
    if (checkpoint)
        kp_write_number(kp.checkpoint_file, kp.position - 2);
}

static void kp_update_backlight(VideoState *is)
{
    uint32_t now = SDL_GetTicks();
    long current;
    if ((!kp.audio_only && !is->paused) || kp.saved_brightness <= 0)
        return;
    if (now - kp.last_backlight_check >= 250) {
        kp.last_backlight_check = now;
        current = kp_read_number(kp.brightness_file);
        if (kp.external_backlight_off) {
            if (current > 0) {
                kp.external_backlight_off = 0;
                kp.backlight_stage = 0;
                kp_save_brightness(current);
                kp.last_activity = now;
                kp_touch(KP_MEDIA_DIMMED, 0);
            } else {
                kp.last_activity = now;
            }
            return;
        }
        if (kp.backlight_stage == 0 && current == 0) {
            kp.external_backlight_off = 1;
            kp.last_activity = now;
            return;
        }
        if (kp.backlight_stage == 1 && current > KP_DIM_RAW) {
            kp.backlight_stage = 0;
            kp_save_brightness(current);
            kp.last_activity = now;
            kp_touch(KP_MEDIA_DIMMED, 0);
        } else if (kp.backlight_stage == 2 && current > 0) {
            kp.backlight_stage = 0;
            kp_save_brightness(current);
            kp.last_activity = now;
            kp_touch(KP_MEDIA_DIMMED, 0);
        }
    }
    if (kp.backlight_stage == 0 && now - kp.last_activity >= KP_DIM_DELAY) {
        current = kp_read_number(kp.brightness_file);
        if (current > 0)
            kp_save_brightness(current);
        if (kp_write_brightness(KP_DIM_RAW)) {
            kp.backlight_stage = 1;
            kp_touch(KP_MEDIA_DIMMED, 1);
        }
    }
    if (kp.backlight_stage == 1 && now - kp.last_activity >= KP_OFF_DELAY) {
        if (kp_write_brightness(0)) {
            kp.backlight_stage = 2;
            kp_touch(KP_MEDIA_DIMMED, 1);
        }
    }
}

static void kp_tick(VideoState *is)
{
    uint32_t now;
    kp_init(is);
    now = SDL_GetTicks();
    kp.paused = is->paused;
    kp_update_status(is, 0);
    kp_update_backlight(is);
    if (now - kp.last_save >= 1000) {
        kp_save_position(is, (now / 5000) != (kp.last_save / 5000));
        kp.last_save = now;
    }
}

static SDL_Surface *kp_text(TTF_Font *font, const char *text, SDL_Color color)
{
    SDL_Color black = {0, 0, 0, 255};
    SDL_Surface *shadow, *face, *result;
    SDL_Rect destination;
    int outline = 2;
    if (!font || !text || !text[0])
        return NULL;
    TTF_SetFontOutline(font, outline);
    shadow = TTF_RenderUTF8_Blended(font, text, black);
    TTF_SetFontOutline(font, 0);
    face = TTF_RenderUTF8_Blended(font, text, color);
    if (!shadow || !face) {
        if (shadow) SDL_FreeSurface(shadow);
        if (face) SDL_FreeSurface(face);
        return NULL;
    }
    result = SDL_CreateRGBSurfaceWithFormat(0, shadow->w, shadow->h, 32,
                                            SDL_PIXELFORMAT_ARGB8888);
    if (result) {
        SDL_FillRect(result, NULL, SDL_MapRGBA(result->format, 0, 0, 0, 0));
        SDL_SetSurfaceBlendMode(shadow, SDL_BLENDMODE_BLEND);
        SDL_SetSurfaceBlendMode(face, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(shadow, NULL, result, NULL);
        destination.x = outline;
        destination.y = outline;
        destination.w = face->w;
        destination.h = face->h;
        SDL_BlitSurface(face, NULL, result, &destination);
        SDL_SetSurfaceBlendMode(result, SDL_BLENDMODE_BLEND);
    }
    SDL_FreeSurface(shadow);
    SDL_FreeSurface(face);
    return result;
}

static void kp_blit(SDL_Surface *target, SDL_Surface *source, int x, int y)
{
    SDL_Rect rect;
    if (!target || !source)
        return;
    rect.x = x;
    rect.y = y;
    rect.w = source->w;
    rect.h = source->h;
    SDL_BlitSurface(source, NULL, target, &rect);
}

static SDL_Surface *kp_cached_text(SDL_Surface **cache, char *cached,
                                   size_t cached_size, TTF_Font *font,
                                   const char *text, SDL_Color color)
{
    if (*cache && !strcmp(cached, text))
        return *cache;
    if (*cache) {
        SDL_FreeSurface(*cache);
        *cache = NULL;
    }
    av_strlcpy(cached, text, cached_size);
    *cache = kp_text(font, text, color);
    return *cache;
}

static void kp_format_time(long seconds, int remaining, char *out, size_t size)
{
    long hours, minutes;
    seconds = FFMAX(seconds, 0);
    hours = seconds / 3600;
    minutes = (seconds % 3600) / 60;
    seconds %= 60;
    if (hours > 0)
        snprintf(out, size, "%s%ld:%02ld:%02ld", remaining ? "-" : "", hours, minutes, seconds);
    else
        snprintf(out, size, "%s%ld:%02ld", remaining ? "-" : "", minutes, seconds);
}

static void kp_draw_progress(SDL_Surface *surface)
{
    SDL_Color white = {255, 255, 255, 255};
    SDL_Rect bar, inner;
    char elapsed[32], remain[32];
    long duration = FFMAX(kp.duration, 1);
    long position = av_clip64(kp.position, 0, duration);
    kp_format_time(position, 0, elapsed, sizeof(elapsed));
    kp_format_time(duration - position, 1, remain, sizeof(remain));
    {
        SDL_Surface *elapsed_label = kp_cached_text(
            &kp.text_elapsed, kp.cached_elapsed, sizeof(kp.cached_elapsed),
            kp.font_small, elapsed, white);
        SDL_Surface *remain_label = kp_cached_text(
            &kp.text_remaining, kp.cached_remaining,
            sizeof(kp.cached_remaining), kp.font_small, remain, white);
        int elapsed_w = 0, elapsed_h = 0, remain_w = 0, remain_h = 0;
        int bar_y = 452;
        int knob_x, dx, dy;
        Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
        Uint32 accent = SDL_MapRGB(surface->format, 174, 72, 255);
        if (elapsed_label) {
            elapsed_w = elapsed_label->w;
            elapsed_h = elapsed_label->h;
        }
        if (remain_label) {
            remain_w = remain_label->w;
            remain_h = remain_label->h;
        }
        bar.x = 16 + elapsed_w + 20;
        bar.y = bar_y - 2;
        bar.w = KP_W - 16 - remain_w - 20 - bar.x;
        if (bar.w < KP_W / 4) {
            bar.x = 154;
            bar.w = 332;
        }
        bar.h = 5;
        SDL_FillRect(surface, &bar, black);
        inner.x = bar.x;
        inner.y = bar_y - 1;
        inner.w = bar.w;
        inner.h = 3;
        SDL_FillRect(surface, &inner, accent);
        knob_x = bar.x + (int)((int64_t)bar.w * position / duration);
        for (dy = -7; dy <= 7; dy++)
            for (dx = -7; dx <= 7; dx++)
                if (dx * dx + dy * dy <= 49) {
                    SDL_Rect pixel = {knob_x + dx, bar_y + dy, 1, 1};
                    SDL_FillRect(surface, &pixel, black);
                }
        for (dy = -5; dy <= 5; dy++)
            for (dx = -5; dx <= 5; dx++)
                if (dx * dx + dy * dy <= 25) {
                    SDL_Rect pixel = {knob_x + dx, bar_y + dy, 1, 1};
                    SDL_FillRect(surface, &pixel, accent);
                }
        kp_blit(surface, elapsed_label, 16, bar_y - elapsed_h / 2 - 6);
        kp_blit(surface, remain_label, 624 - remain_w,
                bar_y - remain_h / 2 - 6);
    }
}

static int kp_battery_index(void)
{
    if (kp.charging) return 5;
    if (kp.battery < 5) return 0;
    if (kp.battery < 30) return 1;
    if (kp.battery < 60) return 2;
    if (kp.battery < 90) return 3;
    return 4;
}

static SDL_Surface *kp_load_battery_icon(void)
{
    static const char *names[6] = {
        "power-0%-icon", "power-20%-icon", "power-50%-icon",
        "power-80%-icon", "power-full-icon", "ic-power-charge-100%"};
    int index = kp_battery_index();
    char path[1024];
    const char *roots[3];
    int i;
    if (kp.battery_icons[index])
        return kp.battery_icons[index];
    roots[0] = "/mnt/SDCARD/Saves/CurrentProfile/theme";
    roots[1] = kp.theme_path[0] ? kp.theme_path : "/mnt/SDCARD/miyoo/app";
    roots[2] = "/mnt/SDCARD/miyoo/app";
    for (i = 0; i < 3 && !kp.battery_icons[index]; i++) {
        snprintf(path, sizeof(path), "%s%s/skin/%s.png", roots[i],
                 roots[i][strlen(roots[i]) - 1] == '/' ? "" : "/", names[index]);
        kp.battery_icons[index] = IMG_Load(path);
    }
    if (!kp.battery_icons[index] && index == 4) {
        for (i = 0; i < 3 && !kp.battery_icons[index]; i++) {
            snprintf(path, sizeof(path), "%s%s/skin/power-full-icon_back.png",
                     roots[i], roots[i][strlen(roots[i]) - 1] == '/' ? "" : "/");
            kp.battery_icons[index] = IMG_Load(path);
        }
    }
    return kp.battery_icons[index];
}

static void kp_draw_battery(SDL_Surface *surface)
{
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *icon = kp_load_battery_icon();
    char text[16];
    int text_w = 0, text_h = 0;
    int icon_x, icon_y, label_x, label_y;
    const int header_center = 596;
    const int center_y = 30;
    snprintf(text, sizeof(text), "%d%%", kp.battery);
    {
        SDL_Surface *label = kp_cached_text(
            &kp.text_battery, kp.cached_battery, sizeof(kp.cached_battery),
            kp.font_small, text, white);
        if (label) {
            text_w = label->w;
            text_h = label->h;
        }
        icon_x = icon ? header_center - icon->w / 2 : header_center;
        icon_y = icon ? center_y - icon->h / 2 : center_y;
        label_x = icon_x;
        if (icon) {
            SDL_Rect dst = {icon_x, icon_y, icon->w, icon->h};
            SDL_SetSurfaceBlendMode(icon, SDL_BLENDMODE_BLEND);
            SDL_BlitSurface(icon, NULL, surface, &dst);
            if (kp.battery_style_fixed) {
                if (kp.battery_text_align > 0)
                    label_x = icon_x + icon->w - text_w + kp.battery_offset_x;
                else if (kp.battery_text_align == 0)
                    label_x = icon_x + (icon->w - text_w) / 2 + kp.battery_offset_x;
                else
                    label_x = icon_x + kp.battery_offset_x;
            } else {
                label_x = icon_x + icon->w + 5 + kp.battery_offset_x;
            }
        }
        label_y = center_y - text_h / 2 + kp.battery_offset_y;
        if (strstr(kp.font_path, "Exo-2"))
            label_y -= 4;
        kp_blit(surface, label, label_x, label_y);
    }
}

static int kp_overlay_visible(VideoState *is)
{
    uint32_t now = SDL_GetTicks();
    return kp.audio_only || is->paused || now < kp.progress_until ||
           now < kp.seek_notice_until || kp.battery_down || kp.charging ||
           (kp.timer_remaining > 0 && kp.timer_remaining <= 300);
}

static void kp_draw_overlay(SDL_Surface *surface, VideoState *is)
{
    SDL_Color white = {255, 255, 255, 255};
    SDL_Color red = {255, 64, 64, 255};
    uint32_t now = SDL_GetTicks();
    if (kp.audio_only || is->paused || now < kp.progress_until ||
        now < kp.seek_notice_until)
        kp_draw_progress(surface);
    if (kp.notice[0] && now < kp.seek_notice_until) {
        SDL_Surface *notice = kp_cached_text(
            &kp.text_notice, kp.cached_notice, sizeof(kp.cached_notice),
            kp.font_small, kp.notice, white);
        if (notice)
            kp_blit(surface, notice, kp.seek_forward ? 624 - notice->w : 16,
                    398);
    }
    if (kp.battery_down || kp.charging)
        kp_draw_battery(surface);
    else if (kp.timer_remaining > 0 && kp.timer_remaining <= 300) {
        char timer[32];
        snprintf(timer, sizeof(timer), "%ld min", (kp.timer_remaining + 59) / 60);
        SDL_Surface *label = kp_cached_text(
            &kp.text_timer, kp.cached_timer, sizeof(kp.cached_timer),
            kp.font_timer ? kp.font_timer : kp.font_small, timer, red);
        if (label)
            kp_blit(surface, label, 620 - label->w, 30 - label->h / 2);
    }
}

static void kp_capture_clean_texture(SDL_Texture *texture)
{
    void *pixels = NULL;
    int pitch = 0;
    size_t size;
    int y;
    if (!texture || SDL_LockTexture(texture, NULL, &pixels, &pitch) < 0)
        return;
    size = (size_t)KP_W * KP_H * 4;
    if (kp.clean_capacity < size) {
        uint8_t *grown = av_realloc(kp.clean_pixels, size);
        if (!grown) {
            SDL_UnlockTexture(texture);
            return;
        }
        kp.clean_pixels = grown;
        kp.clean_capacity = size;
    }
    for (y = 0; y < KP_H; y++)
        memcpy(kp.clean_pixels + y * KP_W * 4, (uint8_t *)pixels + y * pitch, KP_W * 4);
    kp.clean_pitch = KP_W * 4;
    kp.clean_ready = 1;
    SDL_UnlockTexture(texture);
}

static void kp_compose_video(VideoState *is, SDL_Texture *texture)
{
    void *pixels = NULL;
    int pitch = 0;
    int y, bpp = 0;
    Uint32 rmask, gmask, bmask, amask;
    SDL_Surface *surface;
    kp_tick(is);
    if (!kp_overlay_visible(is))
        return;
    if (!kp.clean_ready || !texture || SDL_LockTexture(texture, NULL, &pixels, &pitch) < 0)
        return;
    for (y = 0; y < KP_H; y++)
        memcpy((uint8_t *)pixels + y * pitch, kp.clean_pixels + y * kp.clean_pitch, KP_W * 4);
    if (kp_overlay_visible(is) && SDL_PixelFormatEnumToMasks(SDL_PIXELFORMAT_ARGB8888, &bpp, &rmask, &gmask, &bmask, &amask)) {
        surface = SDL_CreateRGBSurfaceFrom(pixels, KP_W, KP_H, bpp, pitch, rmask, gmask, bmask, amask);
        if (surface) {
            kp_draw_overlay(surface, is);
            SDL_FreeSurface(surface);
        }
    }
    SDL_UnlockTexture(texture);
}

static void kp_draw_scaled_artwork(SDL_Surface *target)
{
    SDL_Rect src, dst;
    int max_w = 300, max_h = 300;
    if (!kp.artwork && kp.artwork_file[0])
        kp.artwork = IMG_Load(kp.artwork_file);
    if (!kp.artwork)
        return;
    src.x = src.y = 0;
    src.w = kp.artwork->w;
    src.h = kp.artwork->h;
    dst.w = max_w;
    dst.h = (int)((int64_t)kp.artwork->h * max_w / FFMAX(kp.artwork->w, 1));
    if (dst.h > max_h) {
        dst.h = max_h;
        dst.w = (int)((int64_t)kp.artwork->w * max_h / FFMAX(kp.artwork->h, 1));
    }
    dst.x = (KP_W - dst.w) / 2;
    dst.y = 45 + (max_h - dst.h) / 2;
    SDL_BlitScaled(kp.artwork, &src, target, &dst);
}

static void kp_audio_display(VideoState *is)
{
    SDL_Color white = {255, 255, 255, 255};
    int title_w = 0;
    kp_tick(is);
    if (!kp.audio_canvas) {
        kp.audio_canvas = SDL_CreateRGBSurfaceWithFormat(0, KP_W, KP_H, 32, SDL_PIXELFORMAT_ARGB8888);
        kp.audio_base = SDL_CreateRGBSurfaceWithFormat(0, KP_W, KP_H, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
        kp.audio_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, KP_W, KP_H);
        if (kp.audio_base) {
            SDL_FillRect(kp.audio_base, NULL,
                         SDL_MapRGB(kp.audio_base->format, 0, 0, 0));
            kp_draw_scaled_artwork(kp.audio_base);
            {
                SDL_Surface *title = kp_cached_text(
                    &kp.text_title, kp.cached_title, sizeof(kp.cached_title),
                    kp.font_title, kp.media_title, white);
                if (title) {
                    title_w = title->w;
                    kp_blit(kp.audio_base, title,
                            FFMAX((KP_W - title_w) / 2, 8), 360);
                }
            }
        }
    }
    if (!kp.audio_canvas || !kp.audio_base || !kp.audio_texture)
        return;
    SDL_BlitSurface(kp.audio_base, NULL, kp.audio_canvas, NULL);
    kp_draw_overlay(kp.audio_canvas, is);
    SDL_UpdateTexture(kp.audio_texture, NULL, kp.audio_canvas->pixels, kp.audio_canvas->pitch);
    SDL_RenderCopy(renderer, kp.audio_texture, NULL, NULL);
}

static void kp_set_notice(int forward, long step)
{
    kp.seek_forward = forward;
    snprintf(kp.notice, sizeof(kp.notice), "%c%ld%s", forward ? '+' : '-',
             step >= 60 ? step / 60 : step, step >= 60 ? "m" : "s");
    kp.seek_notice_until = kp.progress_until = SDL_GetTicks() + 2000;
}

static void kp_seek(VideoState *is, long delta, long label)
{
    double pos = get_master_clock(is);
    if (isnan(pos))
        pos = (double)is->seek_pos / AV_TIME_BASE;
    pos = FFMAX(pos + delta, 0);
    if (kp.duration > 0)
        pos = FFMIN(pos, kp.duration);
    stream_seek(is, (int64_t)(pos * AV_TIME_BASE), delta * AV_TIME_BASE, 0);
    kp.position = pos;
    kp_write_number(kp.position_file, kp.position);
    kp_write_number(kp.checkpoint_file, kp.position - 2);
    kp_set_notice(delta >= 0, label);
}

static void kp_progressive_seek(VideoState *is)
{
    uint32_t now, held;
    long step;
    if (!kp.seek_key)
        return;
    now = SDL_GetTicks();
    if (kp.seek_last && now - kp.seek_last < 450)
        return;
    held = now - kp.seek_at;
    step = kp.seek_vertical ? (held < 1500 ? 300 : 600)
                            : (held < 1500 ? 10 : 60);
    kp_seek(is, kp.seek_forward ? step : -step, step);
    kp.seek_last = now;
}

static int kp_save_screenshot(void)
{
    int bpp = 0;
    Uint32 rmask, gmask, bmask, amask;
    SDL_Surface *surface;
    int result;
    if (!kp.clean_ready || !kp.screenshot_file[0] ||
        !SDL_PixelFormatEnumToMasks(SDL_PIXELFORMAT_ARGB8888, &bpp,
                                    &rmask, &gmask, &bmask, &amask))
        return 0;
    surface = SDL_CreateRGBSurfaceFrom(kp.clean_pixels, KP_W, KP_H, bpp,
                                       kp.clean_pitch, rmask, gmask, bmask, amask);
    if (!surface)
        return 0;
    result = SDL_SaveBMP(surface, kp.screenshot_file) == 0;
    SDL_FreeSurface(surface);
    return result;
}

/* Return 1 when Kids Mode consumed the event. */
static int kp_handle_key(VideoState *is, SDL_KeyboardEvent *key)
{
    SDL_Keycode sym = key->keysym.sym;
    int down = key->state == SDL_PRESSED;
    uint32_t now = SDL_GetTicks();
    kp_init(is);

    if (down)
        kp.last_activity = now;
    if ((kp.audio_only || is->paused) && kp.backlight_stage != 0) {
        if (down)
            kp_restore_backlight();
        return 1;
    }
    if (sym == SDLK_LSHIFT)
        kp.x_down = down;
    if (sym == SDLK_LALT)
        kp.y_down = down;
    if (sym == SDLK_ESCAPE) {
        if (down) {
            if (!kp.menu_down) {
                kp.menu_down = 1;
                kp.menu_used = 0;
                kp.menu_at = now;
                kp.menu_brightness = kp_read_number(kp.brightness_file);
            }
        } else {
            long brightness = kp_read_number(kp.brightness_file);
            kp.menu_down = 0;
            kp.seek_key = 0;
            // Onion's MENU + POWER screenshot is completed when POWER is
            // released, before MENU reaches this release event. Consume that
            // one MENU release so it never becomes a "leave video" action.
            if (access(KP_POWER_SCREENSHOT, F_OK) == 0) {
                unlink(KP_POWER_SCREENSHOT);
                return 1;
            }
            if (brightness > 0 && kp.menu_brightness >= 0 && brightness != kp.menu_brightness) {
                kp.menu_used = 1;
                kp_save_brightness(brightness);
            }
            if (!kp.menu_used && now - kp.menu_at < 500) {
                kp_save_position(is, 1);
                kp_touch(KP_MENU_MARKER, 1);
                do_exit(is);
            }
        }
        return 1;
    }
    if (kp.menu_down && (sym == SDLK_LEFT || sym == SDLK_RIGHT ||
                         sym == SDLK_UP || sym == SDLK_DOWN)) {
        kp.menu_used = 1;
        if (down) {
            if (kp.seek_key != sym) {
                kp.seek_key = sym;
                kp.seek_at = now;
                kp.seek_last = 0;
                kp.seek_forward = sym == SDLK_RIGHT || sym == SDLK_UP;
                kp.seek_vertical = sym == SDLK_UP || sym == SDLK_DOWN;
            }
            kp_progressive_seek(is);
        } else if (kp.seek_key == sym) {
            kp.seek_key = 0;
        }
        return 1;
    }
    if (sym == SDLK_LALT && !kp.menu_down) {
        kp.battery_down = down;
        kp_update_status(is, 1);
        is->force_refresh = 1;
        return 1;
    }
    if (kp.menu_down && (sym == SDLK_LSHIFT || sym == SDLK_LALT)) {
        kp.menu_used = 1;
        if (kp.x_down && kp.y_down && !kp.screenshot_latched) {
            kp.screenshot_latched = 1;
            if (is->paused)
                kp_save_screenshot();
        }
        if (!kp.x_down || !kp.y_down)
            kp.screenshot_latched = 0;
        return 1;
    }
    if (kp.menu_down) {
        if (down)
            kp.menu_used = 1;
        return 1;
    }
    if (sym == SDLK_SPACE) {
        if (down) {
            kp.notice[0] = 0;
            kp.seek_notice_until = 0;
            kp.progress_until = now + 2000;
            if (is->paused) {
                kp_restore_backlight();
                toggle_pause(is);
                kp_touch(KP_MEDIA_PLAYING, 1);
            }
            is->force_refresh = 1;
        }
        return 1;
    }
    if (sym == SDLK_LCTRL) {
        if (down && !is->paused) {
            toggle_pause(is);
            kp_touch(KP_MEDIA_PLAYING, 0);
            kp.progress_until = now + 2000;
            is->force_refresh = 1;
        }
        return 1;
    }
    return 1;
}

static void kp_shutdown(VideoState *is)
{
    int i;
    if (!kp.initialized)
        return;
    kp_save_position(is, 1);
    kp_touch(KP_MEDIA_PLAYING, 0);
    kp_touch(KP_MEDIA_DIMMED, 0);
    if (kp.audio_texture) SDL_DestroyTexture(kp.audio_texture);
    if (kp.audio_canvas) SDL_FreeSurface(kp.audio_canvas);
    if (kp.audio_base) SDL_FreeSurface(kp.audio_base);
    if (kp.artwork) SDL_FreeSurface(kp.artwork);
    if (kp.text_elapsed) SDL_FreeSurface(kp.text_elapsed);
    if (kp.text_remaining) SDL_FreeSurface(kp.text_remaining);
    if (kp.text_battery) SDL_FreeSurface(kp.text_battery);
    if (kp.text_notice) SDL_FreeSurface(kp.text_notice);
    if (kp.text_timer) SDL_FreeSurface(kp.text_timer);
    if (kp.text_title) SDL_FreeSurface(kp.text_title);
    for (i = 0; i < 6; i++)
        if (kp.battery_icons[i]) SDL_FreeSurface(kp.battery_icons[i]);
    if (kp.font_small) TTF_CloseFont(kp.font_small);
    if (kp.font_title) TTF_CloseFont(kp.font_title);
    if (kp.font_timer) TTF_CloseFont(kp.font_timer);
    TTF_Quit();
    av_freep(&kp.clean_pixels);
    kp.initialized = 0;
}
