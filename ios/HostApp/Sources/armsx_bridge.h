#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Provided by libarmsx.dylib (__DLL_BUILD)
//
// external_main keeps the plain `int(int, char**)` main signature — SDL's Android glue
// calls this symbol through a SDL_main_func pointer of exactly that shape, so it must not
// grow extra parameters. Hosts that own the SDL_Window/SDL_Renderer themselves call
// external_main_ex instead.
int external_main(int argc, const char* argv[]);
int external_main_ex(int argc, const char* argv[], void* external_window, void* external_renderer);
void psxe_enqueue_launch_argument(const char* argument);

#ifdef __cplusplus
}
#endif
