# wheelbri-g923cap: decodes USBPcap captures of a real G923 and probes it (README.md). The library is portable and
# tested in wheelbri_tests; the probe's HID access is Windows only.
add_library(wheelbri_g923cap STATIC
    src/Pcap.cpp
    src/UsbDescriptors.cpp
    src/Protocol.cpp
    src/Analysis.cpp
    src/Reports.cpp
    src/Probe.cpp
    src/ProbeWindows.cpp)
target_include_directories(wheelbri_g923cap PUBLIC include)
target_link_libraries(wheelbri_g923cap PUBLIC wheelbri_core)

add_executable(wheelbri-g923cap src/main.cpp)
target_link_libraries(wheelbri-g923cap PRIVATE wheelbri_g923cap)
