// wheelbri-g923cap: decodes USBPcap captures of a real Logitech G923 and probes it (README.md in this folder).
//
//   wheelbri-g923cap decode <session or day folder> [--address N] [--quiet]
//                                       summary.md per session (and stream.tsv), report.md for a day; exit 1 when a
//                                       check fails
//   wheelbri-g923cap locate <capture>...   "found\t<file>\t<bus>\t<address>\t<product hex>" per wheel found
//   wheelbri-g923cap keep <session folder> only the wheel's frames from raw-*.pcap into capture.pcap
//   wheelbri-g923cap probe --out <folder> [--ghub running|closed]   read-only questions to the wheel (Windows)
//
// G923_TARGET=VID[:PID] (hex): a rehearsal with another wheel.

#include "g923cap/Probe.hpp"
#include "g923cap/Reports.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int usage() {
    std::cout << "usage:\n"
                 "  wheelbri-g923cap decode <folder> [--address N] [--quiet]\n"
                 "  wheelbri-g923cap locate <capture>...\n"
                 "  wheelbri-g923cap keep <folder>\n"
                 "  wheelbri-g923cap probe --out <folder> [--ghub running|closed]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace wheelbri::g923cap;
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const std::string& command = args[0];
    if (command == "decode") {
        std::filesystem::path folder;
        std::optional<int> address;
        bool quiet = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--address" && i + 1 < args.size()) address = std::stoi(args[++i]);
            else if (args[i] == "--quiet") quiet = true;
            else if (args[i] == "--repo" && i + 1 < args.size()) ++i;   // accepted, not needed: values are built in
            else folder = args[i];
        }
        if (folder.empty() || !std::filesystem::is_directory(folder)) {
            std::cout << "no folder " << folder.string() << "\n";
            return 2;
        }
        const auto results = decode(folder, address);
        if (results.empty()) {
            std::cout << "no sessions found\n";
            return 2;
        }
        int failed = 0;
        for (const auto& r : results) {
            if (!quiet) std::cout << "== " << r.session << "\n";
            for (const auto& [ok, text] : checksOf(r)) {
                failed += ok ? 0 : 1;
                if (!quiet) std::cout << "  " << (ok ? "PASS" : "FAIL") << "  " << text << "\n";
            }
        }
        return failed != 0 ? 1 : 0;
    }
    if (command == "locate") {
        std::vector<std::filesystem::path> files(args.begin() + 1, args.end());
        if (files.empty()) return usage();
        const auto found = locate(files);
        for (const Located& l : found) {
            char product[8];
            std::snprintf(product, sizeof(product), "%04X", l.product);
            std::cout << "found\t" << l.file.string() << "\t" << l.bus << "\t" << l.address << "\t" << product << "\n";
        }
        return found.empty() ? 1 : 0;
    }
    if (command == "keep") {
        if (args.size() != 2) return usage();
        bool kept = false;
        for (const auto& line : keepWheelOnly(args[1])) {
            std::cout << line << "\n";
            kept = kept || line.rfind("kept", 0) == 0;
        }
        return kept ? 0 : 1;
    }
    if (command == "probe") {
        std::string out;
        std::string ghub = "unknown";
        for (std::size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == "--out") out = args[++i];
            else if (args[i] == "--ghub") ghub = args[++i];
        }
        if (out.empty()) return usage();
        return runProbe(out, ghub);
    }
    return usage();
}
