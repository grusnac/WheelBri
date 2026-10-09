#include "wheelbri/PedalLayout.hpp"

namespace wheelbri {

std::string_view pedalLayoutName(const PedalLayout layout) {
    switch (layout) {
        case PedalLayout::Logitech: return "Logitech G920";
        case PedalLayout::Nfs2015: return "NFS 2015";
    }
    return "Logitech G920";
}

TargetReport applyPedalLayout(const TargetReport& report, const PedalLayout layout) {
    if (layout != PedalLayout::Nfs2015) {
        return report;
    }
    // The G920 packs throttle on Y, brake on Rz and clutch on the Slider; NFS 2015 reads brake Y, clutch Rz and
    // throttle Slider (2026-10-09: with the real layout its throttle was the brake, its clutch the throttle).
    TargetReport moved = report;
    moved.throttle = report.brake;    // Y
    moved.brake = report.clutch;      // Rz
    moved.clutch = report.throttle;   // Slider
    return moved;
}

}  // namespace wheelbri
