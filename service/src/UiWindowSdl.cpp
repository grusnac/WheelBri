// The `--ui` window through SDL3: the fallback for systems without a native backend (macOS) or a Linux
// build without the X11 headers.

#include "UiWindow.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace wheelemu::service::ui {

namespace {

class SdlWindow final : public UiWindow {
public:
    SdlWindow(SDL_Window* window, SDL_Renderer* renderer) : window_(window), renderer_(renderer) {}

    ~SdlWindow() override {
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    WindowInput pump() override {
        WindowInput input;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    input.close = true;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        input.close = true;
                    } else if (!event.key.repeat && event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
                        input.keys += static_cast<char>('a' + (event.key.key - SDLK_A));
                    }
                    break;
                default:
                    break;
            }
        }
        return input;
    }

    [[nodiscard]] bool visible() const override {
        return (SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) == 0;
    }

    void begin(const Colour background) override {
        colour(background);
        SDL_RenderClear(renderer_);
    }

    void fill(const float x, const float y, const float w, const float h, const Colour c) override {
        colour(c);
        const SDL_FRect rect{x, y, w, h};
        SDL_RenderFillRect(renderer_, &rect);
    }

    void outline(const float x, const float y, const float w, const float h, const Colour c) override {
        colour(c);
        const SDL_FRect rect{x, y, w, h};
        SDL_RenderRect(renderer_, &rect);
    }

    void text(const float x, const float y, const std::string_view value, const Colour c) override {
        colour(c);
        const std::string copy(value);
        SDL_RenderDebugText(renderer_, x, y, copy.c_str());   // 8x8 per character, as the layout expects
    }

    void present() override { SDL_RenderPresent(renderer_); }

    bool openFolder(const std::filesystem::path& folder, std::string& error) override {
        if (SDL_OpenURL(folder.string().c_str())) {
            return true;
        }
        error = SDL_GetError();
        return false;
    }

    [[nodiscard]] std::string describe() const override {
        const char* const name = SDL_GetRendererName(renderer_);
        return std::string("SDL, renderer ") + (name != nullptr ? name : "?");
    }

private:
    void colour(const Colour c) { SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, SDL_ALPHA_OPAQUE); }

    SDL_Window* window_;
    SDL_Renderer* renderer_;
};

}  // namespace

std::unique_ptr<UiWindow> openWindow(const std::string& title, const int logicalWidth, const int logicalHeight,
                                     const int windowWidth, const int windowHeight, std::string& error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        error = std::string("SDL video: ") + SDL_GetError();
        return nullptr;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer(title.c_str(), windowWidth, windowHeight, SDL_WINDOW_RESIZABLE, &window,
                                     &renderer)) {
        error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return nullptr;
    }
    SDL_SetRenderLogicalPresentation(renderer, logicalWidth, logicalHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 0);   // never let presenting a frame stall anything
    return std::make_unique<SdlWindow>(window, renderer);
}

}  // namespace wheelemu::service::ui
