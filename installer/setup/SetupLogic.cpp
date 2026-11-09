// SPDX-License-Identifier: GPL-3.0-or-later
#include "SetupLogic.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>

namespace wheelbri::setup {
namespace {

wchar_t lower(const wchar_t c) { return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c))); }

std::wstring trimmed(std::wstring_view s) {
    while (!s.empty() && std::iswspace(static_cast<std::wint_t>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::iswspace(static_cast<std::wint_t>(s.back()))) s.remove_suffix(1);
    return std::wstring(s);
}

std::string_view withoutBom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.remove_prefix(3);
    }
    return s;
}

std::vector<std::wstring> split(std::wstring_view s, const wchar_t by) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        const size_t end = s.find(by, start);
        parts.emplace_back(s.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
        if (end == std::wstring_view::npos) return parts;
        start = end + 1;
    }
}

// --- JSON, just enough for Firefox's Preferences policy: one object, its members kept as written -----------------

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void skipSpace(std::string_view s, size_t& i) {
    while (i < s.size() && isSpace(s[i])) ++i;
}

// A string at s[i] ('"'): moves past it; its text (escapes decoded, \u kept as UTF-8) in `out`.
bool readString(std::string_view s, size_t& i, std::string* out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\') {
            if (out) out->push_back(c);
            continue;
        }
        if (i >= s.size()) return false;
        const char e = s[i++];
        switch (e) {
        case '"': case '\\': case '/': if (out) out->push_back(e); break;
        case 'b': if (out) out->push_back('\b'); break;
        case 'f': if (out) out->push_back('\f'); break;
        case 'n': if (out) out->push_back('\n'); break;
        case 'r': if (out) out->push_back('\r'); break;
        case 't': if (out) out->push_back('\t'); break;
        case 'u': {
            if (i + 4 > s.size()) return false;
            unsigned code = 0;
            for (int k = 0; k < 4; ++k) {
                const char h = s[i++];
                code <<= 4;
                if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                else return false;
            }
            if (out) *out += toUtf8(std::wstring(1, static_cast<wchar_t>(code)));
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool skipValue(std::string_view s, size_t& i, int depth);

// An object or array at s[i]: moves past it.
bool skipContainer(std::string_view s, size_t& i, const int depth) {
    if (depth > 64) return false;
    const char close = s[i] == '{' ? '}' : ']';
    const bool object = close == '}';
    ++i;
    skipSpace(s, i);
    if (i < s.size() && s[i] == close) return ++i, true;
    while (i < s.size()) {
        skipSpace(s, i);
        if (object) {
            if (!readString(s, i, nullptr)) return false;
            skipSpace(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
        }
        if (!skipValue(s, i, depth + 1)) return false;
        skipSpace(s, i);
        if (i >= s.size()) return false;
        if (s[i] == close) return ++i, true;
        if (s[i] != ',') return false;
        ++i;
    }
    return false;
}

bool skipValue(std::string_view s, size_t& i, const int depth) {
    skipSpace(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '"') return readString(s, i, nullptr);
    if (s[i] == '{' || s[i] == '[') return skipContainer(s, i, depth);
    const size_t start = i;
    while (i < s.size() && !isSpace(s[i]) && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    const std::string_view word = s.substr(start, i - start);
    if (word == "true" || word == "false" || word == "null") return true;
    if (word.empty()) return false;
    return std::all_of(word.begin(), word.end(), [](const char c) {
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
    });
}

struct Member {
    std::string key;        // decoded
    std::string rawKey;     // as written, quotes included
    std::string rawValue;   // as written
};

// The members of the one object `s` is (spaces around it allowed); nullopt when it is anything else.
std::optional<std::vector<Member>> objectMembers(std::string_view s) {
    size_t i = 0;
    skipSpace(s, i);
    if (i >= s.size() || s[i] != '{') return std::nullopt;
    std::vector<Member> members;
    ++i;
    skipSpace(s, i);
    if (i < s.size() && s[i] == '}') {
        ++i;
    } else {
        while (true) {
            Member m;
            const size_t keyStart = i;
            if (!readString(s, i, &m.key)) return std::nullopt;
            m.rawKey = std::string(s.substr(keyStart, i - keyStart));
            skipSpace(s, i);
            if (i >= s.size() || s[i] != ':') return std::nullopt;
            ++i;
            skipSpace(s, i);
            const size_t valueStart = i;
            if (!skipValue(s, i, 1)) return std::nullopt;
            m.rawValue = std::string(s.substr(valueStart, i - valueStart));
            members.push_back(std::move(m));
            skipSpace(s, i);
            if (i >= s.size()) return std::nullopt;
            if (s[i] == '}') { ++i; break; }
            if (s[i] != ',') return std::nullopt;
            ++i;
            skipSpace(s, i);
        }
    }
    skipSpace(s, i);
    if (i != s.size()) return std::nullopt;
    return members;
}

std::string objectText(const std::vector<Member>& members) {
    std::string text = "{";
    for (size_t k = 0; k < members.size(); ++k) {
        if (k > 0) text += ',';
        text += members[k].rawKey + ':' + members[k].rawValue;
    }
    return text + '}';
}

constexpr std::string_view kGamepadKey = "dom.gamepad.enabled";

// Whether a dom.gamepad.enabled value says off: an object whose Value is false.
bool saysOff(const std::string& rawValue) {
    const auto inner = objectMembers(rawValue);
    if (!inner) return false;
    return std::any_of(inner->begin(), inner->end(),
                       [](const Member& m) { return m.key == "Value" && m.rawValue == "false"; });
}

bool blank(std::string_view s) { return std::all_of(s.begin(), s.end(), isSpace); }

// --- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

std::uint32_t rotr(const std::uint32_t x, const int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(std::array<std::uint32_t, 8>& h, const unsigned char* block) {
    std::array<std::uint32_t, 64> w{};
    for (int t = 0; t < 16; ++t) {
        w[t] = (std::uint32_t{block[t * 4]} << 24) | (std::uint32_t{block[t * 4 + 1]} << 16) |
               (std::uint32_t{block[t * 4 + 2]} << 8) | std::uint32_t{block[t * 4 + 3]};
    }
    for (int t = 16; t < 64; ++t) {
        const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int t = 0; t < 64; ++t) {
        const std::uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[t] + w[t];
        const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

}  // namespace

std::wstring fromUtf8(const std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code = 0xFFFD;
        size_t length = 1;
        if (c < 0x80) {
            code = c;
        } else if ((c & 0xE0) == 0xC0) {
            length = 2;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
        }
        if (length > 1) {
            if (i + length > text.size()) {
                length = 1;
            } else {
                code = c & (0x7F >> length);
                for (size_t k = 1; k < length; ++k) {
                    const auto next = static_cast<unsigned char>(text[i + k]);
                    if ((next & 0xC0) != 0x80) { code = 0xFFFD, length = 1; break; }
                    code = (code << 6) | (next & 0x3F);
                }
            }
        }
        i += length;
        if (code > 0xFFFF && sizeof(wchar_t) == 2) {
            code -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(code));
        }
    }
    return out;
}

std::string toUtf8(const std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = static_cast<std::uint32_t>(text[i]);
        if (sizeof(wchar_t) == 2 && code >= 0xD800 && code < 0xDC00 && i + 1 < text.size()) {
            const auto low = static_cast<std::uint32_t>(text[i + 1]);
            if (low >= 0xDC00 && low < 0xE000) code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00), ++i;
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

bool sameName(const std::wstring_view a, const std::wstring_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const wchar_t x, const wchar_t y) { return lower(x) == lower(y); });
}

bool NameLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](const wchar_t x, const wchar_t y) { return lower(x) < lower(y); });
}

std::wstring joinPath(const std::wstring_view a, std::wstring_view b) {
    while (!b.empty() && b.front() == L'\\') b.remove_prefix(1);
    if (a.empty()) return std::wstring(b);
    std::wstring out(a);
    if (out.back() != L'\\') out += L'\\';
    return out + std::wstring(b);
}

std::wstring parentOf(const std::wstring_view path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? std::wstring() : std::wstring(path.substr(0, slash));
}

std::wstring nameOf(const std::wstring_view path) {
    const size_t slash = path.find_last_of(L'\\');
    return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

std::wstring withoutTrailingSlash(const std::wstring_view path) {
    std::wstring out(path);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

bool isInside(const std::wstring_view path, const std::wstring_view folder) {
    const std::wstring f = withoutTrailingSlash(folder);
    if (sameName(withoutTrailingSlash(path), f)) return true;
    const std::wstring prefix = f.back() == L'\\' ? f : f + L'\\';
    return path.size() > prefix.size() && sameName(path.substr(0, prefix.size()), prefix);
}

bool isManifestEntry(const std::wstring_view entry) {
    if (entry.empty() || entry.find_first_of(L":/") != std::wstring_view::npos) return false;
    if (std::any_of(entry.begin(), entry.end(), [](const wchar_t c) {
            return c < 0x20 || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'*' || c == L'?';
        })) {
        return false;
    }
    for (const std::wstring& part : split(entry, L'\\')) {
        if (part.empty() || part == L"." || part == L"..") return false;
    }
    return true;
}

std::vector<std::wstring> textLines(const std::string_view utf8) {
    std::vector<std::wstring> lines;
    for (const std::wstring& line : split(fromUtf8(withoutBom(utf8)), L'\n')) {
        std::wstring t = trimmed(line);
        if (!t.empty()) lines.push_back(std::move(t));
    }
    return lines;
}

std::string linesFile(const std::vector<std::wstring>& lines) {
    std::string out = "\xEF\xBB\xBF";
    for (const std::wstring& line : lines) out += toUtf8(line) + "\r\n";
    return out;
}

ConfigHashes readDefaultConfigs(const std::string_view utf8) {
    ConfigHashes hashes;
    for (const std::wstring& line : textLines(utf8)) {
        const std::vector<std::wstring> parts = split(line, L'\t');
        if (parts.size() == 2) hashes[trimmed(parts[0])] = trimmed(parts[1]);
    }
    return hashes;
}

std::string defaultConfigsFile(const ConfigHashes& hashes) {
    std::vector<std::wstring> lines;
    for (const auto& [name, hash] : hashes) lines.push_back(name + L"\t" + hash);
    return linesFile(lines);
}

std::vector<PackageEntry> readPackageFiles(const std::string_view utf8) {
    std::vector<PackageEntry> entries;
    for (const std::wstring& line : textLines(utf8)) {
        const std::vector<std::wstring> parts = split(line, L'\t');
        if (parts.size() != 2) continue;
        PackageEntry e{trimmed(parts[0]), trimmed(parts[1])};
        if (e.flatName.find(L'\\') != std::wstring::npos || !isManifestEntry(e.flatName) || !isManifestEntry(e.relative)) {
            continue;
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

std::vector<std::wstring> olderConfigNames() {
    std::vector<std::wstring> names = {L"moza-r3-as-g920.ini", L"moza-r3-as-g29.ini", L"rig-as-g920.ini",
                                       L"rig-as-g29.ini"};
    for (const wchar_t* target : {L"g920", L"g25", L"g27"}) {
        names.push_back(std::wstring(target) + L"-virtual-device.ini");
        names.push_back(std::wstring(target) + L"-in-game-shim.ini");
    }
    return names;
}

std::wstring startMenuNameProblem(const std::wstring_view name) {
    const std::wstring t = trimmed(name);
    if (t.empty()) return L"Give the Start menu folder a name.";
    if (t.size() > 120) return L"The Start menu folder name is too long (at most 120 characters).";
    for (const std::wstring& part : split(t, L'\\')) {
        const std::wstring p = trimmed(part);
        if (p.empty() || std::all_of(p.begin(), p.end(), [](const wchar_t c) { return c == L'.'; })) {
            return L"A Start menu folder name cannot have an empty part, \".\" or \"..\".";
        }
        if (std::any_of(p.begin(), p.end(), [](const wchar_t c) {
                return c < 0x20 || std::wstring_view(L"/:*?\"<>|").find(c) != std::wstring_view::npos;
            })) {
            return L"A Start menu folder name cannot hold / : * ? \" < > |";
        }
    }
    return {};
}

FirefoxPolicy turnOffGamepads(const std::string_view current) {
    FirefoxPolicy policy;
    std::vector<Member> members;
    if (!blank(current)) {
        auto parsed = objectMembers(current);
        if (!parsed) return policy;
        members = std::move(*parsed);
    }
    for (const Member& m : members) {
        if (m.key == kGamepadKey && saysOff(m.rawValue)) {
            policy.result = FirefoxPolicy::Result::AlreadyOff;
            return policy;
        }
    }
    members.erase(std::remove_if(members.begin(), members.end(), [](const Member& m) { return m.key == kGamepadKey; }),
                  members.end());
    members.push_back({std::string(kGamepadKey), "\"" + std::string(kGamepadKey) + "\"",
                       R"({"Value":false,"Status":"locked"})"});
    policy.result = FirefoxPolicy::Result::Changed;
    policy.text = objectText(members);
    return policy;
}

FirefoxPolicy undoGamepads(const std::string_view current) {
    FirefoxPolicy policy;
    auto members = objectMembers(current);
    if (!members) return policy;
    const auto ours = std::find_if(members->begin(), members->end(),
                                   [](const Member& m) { return m.key == kGamepadKey && saysOff(m.rawValue); });
    if (ours == members->end()) {
        policy.result = FirefoxPolicy::Result::AlreadyOff;
        return policy;
    }
    members->erase(ours);
    policy.result = FirefoxPolicy::Result::Changed;
    if (!members->empty()) policy.text = objectText(*members);
    return policy;
}

const std::vector<OemEntry>& oemEntries() {
    static const std::vector<OemEntry> entries = {
        {L"VID_046D&PID_C299", L"Logitech G25 Racing Wheel USB"},
        {L"VID_046D&PID_C29B", L"Logitech G27 Racing Wheel USB"},
        {L"VID_046D&PID_C262", L"Logitech G920 Driving Force Racing Wheel USB"},
        {L"VID_046D&PID_C26E", L"Logitech G923 Racing Wheel for Xbox One and PC"},
    };
    return entries;
}

std::vector<ShimGame> readShimGames(const std::string_view utf8) {
    std::vector<ShimGame> games;
    for (const std::wstring& line : split(fromUtf8(withoutBom(utf8)), L'\n')) {
        std::wstring l = line;
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        const std::vector<std::wstring> parts = split(l, L'\t');
        if (parts.size() != 3 || parts[0] != L"game" || parts[2].empty()) continue;
        ShimGame game{parts[1], withoutTrailingSlash(parts[2])};
        const bool known = std::any_of(games.begin(), games.end(),
                                       [&](const ShimGame& g) { return sameName(g.folder, game.folder); });
        if (!known) games.push_back(std::move(game));
    }
    return games;
}

bool isOurShim(const std::string_view bytes) { return bytes.find("wheelbri") != std::string_view::npos; }

bool isAsiLoader(const std::string_view bytes) {
    return bytes.find("IsUltimateASILoader") != std::string_view::npos &&
           bytes.find("IsWheelBriShim") == std::string_view::npos;
}

Options parseOptions(const std::vector<std::wstring>& arguments, std::wstring& problem) {
    Options o;
    problem.clear();
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::wstring& a = arguments[i];
        auto value = [&](std::wstring& into) {
            if (i + 1 >= arguments.size() || arguments[i + 1].empty()) {
                problem = a + L" needs a value.";
                return;
            }
            into = arguments[++i];
        };
        if (a == L"--uninstall") o.uninstall = true;
        else if (a == L"--quiet") o.quiet = true;
        else if (a == L"--temp-copy") o.tempCopy = true;
        else if (a == L"--user-data") value(o.userData);
        else if (a == L"--install-dir") value(o.installDir);
        else if (a == L"--start-menu-folder") value(o.startMenuFolder);
        else if (a == L"--no-desktop-shortcut") o.desktopShortcut = false;
        else if (a == L"--stop-browser-gamepads") o.stopBrowserGamepads = true;
        else if (a == L"--restart") {
            std::wstring when;
            value(when);
            if (when == L"now") o.restartNow = true;
            else if (when != L"later" && problem.empty()) problem = L"--restart takes now or later.";
        }
        else if (a == L"--keep-config") o.keepConfig = true;
        else if (a == L"--remove-settings") o.removeSettings = true;
        else if (a == L"--remove-logs") o.removeLogs = true;
        else if (a == L"--keep-shims") o.keepShims = true;
        else if (a == L"--remove-certificate") o.removeCertificate = true;
        else problem = L"Unknown option: " + a;
        if (!problem.empty()) return o;
    }
    if (o.keepConfig && o.removeSettings) problem = L"--keep-config and --remove-settings exclude each other.";
    else if (!o.uninstall) problem = startMenuNameProblem(o.startMenuFolder);
    return o;
}

std::wstring optionsCommandLine(const Options& o) {
    std::wstring line;
    auto flag = [&](const bool on, const wchar_t* name) {
        if (on) line += std::wstring(line.empty() ? L"" : L" ") + name;
    };
    auto value = [&](const wchar_t* name, const std::wstring& v) {
        if (v.empty()) return;
        // A trailing backslash would escape the closing quote ("D:\" -> "D:\\").
        std::wstring q = v;
        if (q.back() == L'\\') q += L'\\';
        line += std::wstring(line.empty() ? L"" : L" ") + name + L" \"" + q + L"\"";
    };
    flag(o.uninstall, L"--uninstall");
    flag(o.quiet, L"--quiet");
    value(L"--user-data", o.userData);
    value(L"--install-dir", o.installDir);
    if (!o.uninstall) value(L"--start-menu-folder", o.startMenuFolder);
    flag(!o.desktopShortcut, L"--no-desktop-shortcut");
    flag(o.stopBrowserGamepads, L"--stop-browser-gamepads");
    flag(o.restartNow, L"--restart now");
    flag(o.keepConfig, L"--keep-config");
    flag(o.removeSettings, L"--remove-settings");
    flag(o.removeLogs, L"--remove-logs");
    flag(o.keepShims, L"--keep-shims");
    flag(o.removeCertificate, L"--remove-certificate");
    return line;
}

std::wstring sha256Hex(const std::string_view bytes) {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t whole = bytes.size() / 64 * 64;
    for (size_t i = 0; i < whole; i += 64) sha256Block(h, data + i);
    std::array<unsigned char, 128> tail{};
    const size_t rest = bytes.size() - whole;
    if (rest > 0) std::memcpy(tail.data(), data + whole, rest);
    tail[rest] = 0x80;
    const size_t tailSize = rest < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (int k = 0; k < 8; ++k) tail[tailSize - 1 - k] = static_cast<unsigned char>(bits >> (8 * k));
    for (size_t i = 0; i < tailSize; i += 64) sha256Block(h, tail.data() + i);
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring out;
    for (const std::uint32_t word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(kHex[(word >> shift) & 0xF]);
    }
    return out;
}

}  // namespace wheelbri::setup
