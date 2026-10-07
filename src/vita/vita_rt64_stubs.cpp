// SDL2 and native file dialog functions RT64 references, stubbed for the Vita.
//
// RT64 only calls these from its developer inspector (ImGui's SDL backend), its window-creation path and its file
// dialogs. None of those run here: the port passes RT64 a window handle and developer mode is off. The VitaSDK SDL2
// is built on vitaGL, which would pull a second GPU layer and a runtime shader compiler into an app that drives GXM
// itself, so these stubs are linked instead. Each one logs the first time it is called, so a path that does reach
// them shows up in the log.

#include <SDL.h>
#include <SDL_syswm.h>
#include <nfd.h>

#include "vita_log.h"

#define HM64_STUB_CALLED(name)                                                                  \
    do {                                                                                        \
        static bool logged = false;                                                             \
        if (!logged) {                                                                          \
            logged = true;                                                                      \
            hm64vita::log_line("RT64 stub called: %s (not available on the Vita)", name);       \
        }                                                                                       \
    } while (0)

extern "C" {
    // SDL2

    Uint32 SDL_WasInit(Uint32 flags) { HM64_STUB_CALLED("SDL_WasInit"); return 0; }
    SDL_bool SDL_SetHint(const char* name, const char* value) { HM64_STUB_CALLED("SDL_SetHint"); return SDL_FALSE; }
    void SDL_free(void* mem) { HM64_STUB_CALLED("SDL_free"); }
    const char* SDL_GetCurrentVideoDriver(void) { HM64_STUB_CALLED("SDL_GetCurrentVideoDriver"); return nullptr; }

    SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) { HM64_STUB_CALLED("SDL_CreateWindow"); return nullptr; }
    SDL_Window* SDL_GetWindowFromID(Uint32 id) { HM64_STUB_CALLED("SDL_GetWindowFromID"); return nullptr; }
    SDL_bool SDL_GetWindowWMInfo(SDL_Window* window, SDL_SysWMinfo* info) { HM64_STUB_CALLED("SDL_GetWindowWMInfo"); return SDL_FALSE; }
    Uint32 SDL_GetWindowFlags(SDL_Window* window) { HM64_STUB_CALLED("SDL_GetWindowFlags"); return 0; }
    void SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
        HM64_STUB_CALLED("SDL_GetWindowSize");
        if (w != nullptr) *w = 960;
        if (h != nullptr) *h = 544;
    }
    void SDL_GetWindowPosition(SDL_Window* window, int* x, int* y) {
        HM64_STUB_CALLED("SDL_GetWindowPosition");
        if (x != nullptr) *x = 0;
        if (y != nullptr) *y = 0;
    }
    void SDL_GL_GetDrawableSize(SDL_Window* window, int* w, int* h) {
        HM64_STUB_CALLED("SDL_GL_GetDrawableSize");
        if (w != nullptr) *w = 960;
        if (h != nullptr) *h = 544;
    }
    int SDL_GetRendererOutputSize(SDL_Renderer* renderer, int* w, int* h) {
        HM64_STUB_CALLED("SDL_GetRendererOutputSize");
        if (w != nullptr) *w = 960;
        if (h != nullptr) *h = 544;
        return 0;
    }
    SDL_Window* SDL_GetKeyboardFocus(void) { HM64_STUB_CALLED("SDL_GetKeyboardFocus"); return nullptr; }

    void SDL_SetEventFilter(SDL_EventFilter filter, void* userdata) { HM64_STUB_CALLED("SDL_SetEventFilter"); }
    SDL_bool SDL_GetEventFilter(SDL_EventFilter* filter, void** userdata) { HM64_STUB_CALLED("SDL_GetEventFilter"); return SDL_FALSE; }

    Uint64 SDL_GetPerformanceCounter(void) { HM64_STUB_CALLED("SDL_GetPerformanceCounter"); return 0; }
    Uint64 SDL_GetPerformanceFrequency(void) { HM64_STUB_CALLED("SDL_GetPerformanceFrequency"); return 1; }

    SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id) { HM64_STUB_CALLED("SDL_CreateSystemCursor"); return nullptr; }
    void SDL_SetCursor(SDL_Cursor* cursor) { HM64_STUB_CALLED("SDL_SetCursor"); }
    void SDL_FreeCursor(SDL_Cursor* cursor) { HM64_STUB_CALLED("SDL_FreeCursor"); }
    int SDL_ShowCursor(int toggle) { HM64_STUB_CALLED("SDL_ShowCursor"); return 0; }
    int SDL_CaptureMouse(SDL_bool enabled) { HM64_STUB_CALLED("SDL_CaptureMouse"); return -1; }
    void SDL_WarpMouseInWindow(SDL_Window* window, int x, int y) { HM64_STUB_CALLED("SDL_WarpMouseInWindow"); }
    Uint32 SDL_GetGlobalMouseState(int* x, int* y) {
        HM64_STUB_CALLED("SDL_GetGlobalMouseState");
        if (x != nullptr) *x = 0;
        if (y != nullptr) *y = 0;
        return 0;
    }

    void SDL_SetTextInputRect(const SDL_Rect* rect) { HM64_STUB_CALLED("SDL_SetTextInputRect"); }
    char* SDL_GetClipboardText(void) { HM64_STUB_CALLED("SDL_GetClipboardText"); return nullptr; }
    int SDL_SetClipboardText(const char* text) { HM64_STUB_CALLED("SDL_SetClipboardText"); return -1; }

    // Native file dialog: no dialogs on the Vita, so every dialog reports "cancelled".

    nfdresult_t NFD_Init(void) { HM64_STUB_CALLED("NFD_Init"); return NFD_OKAY; }
    void NFD_Quit(void) { HM64_STUB_CALLED("NFD_Quit"); }
    void NFD_FreePathN(nfdnchar_t* filePath) { HM64_STUB_CALLED("NFD_FreePathN"); }
    nfdresult_t NFD_OpenDialogN(nfdnchar_t** outPath, const nfdnfilteritem_t* filterList, nfdfiltersize_t filterCount, const nfdnchar_t* defaultPath) {
        HM64_STUB_CALLED("NFD_OpenDialogN");
        return NFD_CANCEL;
    }
    nfdresult_t NFD_SaveDialogN(nfdnchar_t** outPath, const nfdnfilteritem_t* filterList, nfdfiltersize_t filterCount, const nfdnchar_t* defaultPath, const nfdnchar_t* defaultName) {
        HM64_STUB_CALLED("NFD_SaveDialogN");
        return NFD_CANCEL;
    }
    nfdresult_t NFD_PickFolderN(nfdnchar_t** outPath, const nfdnchar_t* defaultPath) {
        HM64_STUB_CALLED("NFD_PickFolderN");
        return NFD_CANCEL;
    }
}
