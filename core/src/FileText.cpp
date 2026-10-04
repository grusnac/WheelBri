#include "wheelbri/FileText.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wheelbri {

std::string pathToUtf8(const std::filesystem::path& path) {
    try {
        const std::u8string text = path.u8string();
        return {text.begin(), text.end()};
    } catch (const std::exception&) {
        return {};   // e.g. an unpaired UTF-16 surrogate in a Windows name
    }
}

namespace {

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF, no cut sequences.
bool validUtf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            length = 2;
            code = c & 0x1FU;
        } else if (c >= 0xE0 && c <= 0xEF) {
            length = 3;
            code = c & 0x0FU;
        } else if (c >= 0xF0 && c <= 0xF4) {
            length = 4;
            code = c & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) return false;
            code = (code << 6) | (next & 0x3FU);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

}  // namespace

std::filesystem::path pathFromUtf8(const std::string& text) {
    if (!validUtf8(text)) return {};   // a file edited by hand and saved in another code page
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception&) {
        return {};   // not valid UTF-8 (a file edited by hand and saved in another code page)
    }
}

std::optional<std::string> readFileText(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;   // missing, or a folder or device
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) return std::nullopt;
    return text.str();
}

bool writeFileReplacing(const std::filesystem::path& path, const std::string_view text) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return false;   // never put a file in place of a folder
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::filesystem::remove(temporary, ec);   // left over from an earlier attempt (or a link: removed, not followed)
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        // Some systems do not replace an existing file on rename: take the old one away first.
        std::error_code removeEc;
        std::filesystem::remove(path, removeEc);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            std::filesystem::remove(temporary, removeEc);
            return false;
        }
    }
    return true;
}

void forEachEntry(const std::filesystem::path& folder,
                  const std::function<void(const std::filesystem::directory_entry&)>& visit) {
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return;
    std::filesystem::directory_iterator it(folder, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        try {
            visit(*it);
        } catch (const std::exception&) {
            // a name that cannot be converted (non-UTF-8 on Windows): skip that entry
        }
        it.increment(ec);
    }
}

std::string truncateUtf8(std::string text, const std::size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    // Step back over continuation bytes (10xxxxxx) to the start of the character that would be split.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
    text.resize(cut);
    return text;
}

}  // namespace wheelbri
