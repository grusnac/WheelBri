#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::helper {

// What the app (wheelbri.exe, running as the user) and the WheelBri Helper (wheelbri-helper.exe, a Windows service
// running as SYSTEM) say to each other over a named pipe. The helper does the few steps that need Administrator:
// a fixed list of requests, each checked here before anything is done, and nothing else. Portable and unit-tested;
// the Windows side (pipe, caller checks, the steps themselves) lives in the service.
//
// Safety, kept by the helper whatever the request says:
// - HideRig always names the caller's own process (the pipe tells the helper who it is), never one from the request.
// - PutHideFilter only takes HID interface instance ids, never the virtual G920 (046D:C262); the helper checks again
//   that each is present and a game controller before it puts the filter on.
// - No paths, no file names, no command lines: the helper reads none of the user's files and runs only its own
//   scripts from its install folder.
// - When the pipe closes (Stop, crash, logoff) the helper removes the virtual G920 and shows the rig again.

// \\.\pipe\wheelbri-helper
inline constexpr std::string_view kPipeName = R"(\\.\pipe\wheelbri-helper)";
inline constexpr std::uint16_t kVersion = 1;

inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kMaxMessage = 8192;   // either way; anything longer is refused unread
inline constexpr std::size_t kMaxDevices = 16;     // interfaces in one PutHideFilter
inline constexpr std::size_t kMaxIdLength = 200;   // one instance id
inline constexpr std::size_t kMaxText = 2000;      // a reply's text (cut at a UTF-8 boundary when longer)

enum class RequestType : std::uint8_t {
    Hello = 1,              // first on every connection: the reply's text is the helper's version
    CreateVirtualG920 = 2,  // the wheelbri\G920 software device (and the DirectInput entries), for this connection
    RemoveVirtualG920 = 3,
    HideRig = 4,            // only the caller's process may open the filtered devices (AllowedPid = caller)
    ShowRig = 5,            // clears that again
    PutHideFilter = 6,      // devices: the HID interfaces to put wheelbri-hide on (then waits for it to start)
    InstallG920Driver = 7,  // when Windows' driver store has none
    ShareLogFolder = 8,     // lets the drivers write into the caller's own log folder
};

enum class Status : std::uint8_t {
    Ok = 0,
    Refused = 1,      // the request broke a rule (text says which); nothing was done
    Failed = 2,       // tried and failed (text says why)
    Pending = 3,      // done as far as possible now (the filter is on but not started yet; text says what to do)
    Unsupported = 4,  // a request type this helper does not know (an older helper)
};

struct Request {
    RequestType type = RequestType::Hello;
    std::uint32_t seq = 0;              // echoed in the reply
    std::vector<std::string> devices;   // PutHideFilter only
};

struct Reply {
    std::uint32_t seq = 0;
    Status status = Status::Ok;
    std::string text;
};

// Layout, little-endian. Header (12 bytes): 0 u32 magic ("WEHQ" request, "WEHR" reply), 4 u16 version,
// 6 u8 type (request) or status (reply), 7 u8 reserved 0, 8 u32 seq.
// Request body: u8 device count, then per device u16 length + that many bytes (ASCII).
// Reply body: u16 length + that many bytes of UTF-8 text.
inline constexpr std::uint32_t kRequestMagic = 0x51484557;   // "WEHQ"
inline constexpr std::uint32_t kReplyMagic = 0x52484557;     // "WEHR"

[[nodiscard]] std::vector<std::uint8_t> encode(const Request& request);
[[nodiscard]] std::vector<std::uint8_t> encode(const Reply& reply);   // text cut to kMaxText
// Nullopt with `error` set when the bytes are not exactly one well-formed message of this version. A request of an
// unknown type decodes (so the helper can answer Unsupported); requestProblem says so.
[[nodiscard]] std::optional<Request> decodeRequest(std::span<const std::uint8_t> bytes, std::string& error);
[[nodiscard]] std::optional<Reply> decodeReply(std::span<const std::uint8_t> bytes, std::string& error);

// Empty when the request may be carried out, else why not (the helper answers Refused with it, or Unsupported for
// an unknown type: isKnown).
[[nodiscard]] std::string requestProblem(const Request& request);
[[nodiscard]] bool isKnown(RequestType type);

// Empty when `id` is a HID interface instance id the hide filter may go on: "HID\VID_xxxx&PID_xxxx...\<instance>",
// at most kMaxIdLength characters of [A-Za-z0-9&_.#{}-] and backslashes, exactly three parts, no empty part, and not
// the virtual G920.
[[nodiscard]] std::string hideDeviceProblem(std::string_view id);

// Whether `image` (the caller's program, as Windows names it) is wheelbri.exe (the app) or wheelbri-service.exe (the
// same sessions from a console) in `installFolder`, ignoring case,
// '/' or '\', a trailing separator and a "\\?\" prefix. False for any ".." or "." part, or an empty folder.
[[nodiscard]] bool isInstalledApp(std::string_view image, std::string_view installFolder);

// Whether `image` is WheelBri's own program (wheelbri.exe or wheelbri-service.exe) in any folder, ignoring case and
// '/' or '\'. Not a check of trust (anyone may name a program so): only whether the helper's log may follow it
// (HelperDispatch.hpp mayTakeTheLog).
[[nodiscard]] bool isWheelBriProgram(std::string_view image);

// Encoded, `request` fits in one message (kMaxMessage); a longer one is refused unread by the helper.
[[nodiscard]] bool fitsInOneMessage(const Request& request);

[[nodiscard]] const char* requestName(RequestType type);
[[nodiscard]] const char* statusName(Status status);

}  // namespace wheelbri::helper
