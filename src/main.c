/* main.c - PinFall entry point: SDL/TTF lifecycle around the UI module.
 *
 * Everything app-level (window, renderer, threads, feed state) lives in ui.c;
 * this file only owns process-wide SDL init/teardown.
 *
 * Safety: --snapshot and --selftest are verification modes. If the caller did
 * not force a video driver, they select SDL's "offscreen" driver so the app can
 * NEVER pop a real window on the user's live desktop during automated checks.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "android.h"

#include <stdio.h>
#include <string.h>

/* implemented in ui.c */
int pinfall_ui_main(int argc, char **argv);

static int has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], flag) == 0)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if ((has_flag(argc, argv, "--snapshot") ||
         has_flag(argc, argv, "--selftest") ||
         has_flag(argc, argv, "--auth-preview")) &&
        !SDL_getenv("SDL_VIDEODRIVER"))
        SDL_setenv("SDL_VIDEODRIVER", "offscreen", 1);

#ifdef PC_ANDROID
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
    /* we handle SDL_FINGER* ourselves; no synthesized mouse double-taps */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    pf_android_log("main: startup argc=%d", argc);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "pinfall: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    pf_android_log("main: SDL_Init ok");
    if (TTF_Init() != 0) {
        fprintf(stderr, "pinfall: TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

#ifdef PC_ANDROID
    pf_android_init();
#endif
    pf_android_log("main: TTF_Init ok, entering ui_main");
    int rc = pinfall_ui_main(argc, argv);
    pf_android_log("main: ui_main returned %d", rc);

#ifdef PC_ANDROID
    pf_android_shutdown();
#endif
    TTF_Quit();
    SDL_Quit();
    return rc;
}
