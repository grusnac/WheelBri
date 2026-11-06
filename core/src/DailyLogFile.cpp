// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/DailyLogFile.hpp"

#include <utility>

namespace wheelbri {

DailyLogFile::DailyLogFile(const Settings settings, NoteLine noteLine)
    : settings_(settings), noteLine_(std::move(noteLine)) {}

DailyLogFile::~DailyLogFile() {
    if (open_) target_->close();
}

void DailyLogFile::attach(std::unique_ptr<LogTarget> target, const std::string& date, const std::uint64_t nowMs,
                          const std::string& where) {
    if (target != nullptr && target_ != nullptr && !where.empty() && where == where_) {
        // The same place again: the current file goes on (`target` is dropped, closing nothing it had not opened).
        // One that could not be opened is tried again now, as a new target would have been.
        if (!open_) tryOpen(date, nowMs);
        return;
    }
    if (open_) target_->close();
    where_ = target != nullptr ? where : std::string();
    open_ = false;
    fileDate_.clear();
    target_ = std::move(target);
    if (target_ != nullptr) tryOpen(date, nowMs);
}

void DailyLogFile::write(std::string line, const std::string& date, const std::uint64_t nowMs) {
    if (open_ && date != fileDate_) {
        target_->close();   // a new day: its own file, at once
        open_ = false;
        tryOpen(date, nowMs);
    } else if (!open_ && target_ != nullptr && (nowMs < lastTryMs_ || nowMs - lastTryMs_ >= settings_.retryMs)) {
        tryOpen(date, nowMs);
    }
    if (open_) {
        if (target_->write(line)) return;
        lost(nowMs);
    }
    keep(std::move(line));
}

void DailyLogFile::tryOpen(const std::string& date, const std::uint64_t nowMs) {
    lastTryMs_ = nowMs;
    open_ = target_->open(date);
    if (!open_) return;
    fileDate_ = date;
    if (!writeWaiting()) lost(nowMs);
}

bool DailyLogFile::writeWaiting() {
    if (dropped_ > 0) {
        if (noteLine_) {
            const std::string note =
                noteLine_(std::to_string(dropped_) + " earlier lines were dropped while this log had no file");
            if (!target_->write(note)) return false;
        }
        dropped_ = 0;
    }
    while (!pending_.empty()) {
        if (!target_->write(pending_.front())) return false;
        pending_.pop_front();
    }
    return true;
}

void DailyLogFile::lost(const std::uint64_t nowMs) {
    target_->close();
    open_ = false;
    lastTryMs_ = nowMs;
}

void DailyLogFile::keep(std::string line) {
    if (settings_.maxPending == 0) {
        ++dropped_;
        return;
    }
    if (pending_.size() >= settings_.maxPending) {
        pending_.pop_front();
        ++dropped_;
    }
    pending_.push_back(std::move(line));
}

}  // namespace wheelbri
