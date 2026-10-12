#include "DiConvert.hpp"

#include <array>
#include <utility>

namespace wheelbri::shim {

namespace {

const std::array<std::pair<ObjectGuid, const GUID*>, 10>& knownGuids() {
    static const std::array<std::pair<ObjectGuid, const GUID*>, 10> kGuids = {{
        {ObjectGuid::XAxis, &GUID_XAxis},
        {ObjectGuid::YAxis, &GUID_YAxis},
        {ObjectGuid::ZAxis, &GUID_ZAxis},
        {ObjectGuid::RxAxis, &GUID_RxAxis},
        {ObjectGuid::RyAxis, &GUID_RyAxis},
        {ObjectGuid::RzAxis, &GUID_RzAxis},
        {ObjectGuid::Slider, &GUID_Slider},
        {ObjectGuid::Button, &GUID_Button},
        {ObjectGuid::Key, &GUID_Key},
        {ObjectGuid::Pov, &GUID_POV},
    }};
    return kGuids;
}

}  // namespace

ObjectGuid objectGuidOf(const GUID* guid) {
    if (guid == nullptr) {
        return ObjectGuid::Any;
    }
    for (const auto& [kind, known] : knownGuids()) {
        if (*guid == *known) {
            return kind;
        }
    }
    return ObjectGuid::Other;
}

const GUID& guidOf(const ObjectGuid guid) {
    for (const auto& [kind, known] : knownGuids()) {
        if (kind == guid) {
            return *known;
        }
    }
    return GUID_Unknown;
}

std::vector<FormatEntry> toEntries(const DIDATAFORMAT& format) {
    std::vector<FormatEntry> entries;
    entries.reserve(format.dwNumObjs);
    for (DWORD i = 0; i < format.dwNumObjs; ++i) {
        const DIOBJECTDATAFORMAT& object = format.rgodf[i];
        entries.push_back({objectGuidOf(object.pguid), object.dwOfs, object.dwType, object.dwFlags});
    }
    return entries;
}

GUID toGuid(const Guid& guid) {
    GUID out{};
    out.Data1 = guid.data1;
    out.Data2 = guid.data2;
    out.Data3 = guid.data3;
    for (std::size_t i = 0; i < 8; ++i) {
        out.Data4[i] = guid.data4[i];
    }
    return out;
}

}  // namespace wheelbri::shim
