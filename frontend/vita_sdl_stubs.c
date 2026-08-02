#include <SDL.h>

#ifdef PSVITA_TARGET

int SDL_OpenURL(const char* url) {
    (void)url;
    return -1;
}

int SDL_SetClipboardText(const char* text) {
    (void)text;
    return -1;
}

SDL_bool SDL_HasClipboardText(void) {
    return SDL_FALSE;
}

char* SDL_GetClipboardText(void) {
    char* text = (char*)SDL_malloc(1);
    if (text) {
        text[0] = '\0';
    }
    return text;
}

#endif
