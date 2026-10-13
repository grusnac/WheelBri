#include "wheelbri/HidHideCli.hpp"

#include "wheelbri/HidG920.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

namespace wheelbri::hidhide {

namespace {

// A small JSON reader: enough for HidHideCLI's output (objects, arrays, strings, numbers, true/false/null).
struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    std::string text;                                  // string, or the number as written
    std::vector<Value> items;                          // array
    std::vector<std::pair<std::string, Value>> fields; // object

    [[nodiscard]] const Value* field(std::string_view name) const {
        for (const auto& [key, value] : fields) {
            if (key == name) return &value;
        }
        return nullptr;
    }
};

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    std::optional<Value> document() {
        auto value = parse(0);
        skipSpace();
        if (!value || pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void skipSpace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool take(char c) {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::optional<std::string> string() {
        if (!take('"')) return std::nullopt;
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return std::nullopt;
            const char e = text_[pos_++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return std::nullopt;
                    // Exactly four hex digits; anything else is a broken string, not an exception.
                    unsigned code = 0;
                    const std::string_view hex = text_.substr(pos_, 4);
                    const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), code, 16);
                    if (parsed.ec != std::errc() || parsed.ptr != hex.data() + hex.size()) return std::nullopt;
                    pos_ += 4;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: out += e; break;   // \" \\ \/ (and HidHide's "\?" kept as "?")
            }
        }
        return std::nullopt;
    }

    std::optional<Value> parse(const int depth) {
        if (depth > 32) return std::nullopt;
        skipSpace();
        if (pos_ >= text_.size()) return std::nullopt;
        Value value;
        const char c = text_[pos_];
        if (c == '{') {
            ++pos_;
            value.kind = Value::Kind::Object;
            if (take('}')) return value;
            do {
                auto key = string();
                if (!key || !take(':')) return std::nullopt;
                auto item = parse(depth + 1);
                if (!item) return std::nullopt;
                value.fields.emplace_back(std::move(*key), std::move(*item));
            } while (take(','));
            if (!take('}')) return std::nullopt;
            return value;
        }
        if (c == '[') {
            ++pos_;
            value.kind = Value::Kind::Array;
            if (take(']')) return value;
            do {
                auto item = parse(depth + 1);
                if (!item) return std::nullopt;
                value.items.push_back(std::move(*item));
            } while (take(','));
            if (!take(']')) return std::nullopt;
            return value;
        }
        if (c == '"') {
            auto s = string();
            if (!s) return std::nullopt;
            value.kind = Value::Kind::String;
            value.text = std::move(*s);
            return value;
        }
        for (const auto& [word, kind, flag] : {std::tuple{std::string_view("true"), Value::Kind::Bool, true},
                                               std::tuple{std::string_view("false"), Value::Kind::Bool, false},
                                               std::tuple{std::string_view("null"), Value::Kind::Null, false}}) {
            if (text_.substr(pos_, word.size()) == word) {
                pos_ += word.size();
                value.kind = kind;
                value.boolean = flag;
                return value;
            }
        }
        const std::size_t start = pos_;
        while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '-' ||
                                       text_[pos_] == '+' || text_[pos_] == '.' || text_[pos_] == 'e' ||
                                       text_[pos_] == 'E')) {
            ++pos_;
        }
        if (pos_ == start) return std::nullopt;
        value.kind = Value::Kind::Number;
        value.text = std::string(text_.substr(start, pos_ - start));
        return value;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::string flag(const Value& device, std::string_view name) {
    const Value* v = device.field(name);
    if (v == nullptr) return "?";
    if (v->kind == Value::Kind::Bool) return v->boolean ? "true" : "false";
    return v->text;
}

// Calls `visit(device, path)` for every device whose instance path names `vendorId`.
template <typename Visit>
void forEachDevice(std::string_view json, std::string_view vendorId, Visit visit) {
    const auto root = Reader(json).document();
    if (!root || root->kind != Value::Kind::Array) return;
    const std::string needle = "VID_" + std::string(vendorId);
    for (const Value& container : root->items) {
        const Value* devices = container.field("devices");
        if (devices == nullptr || devices->kind != Value::Kind::Array) continue;
        for (const Value& device : devices->items) {
            const Value* path = device.field("deviceInstancePath");
            if (path == nullptr || path->kind != Value::Kind::String) continue;
            if (containsIgnoreCase(path->text, needle)) visit(device, path->text);
        }
    }
}

}  // namespace

namespace {

std::optional<std::uint16_t> hex4(const std::string_view text) {
    if (text.size() < 4) return std::nullopt;
    unsigned value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = text[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return std::nullopt;
        value = value * 16 + digit;
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::optional<DeviceId> hidDeviceIdOf(const std::string_view instanceId) {
    constexpr std::string_view kVid = "HID\\VID_";
    constexpr std::string_view kPid = "&PID_";
    if (instanceId.size() < kVid.size() + 4 + kPid.size() + 4) return std::nullopt;
    if (!equalsIgnoreCase(instanceId.substr(0, kVid.size()), kVid)) return std::nullopt;
    const auto vendor = hex4(instanceId.substr(kVid.size()));
    const std::size_t pidAt = kVid.size() + 4;
    if (!equalsIgnoreCase(instanceId.substr(pidAt, kPid.size()), kPid)) return std::nullopt;
    const auto product = hex4(instanceId.substr(pidAt + kPid.size()));
    if (!vendor || !product) return std::nullopt;
    return DeviceId{*vendor, *product};
}

bool isGameController(const std::vector<std::string>& compatibleIds) {
    for (const std::string& id : compatibleIds) {
        if (equalsIgnoreCase(id, "HID_DEVICE_SYSTEM_GAME") || equalsIgnoreCase(id, "HID_DEVICE_UP:0001_U:0004") ||
            equalsIgnoreCase(id, "HID_DEVICE_UP:0001_U:0005") || equalsIgnoreCase(id, "HID_DEVICE_UP:0001_U:0008")) {
            return true;
        }
        constexpr std::string_view kSimulation = "HID_DEVICE_UP:0002_";
        if (id.size() >= kSimulation.size() && equalsIgnoreCase(std::string_view(id).substr(0, kSimulation.size()), kSimulation)) {
            return true;
        }
    }
    return false;
}

std::string neverHiddenUsage(const std::vector<std::string>& compatibleIds) {
    struct Rule {
        std::string_view id;
        bool prefix;
        std::string_view what;
    };
    static constexpr Rule kRules[] = {
        {"HID_DEVICE_SYSTEM_KEYBOARD", false, "a keyboard"},
        {"HID_DEVICE_SYSTEM_MOUSE", false, "a mouse"},
        {"HID_DEVICE_SYSTEM_CONTROL", false, "system controls"},
        {"HID_DEVICE_SYSTEM_CONSUMER", false, "consumer controls"},
        {"HID_DEVICE_UP:0001_U:0001", false, "a pointer"},
        {"HID_DEVICE_UP:0001_U:0002", false, "a mouse"},
        {"HID_DEVICE_UP:0001_U:0006", false, "a keyboard"},
        {"HID_DEVICE_UP:0001_U:0007", false, "a keypad"},
        {"HID_DEVICE_UP:0001_U:0080", false, "system controls"},
        {"HID_DEVICE_UP:000C_", true, "consumer controls"},
        {"HID_DEVICE_UP:000D_", true, "a digitizer"},
    };
    for (const std::string& id : compatibleIds) {
        for (const Rule& rule : kRules) {
            const bool match = rule.prefix ? id.size() >= rule.id.size() &&
                                                 equalsIgnoreCase(std::string_view(id).substr(0, rule.id.size()), rule.id)
                                           : equalsIgnoreCase(id, rule.id);
            if (match) return std::string(rule.what) + " (" + id + ")";
        }
    }
    return {};
}

std::string instanceIdOfHidPath(std::string_view path) {
    constexpr std::string_view kPrefix = "\\\\?\\";
    if (path.size() >= kPrefix.size() && path.substr(0, kPrefix.size()) == kPrefix) path.remove_prefix(kPrefix.size());
    if (path.size() < 4 || !equalsIgnoreCase(path.substr(0, 4), "hid#")) return {};
    const std::size_t guid = path.rfind("#{");
    if (guid != std::string_view::npos) path = path.substr(0, guid);
    std::string id;
    id.reserve(path.size());
    for (const char c : path) {
        id += c == '#' ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return id;
}

bool mayHide(const DeviceId& id) {
    return !id.empty() && !isVirtualWheelId(id.vendorId, id.productId);
}

std::string deviceIdText(const DeviceId& id) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04x:%04x", id.vendorId, id.productId);
    return text;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool containsIgnoreCase(std::string_view text, std::string_view part) {
    if (part.size() > text.size()) return false;
    for (std::size_t i = 0; i + part.size() <= text.size(); ++i) {
        if (equalsIgnoreCase(text.substr(i, part.size()), part)) return true;
    }
    return false;
}

std::vector<std::string> gamingDevicesOf(std::string_view json, std::string_view vendorId) {
    std::vector<std::string> result;
    forEachDevice(json, vendorId, [&](const Value& device, const std::string& path) {
        const Value* gaming = device.field("gamingDevice");
        if (gaming == nullptr || gaming->kind != Value::Kind::Bool || !gaming->boolean) return;
        const bool known = std::any_of(result.begin(), result.end(), [&](const std::string& p) { return equalsIgnoreCase(p, path); });
        if (!known) result.push_back(path);
    });
    return result;
}

std::vector<std::string> describeDevicesOf(std::string_view json, std::string_view vendorId) {
    std::vector<std::string> result;
    forEachDevice(json, vendorId, [&](const Value& device, const std::string& path) {
        result.push_back(path + " (gamingDevice " + flag(device, "gamingDevice") + ", present " +
                         flag(device, "present") + ")");
    });
    return result;
}

std::vector<std::string> quotedValues(std::string_view output) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string_view::npos) end = output.size();
        const std::string_view line = output.substr(start, end - start);
        const std::size_t first = line.find('"');
        const std::size_t last = line.rfind('"');
        if (first != std::string_view::npos && last > first) {
            result.emplace_back(line.substr(first + 1, last - first - 1));
        }
        start = end + 1;
    }
    return result;
}

std::vector<std::string> storedSpellings(const std::vector<std::string>& stored, const std::vector<std::string>& wanted) {
    std::vector<std::string> result;
    for (const auto& entry : stored) {
        if (std::any_of(wanted.begin(), wanted.end(), [&](const std::string& w) { return equalsIgnoreCase(entry, w); })) {
            result.push_back(entry);
        }
    }
    return result;
}

}  // namespace wheelbri::hidhide
