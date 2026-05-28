/*
 * SDLmain replacement for modern MinGW (UCRT).
 *
 * The pre-built libSDLmain.a from SDL 1.2.15 references _iob
 * which doesn't exist in UCRT. This stub replaces it.
 *
 * SDL_main.h redefines main -> SDL_main, so the application's
 * main() becomes SDL_main(). We provide the real entry point.
 */

extern int SDL_main(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    return SDL_main(argc, argv);
}
