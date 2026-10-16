// Review points 4 and 6 e2e (2026-09-26). Test-only LD_PRELOAD shim, never
// linked into dirk: attaches SDL virtual joysticks right after dirk
// initialises its gamepad/joystick subsystem, then drives their hat/axes/
// buttons on a wall-clock timeline (env DIRK_TEST_TIMELINE) from inside
// SDL_PollEvent -- same thread as dirk's event loop, so no locking games.
// Markers are printed on stdout (same FILE* as Lua's print) for ordering.
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static SDL_Joystick* joys[2];
static Uint64 t0;
typedef struct { long ms; int dev; char kind; int idx; int val; char label[48]; } Act;
static Act acts[128];
static int nacts, next;

static void parse(void) {
    const char* s = getenv("DIRK_TEST_TIMELINE");
    if (!s) return;
    char* dup = strdup(s);
    for (char* tok = strtok(dup, ";"); tok && nacts < 128; tok = strtok(NULL, ";")) {
        Act a; memset(&a, 0, sizeof a);
        // ms,dev,kind(h|a|b),idx,val,label
        if (sscanf(tok, "%ld,%d,%c,%d,%d,%47s", &a.ms, &a.dev, &a.kind, &a.idx, &a.val, a.label) >= 5) acts[nacts++] = a;
    }
    free(dup);
}

static void attach(int dev, Uint16 type, const char* name) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = type;
    d.naxes = 6; d.nbuttons = 15; d.nhats = 1;
    d.name = name;
    if (type == SDL_JOYSTICK_TYPE_GAMEPAD) {
        d.button_mask = 0x7fff;
        d.axis_mask = 0x3f;
    }
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    joys[dev] = id ? SDL_OpenJoystick(id) : NULL;
    printf("SHIM attached dev%d '%s' id=%u open=%s\n", dev, name, static_cast<unsigned>(id), joys[dev] ? "yes" : SDL_GetError());
    fflush(stdout);
}

extern "C" bool SDL_InitSubSystem(SDL_InitFlags flags) {
    static bool (*real)(SDL_InitFlags);
    static int done;
    if (!real) real = reinterpret_cast<bool (*)(SDL_InitFlags)>(dlsym(RTLD_NEXT, "SDL_InitSubSystem"));
    bool ok = real(flags);
    if (ok && !done && (flags & (SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK))) {
        done = 1;
        parse();
        attach(0, SDL_JOYSTICK_TYPE_UNKNOWN, "Dirk test raw stick");
        if (getenv("DIRK_TEST_GAMEPAD")) attach(1, SDL_JOYSTICK_TYPE_GAMEPAD, "Dirk test gamepad");
    }
    return ok;
}

extern "C" bool SDL_PollEvent(SDL_Event* ev) {
    static bool (*real)(SDL_Event*);
    if (!real) real = reinterpret_cast<bool (*)(SDL_Event*)>(dlsym(RTLD_NEXT, "SDL_PollEvent"));
    if (nacts) {
        Uint64 now = SDL_GetTicks();
        if (!t0) t0 = now;
        while (next < nacts && static_cast<long>(now - t0) >= acts[next].ms) {
            Act* a = &acts[next++];
            SDL_Joystick* j = joys[a->dev];
            if (j) {
                if (a->kind == 'h') SDL_SetJoystickVirtualHat(j, a->idx, static_cast<Uint8>(a->val));
                else if (a->kind == 'a') SDL_SetJoystickVirtualAxis(j, a->idx, static_cast<Sint16>(a->val));
                else if (a->kind == 'b') SDL_SetJoystickVirtualButton(j, a->idx, a->val != 0);
            }
            if (a->kind == 'p') { // screenshot: DIRK_SCREENSHOT=<DIRK_TEST_SHOTDIR>/<label>.png, then F6
                char path[512];
                snprintf(path, sizeof path, "%s/%s.png", getenv("DIRK_TEST_SHOTDIR") ? getenv("DIRK_TEST_SHOTDIR") : "/tmp", a->label);
                SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "DIRK_SCREENSHOT", path, true);
                SDL_Event k; SDL_zero(k);
                k.type = SDL_EVENT_KEY_DOWN; k.key.scancode = SDL_SCANCODE_F6; k.key.key = SDLK_F6; k.key.down = true;
                SDL_PushEvent(&k);
                k.type = SDL_EVENT_KEY_UP; k.key.down = false;
                SDL_PushEvent(&k);
            }
            if (a->kind == 'k') { // synthetic keyboard event, idx = scancode
                SDL_Event k; SDL_zero(k);
                k.type = a->val ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                k.key.scancode = static_cast<SDL_Scancode>(a->idx);
                k.key.key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(a->idx), SDL_KMOD_NONE, false);
                k.key.down = a->val != 0;
                SDL_PushEvent(&k);
            }
            // Optional: report a watched file's mtime (ns) at every action,
            // e.g. to tell whether dirk rewrote game.cfg in between.
            long long mt = -1;
            struct stat st;
            if (getenv("DIRK_TEST_WATCH") && stat(getenv("DIRK_TEST_WATCH"), &st) == 0)
                mt = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
            printf("SHIM t=%ld %s mtime=%lld\n", a->ms, a->label[0] ? a->label : "-", mt);
            fflush(stdout);
        }
    }
    return real(ev);
}
__attribute__((constructor)) static void lineBufferedStdout() { setvbuf(stdout, NULL, _IOLBF, 0); }
