#pragma once

#include "wheelbri/LogFileNames.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::logfiles {

// The file system side of LogFileNames.hpp: which file this run of a program writes.

// The names of the files in `folder` (empty when it cannot be read).
[[nodiscard]] std::vector<std::string> namesIn(const std::filesystem::path& folder);

// This run's file of `stem` (e.g. "wheelbri-service") for `date` in `folder`: made the first time it is asked for
// (with the next free number, created exclusively, so two programs starting at once never share one), then the same
// path for the rest of the run (several sessions of one run append to it). Every stem of one run takes the run's
// number of that date when it is still free there, so the app's log, telemetry-* and ffb-* of one run carry the same
// number. Empty, with `error`, when no file can be made (a missing or read-only folder).
[[nodiscard]] std::filesystem::path runFile(const std::filesystem::path& folder, std::string_view stem,
                                            std::string_view date, std::string_view extension, std::string& error);

// Creates "<stem>-<date>-<NN><extension>" in `folder` with the first number from `firstIndex` up whose name is free
// (exclusively: of programs creating one name at once exactly one succeeds, the others take the next). Empty, with
// `error`, when none could be made; `made` gets the number used. runFile uses it; it does not remember anything.
[[nodiscard]] std::filesystem::path createNumbered(const std::filesystem::path& folder, std::string_view stem,
                                                   std::string_view date, std::string_view extension, int firstIndex,
                                                   std::string& error, int* made = nullptr);

// The newest log of `stem` in `folder` (newestOf); empty when there is none.
[[nodiscard]] std::filesystem::path newestFile(const std::filesystem::path& folder, std::string_view stem);

// Forgets this run's files and numbers (the tests: each starts as a new run).
void forgetRun();

}  // namespace wheelbri::logfiles
