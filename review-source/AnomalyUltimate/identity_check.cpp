#include "anomaly/sdk/plugin.h"
#include <Windows.h>
#include <cstdio>
#include <string_view>

std::string_view View(AnomalyStringViewV1 value) {
    if (value.data == nullptr || value.size == 0) return {};
    return {value.data, value.size};
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: anomaly_ultimate_identity_check.exe <plugin.dll>\n");
        return 2;
    }
    HMODULE image = LoadLibraryW(argv[1]);
    if (image == nullptr) {
        std::fprintf(stderr, "LoadLibraryW failed: %lu\n", static_cast<unsigned long>(GetLastError()));
        return 3;
    }
    const bool bridge = GetProcAddress(image, "AnomalyPluginCxxThrowV1") != nullptr;
    const auto entry = reinterpret_cast<AnomalyPluginEntryV1Fn>(
        GetProcAddress(image, ANOMALY_PLUGIN_V1_ENTRY_NAME));
    if (!bridge || entry == nullptr) {
        std::fprintf(stderr, "required export missing: bridge=%d entry=%d\n", bridge, entry != nullptr);
        FreeLibrary(image);
        return 4;
    }
    AnomalyPluginDescriptorV1 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    const AnomalyStatusV1 status = entry(&descriptor);
    if (status.code != ANOMALY_STATUS_V1_OK || descriptor.struct_size < sizeof(descriptor)) {
        std::fprintf(stderr, "descriptor failed: status=%u size=%u\n",
            static_cast<unsigned>(status.code), static_cast<unsigned>(descriptor.struct_size));
        FreeLibrary(image);
        return 5;
    }
    const auto id = View(descriptor.id);
    const auto name = View(descriptor.name);
    const auto version = View(descriptor.version);
    std::printf("plugin id=%.*s name=%.*s version=%.*s\n",
        static_cast<int>(id.size()), id.data(),
        static_cast<int>(name.size()), name.data(),
        static_cast<int>(version.size()), version.data());
    std::printf("exports: AnomalyPluginEntryV1=OK AnomalyPluginCxxThrowV1=OK\n");
    FreeLibrary(image);
    return 0;
}
