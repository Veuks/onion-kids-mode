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
static poll_fn real_poll;
static wait_fn real_wait;
static peep_fn real_peep;
static bool inside_event_call;
static bool paused;
static bool menu_down;
static bool menu_used;
static bool clock_ready;
static Uint32 clock_tick;
static Uint32 last_save;
static long position_seconds;
static const char *position_file;

__attribute__((constructor)) static void vcinput_loaded(void)
{
    FILE *fp = fopen("/tmp/vcinput_loaded", "w");
    if (fp) {
        fputs("1\n", fp);
        fclose(fp);
    }
}

static void save_position(void)
{
    if (!position_file || !*position_file)
        return;
    FILE *fp = fopen(position_file, "w");
    if (fp) {
        fprintf(fp, "%ld\n", position_seconds < 0 ? 0 : position_seconds);
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
        clock_tick = last_save = now;
        clock_ready = true;
    }
    if (!paused && now - clock_tick >= 1000) {
        position_seconds += (now - clock_tick) / 1000;
        clock_tick += ((now - clock_tick) / 1000) * 1000;
    } else if (paused) {
        clock_tick = now;
    }
    if (now - last_save >= 5000) {
        save_position();
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

static bool map_event(SDL_Event *event)
{
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)
        return false;
    Uint8 state = event->key.state;
    SDLKey in = event->key.keysym.sym;

    if (in == SDLK_ESCAPE) {
        if (state == SDL_PRESSED) {
            menu_down = true;
            menu_used = false;
            return true;
        }
        menu_down = false;
        if (!menu_used)
        {
            save_position();
            key(event, SDLK_q, SDL_PRESSED);
        }
        else
            return true;
        return false;
    }

    if (menu_down && state == SDL_PRESSED) {
        SDLKey out = SDLK_UNKNOWN;
        if (in == SDLK_LEFT) out = SDLK_DOWN;       /* -60 s */
        if (in == SDLK_RIGHT) out = SDLK_UP;        /* +60 s */
        if (in == SDLK_UP) out = SDLK_PAGEUP;       /* +600 s */
        if (in == SDLK_DOWN) out = SDLK_PAGEDOWN;   /* -600 s */
        if (out != SDLK_UNKNOWN) {
            menu_used = true;
            if (in == SDLK_LEFT) position_seconds -= 60;
            if (in == SDLK_RIGHT) position_seconds += 60;
            if (in == SDLK_UP) position_seconds += 600;
            if (in == SDLK_DOWN) position_seconds -= 600;
            if (position_seconds < 0) position_seconds = 0;
            save_position();
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
    return 0;
}

int SDL_WaitEvent(SDL_Event *event)
{
    if (!real_wait)
        real_wait = (wait_fn)dlsym(RTLD_NEXT, "SDL_WaitEvent");
    while (real_wait) {
        update_clock();
        inside_event_call = true;
        int got = real_wait(event);
        inside_event_call = false;
        if (!got)
            return 0;
        if (!map_event(event))
            return 1;
    }
    return 0;
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action,
                   Uint32 mask)
{
    if (!real_peep)
        real_peep = (peep_fn)dlsym(RTLD_NEXT, "SDL_PeepEvents");
    if (!real_peep)
        return -1;
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
    return kept;
}
