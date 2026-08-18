#define _GNU_SOURCE
#include <SDL/SDL.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*poll_fn)(SDL_Event *);
typedef int (*wait_fn)(SDL_Event *);
typedef int (*peep_fn)(SDL_Event *, int, SDL_eventaction, Uint32);
typedef int (*overlay_fn)(SDL_Overlay *, SDL_Rect *);
static poll_fn real_poll;
static wait_fn real_wait;
static peep_fn real_peep;
static overlay_fn real_overlay;
static bool inside_event_call;
static bool paused;
static bool menu_down;
static bool menu_used;
static Uint32 menu_pressed_at;
static SDLKey seek_input = SDLK_UNKNOWN;
static Uint32 seek_started_at;
static Uint32 seek_last_step;
static bool clock_ready;
static Uint32 clock_tick;
static Uint32 last_save;
static long position_seconds;
static const char *position_file;
static const char *checkpoint_file;
static bool key_repeat_enabled;
static char seek_notice[8];
static bool seek_notice_forward;
static Uint32 seek_notice_until;
static bool seek_notice_drawn;
static SDL_Rect seek_notice_rect;

#define SYNTHETIC_SEEK_UNICODE 0x5643

__attribute__((constructor)) static void vcinput_loaded(void)
{
    FILE *fp = fopen("/tmp/vcinput_loaded", "w");
    if (fp) {
        fputs("1\n", fp);
        fclose(fp);
    }
}

static void write_position(const char *path)
{
    if (!path || !*path)
        return;
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%ld\n", position_seconds < 0 ? 0 : position_seconds);
        fclose(fp);
    }
}

static void save_position(void)
{
    write_position(position_file);
}

static void save_checkpoint(void)
{
    write_position(checkpoint_file);
}

static void mark_menu_exit(void)
{
    FILE *fp = fopen("/tmp/videocarousel_menu_exit", "w");
    if (fp) {
        fputs("1\n", fp);
        fclose(fp);
    }
}

static void update_clock(void)
{
    Uint32 now = SDL_GetTicks();
    if (!clock_ready) {
        const char *start = getenv("VC_START_SECONDS");
        position_seconds = start ? strtol(start, NULL, 10) : 0;
        position_file = getenv("VC_POSITION_FILE");
        checkpoint_file = getenv("VC_CHECKPOINT_FILE");
        clock_tick = last_save = now;
        clock_ready = true;
    }
    if (!paused && now - clock_tick >= 1000) {
        position_seconds += (now - clock_tick) / 1000;
        clock_tick += ((now - clock_tick) / 1000) * 1000;
    } else if (paused) {
        clock_tick = now;
    }
    if (now - last_save >= 1000) {
        save_position();
        if ((now / 5000) != (last_save / 5000))
            save_checkpoint();
        last_save = now;
    }
}

static void key(SDL_Event *event, SDLKey sym, Uint8 state)
{
    memset(event, 0, sizeof(*event));
    event->type = state == SDL_PRESSED ? SDL_KEYDOWN : SDL_KEYUP;
    event->key.type = event->type;
    event->key.state = state;
    event->key.keysym.sym = sym;
}

static void ensure_key_repeat(void)
{
    if (!key_repeat_enabled) {
        // SDL 1.2 does not repeat held keys unless the application enables
        // it. Onion's FFplay leaves it disabled on some builds.
        SDL_EnableKeyRepeat(300, 100);
        key_repeat_enabled = true;
    }
}

static const unsigned char *glyph_rows(char c)
{
    static const unsigned char plus[7] = {0, 4, 4, 31, 4, 4, 0};
    static const unsigned char minus[7] = {0, 0, 0, 31, 0, 0, 0};
    static const unsigned char zero[7] = {14, 17, 19, 21, 25, 17, 14};
    static const unsigned char one[7] = {4, 12, 4, 4, 4, 4, 14};
    static const unsigned char five[7] = {31, 16, 16, 30, 1, 1, 30};
    static const unsigned char ess[7] = {0, 0, 15, 16, 14, 1, 30};
    static const unsigned char em[7] = {0, 0, 26, 21, 21, 21, 21};
    switch (c) {
    case '+': return plus;
    case '-': return minus;
    case '0': return zero;
    case '1': return one;
    case '5': return five;
    case 's': return ess;
    case 'm': return em;
    default: return NULL;
    }
}

static void draw_glyph(SDL_Surface *surface, char c, int x, int y, int scale,
                       Uint32 black, Uint32 white)
{
    const unsigned char *rows = glyph_rows(c);
    if (!rows)
        return;
    for (int pass = 0; pass < 2; pass++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                // FFplay's Miyoo framebuffer is physically rotated 180°.
                // Draw every glyph pre-rotated so it appears upright.
                if (!(rows[6 - row] & (1 << col)))
                    continue;
                SDL_Rect rect = pass == 0
                                    ? (SDL_Rect){x + col * scale - 1,
                                                 y + row * scale - 1,
                                                 scale + 2, scale + 2}
                                    : (SDL_Rect){x + col * scale,
                                                 y + row * scale,
                                                 scale, scale};
                SDL_FillRect(surface, &rect, pass == 0 ? black : white);
            }
}

static void draw_seek_notice(void)
{
    SDL_Surface *surface = SDL_GetVideoSurface();
    if (!surface)
        return;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    bool cleared_notice = seek_notice_drawn;
    SDL_Rect cleared_rect = seek_notice_rect;
    if (seek_notice_drawn) {
        SDL_FillRect(surface, &seek_notice_rect, black);
        seek_notice_drawn = false;
    }
    if (!seek_notice[0] || SDL_GetTicks() >= seek_notice_until) {
        if (cleared_notice)
            SDL_UpdateRect(surface, seek_notice_rect.x, seek_notice_rect.y,
                           seek_notice_rect.w, seek_notice_rect.h);
        seek_notice[0] = '\0';
        return;
    }

    int scale = surface->w >= 600 ? 3 : 2;
    int length = (int)strlen(seek_notice);
    int width = length * 6 * scale - scale;
    // Coordinates are pre-rotated as well: logical top-left becomes the
    // physical bottom-right, and logical top-right becomes bottom-left.
    int x = seek_notice_forward ? 18 : surface->w - width - 18;
    int y = 20;
    SDL_Rect new_rect = {x - 2, y - 2, width + 4, 7 * scale + 4};
    if (cleared_notice &&
        (cleared_rect.x != new_rect.x || cleared_rect.y != new_rect.y ||
         cleared_rect.w != new_rect.w || cleared_rect.h != new_rect.h))
        SDL_UpdateRect(surface, cleared_rect.x, cleared_rect.y,
                       cleared_rect.w, cleared_rect.h);
    Uint32 white = SDL_MapRGB(surface->format, 255, 255, 255);
    for (int i = 0; i < length; i++)
        draw_glyph(surface, seek_notice[length - 1 - i],
                   x + i * 6 * scale, y, scale,
                   black, white);
    seek_notice_rect = new_rect;
    seek_notice_drawn = true;
    SDL_UpdateRect(surface, seek_notice_rect.x, seek_notice_rect.y,
                   seek_notice_rect.w, seek_notice_rect.h);
}

static void set_seek_notice(bool forward, long step)
{
    snprintf(seek_notice, sizeof(seek_notice), "%c%ld%s",
             forward ? '+' : '-', step >= 60 ? step / 60 : step,
             step >= 60 ? "m" : "s");
    seek_notice_forward = forward;
    seek_notice_until = SDL_GetTicks() + 900;
}

static bool progressive_seek(SDL_Event *event, Uint32 now)
{
    if (seek_input != SDLK_LEFT && seek_input != SDLK_RIGHT)
        return false;
    if (seek_last_step != 0 && now - seek_last_step < 450)
        return false;

    Uint32 held = now - seek_started_at;
    SDLKey out;
    long step;
    if (held < 1500) {
        out = seek_input == SDLK_LEFT ? SDLK_LEFT : SDLK_RIGHT;
        step = 10;
    } else if (held < 3500) {
        out = seek_input == SDLK_LEFT ? SDLK_DOWN : SDLK_UP;
        step = 60;
    } else {
        // FFplay has no native five-minute key. Deliver five one-minute
        // events as one logical step, tagging the four queued events so our
        // input filter passes them straight through.
        out = seek_input == SDLK_LEFT ? SDLK_DOWN : SDLK_UP;
        step = 300;
        for (int i = 0; i < 4; i++) {
            SDL_Event extra;
            key(&extra, out, SDL_PRESSED);
            extra.key.keysym.unicode = SYNTHETIC_SEEK_UNICODE;
            SDL_PushEvent(&extra);
        }
    }

    position_seconds += seek_input == SDLK_LEFT ? -step : step;
    if (position_seconds < 0)
        position_seconds = 0;
    save_position();
    save_checkpoint();
    set_seek_notice(seek_input == SDLK_RIGHT, step);
    seek_last_step = now;
    key(event, out, SDL_PRESSED);
    return true;
}

static bool map_event(SDL_Event *event)
{
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)
        return false;
    if (event->key.keysym.unicode == SYNTHETIC_SEEK_UNICODE)
        return false;
    Uint8 state = event->key.state;
    SDLKey in = event->key.keysym.sym;

    if (in == SDLK_ESCAPE) {
        if (state == SDL_PRESSED) {
            // Ignore MENU auto-repeat: only the first press starts the
            // gesture, otherwise its timer and combo flag would reset.
            if (!menu_down) {
                menu_down = true;
                menu_used = false;
                menu_pressed_at = SDL_GetTicks();
            }
            return true;
        }
        menu_down = false;
        seek_input = SDLK_UNKNOWN;
        Uint32 held_ms = SDL_GetTicks() - menu_pressed_at;
        if (!menu_used && held_ms < 500)
        {
            update_clock();
            save_position();
            save_checkpoint();
            mark_menu_exit();
            key(event, SDLK_q, SDL_PRESSED);
        }
        else
            return true;
        return false;
    }

    if (menu_down && (in == SDLK_LEFT || in == SDLK_RIGHT)) {
        menu_used = true;
        if (state == SDL_RELEASED) {
            if (seek_input == in)
                seek_input = SDLK_UNKNOWN;
            return true;
        }
        Uint32 now = SDL_GetTicks();
        if (seek_input != in) {
            seek_input = in;
            seek_started_at = now;
            seek_last_step = 0;
        }
        if (progressive_seek(event, now))
            return false;
        return true;
    }

    if (menu_down && state == SDL_PRESSED) {
        SDLKey out = SDLK_UNKNOWN;
        // Any key used while MENU is held makes this a combination, even
        // when FFplay does not need the key (notably hardware volume keys).
        menu_used = true;
        if (in == SDLK_UP) out = SDLK_LSHIFT;       /* native X: +600 s */
        if (in == SDLK_DOWN) out = SDLK_LALT;        /* native Y: -600 s */
        if (out != SDLK_UNKNOWN) {
            if (in == SDLK_UP) position_seconds += 600;
            if (in == SDLK_DOWN) position_seconds -= 600;
            if (position_seconds < 0) position_seconds = 0;
            save_position();
            save_checkpoint();
            set_seek_notice(in == SDLK_UP, 600);
            key(event, out, SDL_PRESSED);
            return false;
        }
    }

    if (in == SDLK_SPACE) { /* Miyoo A: resume only */
        if (state == SDL_PRESSED && paused) {
            paused = false;
            key(event, SDLK_SPACE, SDL_PRESSED);
            return false;
        }
        return true;
    }
    if (in == SDLK_LCTRL) { /* Miyoo B: pause only */
        if (state == SDL_PRESSED && !paused) {
            paused = true;
            key(event, SDLK_SPACE, SDL_PRESSED);
            return false;
        }
        return true;
    }
    return true; /* child mode: discard every other key */
}

int SDL_PollEvent(SDL_Event *event)
{
    if (!real_poll)
        real_poll = (poll_fn)dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!real_poll && !real_wait)
        real_wait = (wait_fn)dlsym(RTLD_NEXT, "SDL_WaitEvent");
    ensure_key_repeat();
    update_clock();
    while (real_poll) {
        inside_event_call = true;
        int got = real_poll(event);
        inside_event_call = false;
        if (!got)
            break;
        if (!map_event(event))
            return 1;
    }
    if (progressive_seek(event, SDL_GetTicks()))
        return 1;
    return 0;
}

int SDL_WaitEvent(SDL_Event *event)
{
    // Do not block in the real SDL_WaitEvent while a seek key is held: the
    // Miyoo input driver does not reliably emit key-repeat events. Polling
    // here lets us generate the progressively larger seek steps ourselves.
    ensure_key_repeat();
    if (!real_poll)
        real_poll = (poll_fn)dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!real_poll && !real_wait)
        real_wait = (wait_fn)dlsym(RTLD_NEXT, "SDL_WaitEvent");
    while (real_poll) {
        update_clock();
        inside_event_call = true;
        int got = real_poll(event);
        inside_event_call = false;
        if (got && !map_event(event))
            return 1;
        if (progressive_seek(event, SDL_GetTicks()))
            return 1;
        SDL_Delay(10);
    }
    return real_wait ? real_wait(event) : 0;
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action,
                   Uint32 mask)
{
    ensure_key_repeat();
    if (!real_peep)
        real_peep = (peep_fn)dlsym(RTLD_NEXT, "SDL_PeepEvents");
    if (!real_peep)
        return -1;
    update_clock();
    int count = real_peep(events, numevents, action, mask);
    if (inside_event_call)
        return count;
    if (action != SDL_GETEVENT || count <= 0)
        return count;
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (!map_event(&events[i])) {
            if (kept != i)
                events[kept] = events[i];
            kept++;
        }
    }
    if (action == SDL_GETEVENT && kept < numevents &&
        progressive_seek(&events[kept], SDL_GetTicks()))
        kept++;
    return kept;
}

int SDL_DisplayYUVOverlay(SDL_Overlay *overlay, SDL_Rect *dstrect)
{
    if (!real_overlay)
        real_overlay = (overlay_fn)dlsym(RTLD_NEXT, "SDL_DisplayYUVOverlay");
    if (!real_overlay)
        return -1;
    int result = real_overlay(overlay, dstrect);
    draw_seek_notice();
    return result;
}
