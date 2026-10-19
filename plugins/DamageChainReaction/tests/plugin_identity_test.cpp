#include "anomaly/sdk/plugin.h"

#include <Windows.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace {

std::string NarrowAscii(const wchar_t* value) {
    if (value == nullptr) return {};
    std::string result;
    for (; *value != L'\0'; ++value) {
        if (*value > 0x7F) return {};
        result.push_back(static_cast<char>(*value));
    }
    return result;
}

std::string_view TextView(const AnomalyStringViewV1 value) noexcept {
    if (value.data == nullptr || value.size == 0) return {};
    return {value.data, value.size};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: plugin_identity_test.exe <plugin.dll> <manifest-id> <manifest-version>\n");
        return 2;
    }

    const std::string expected_id = NarrowAscii(argv[2]);
    const std::string expected_version = NarrowAscii(argv[3]);
    if (expected_id.empty() || expected_version.empty()) {
        std::fprintf(stderr, "expected manifest ID/version must be non-empty ASCII strings\n");
        return 2;
    }

    HMODULE image = LoadLibraryW(argv[1]);
    if (image == nullptr) {
        std::fprintf(stderr, "LoadLibraryW failed: Win32 error %lu\n",
            static_cast<unsigned long>(GetLastError()));
        return 3;
    }

    const auto entry = reinterpret_cast<AnomalyPluginEntryV1Fn>(
        GetProcAddress(image, "AnomalyPluginEntryV1"));
    if (entry == nullptr) {
        std::fprintf(stderr, "AnomalyPluginEntryV1 export not found\n");
        FreeLibrary(image);
        return 4;
    }

    AnomalyPluginDescriptorV1 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    const AnomalyStatusV1 status = entry(&descriptor);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        std::fprintf(stderr, "AnomalyPluginEntryV1 returned status %u\n",
            static_cast<unsigned>(status.code));
        FreeLibrary(image);
        return 5;
    }
    if (descriptor.struct_size < sizeof(descriptor)) {
        std::fprintf(stderr, "plugin descriptor struct_size is too small: %u\n",
            static_cast<unsigned>(descriptor.struct_size));
        FreeLibrary(image);
        return 6;
    }

    const std::string_view actual_id = TextView(descriptor.id);
    const std::string_view actual_version = TextView(descriptor.version);
    if (actual_id != expected_id || actual_version != expected_version) {
        std::fprintf(stderr,
            "manifest/descriptor mismatch: manifest=%s@%s descriptor=%.*s@%.*s\n",
            expected_id.c_str(), expected_version.c_str(),
            static_cast<int>(actual_id.size()), actual_id.data() == nullptr ? "" : actual_id.data(),
            static_cast<int>(actual_version.size()), actual_version.data() == nullptr ? "" : actual_version.data());
        FreeLibrary(image);
        return 7;
    }

    std::printf("validated plugin descriptor: %s@%s\n",
        expected_id.c_str(), expected_version.c_str());
    FreeLibrary(image);
    return 0;
}
