#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wheelbri {

// One frame of a window as a list of drawing commands, recorded instead of drawn at once, so a frame that looks the
// same as the one on screen is not drawn again. Drawing the app's window with GDI 60 times a second kept its thread
// at 15-18 % of a core even while nothing changed (2026-10-06); recording and comparing costs a fraction of that.
// Commands and their text keep their memory from frame to frame: recording a frame like the last allocates nothing.
class DrawList {
public:
    using Rgb = std::array<std::uint8_t, 3>;

    struct Command {
        enum class Kind : std::uint8_t { Fill, Text };
        Kind kind = Kind::Fill;
        float x = 0.0F;
        float y = 0.0F;
        float w = 0.0F;   // fills only
        float h = 0.0F;
        Rgb colour{};
        std::string text;   // texts only, UTF-8

        friend bool operator==(const Command&, const Command&) = default;
    };

    // A new frame on `background`.
    void begin(const Rgb background) {
        background_ = background;
        used_ = 0;
    }
    void fill(const float x, const float y, const float w, const float h, const Rgb colour) {
        Command& c = next(Command::Kind::Fill, x, y, colour);
        c.w = w;
        c.h = h;
        c.text.clear();
    }
    void text(const float x, const float y, const std::string_view value, const Rgb colour) {
        Command& c = next(Command::Kind::Text, x, y, colour);
        c.w = 0.0F;
        c.h = 0.0F;
        c.text.assign(value);
    }

    [[nodiscard]] std::span<const Command> commands() const { return {commands_.data(), used_}; }
    [[nodiscard]] Rgb background() const { return background_; }

    // The same picture: same background and the same commands in the same order.
    [[nodiscard]] bool sameAs(const DrawList& other) const {
        if (background_ != other.background_ || used_ != other.used_) return false;
        for (std::size_t i = 0; i < used_; ++i) {
            if (!(commands_[i] == other.commands_[i])) return false;
        }
        return true;
    }

    void swap(DrawList& other) noexcept {
        commands_.swap(other.commands_);
        std::swap(background_, other.background_);
        std::swap(used_, other.used_);
    }

private:
    Command& next(const Command::Kind kind, const float x, const float y, const Rgb colour) {
        if (used_ == commands_.size()) commands_.emplace_back();
        Command& c = commands_[used_++];
        c.kind = kind;
        c.x = x;
        c.y = y;
        c.colour = colour;
        return c;
    }

    std::vector<Command> commands_;   // the first used_ are this frame's
    std::size_t used_ = 0;
    Rgb background_{};
};

}  // namespace wheelbri
