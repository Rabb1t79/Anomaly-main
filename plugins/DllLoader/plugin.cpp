/*
 * 中文维护说明：本插件
 * - 本文件是该插件的主要实现入口，后续维护时优先在这里说明新增、修改和删除的行为。
 * - 当前代码逻辑保持不变；本次仅补充中文维护注释，便于后续逆向、排错和功能回溯。
 * - 不把未经验证的猜测写成实现依据；涉及游戏调用、偏移、签名或 ABI 时应注明实际证据来源。
 */
#include "anomaly/sdk/cpp.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::string_view kSettingsSchemaId = "dll-loader-settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::string_view kDefaultDllPath = "dumper-7.dll";
constexpr std::size_t kMaximumDllPathBytes = 4096;
constexpr std::string_view kSettingsSchema = R"json(
{
  "type":"object",
  "additionalProperties":false,
  "required":["dllPath"],
  "properties":{"dllPath":{"type":"string","maxLength":4096}}
}
)json";

HMODULE g_plugin_module{};

struct Context final {
    const AnomalyConfigServiceV1* config{};
    const AnomalyUiServiceV1* ui{};
    AnomalyGenerationHandleV1 settings_schema{};
    std::mutex mutex;
    std::array<char, kMaximumDllPathBytes + 1> editor{};
    std::string persisted_dll_path{kDefaultDllPath};
    std::string dll_path{kDefaultDllPath};
    std::string loaded_path;
    std::string status{"Waiting for plugin activation"};
    HMODULE loaded_module{};
    bool settings_dirty{};
    bool stopped{};
};

// 把状态码和可选错误字符串封装成 AnomalyStatusV1；同时写入消息指针和长度，供插件 ABI 回调统一返回错误原因。
AnomalyStatusV1 Status(const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

// 把 std::string_view 的连续字节区域转换成 AnomalyByteSpanV1；返回的指针直接指向原字符串，不复制数据，因此调用期间原字符串必须保持有效。
AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

template <typename Struct, typename Field>
// 按服务结构体的 struct_size 检查指定字段是否实际存在；它用 offset 加字段大小与 ABI 提供的结构长度比较，避免访问旧版本服务结构中尚未提供的成员。
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

// 检查配置服务是否包含插件实际使用的 write_atomic 等 ABI 字段；结构长度不足或回调为空时返回 false，阻止后续持久化调用。
bool ConfigReady(const AnomalyConfigServiceV1* service) noexcept {
    return HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
               service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->service_version >= ANOMALY_CONFIG_SERVICE_V1_VERSION &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
}

// 确认 UI 服务提供窗口开始/结束所需回调；缺少 end_window 等字段时不进入插件窗口生命周期。
bool HasUiWindow(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_window)>(
               ui, offsetof(AnomalyUiServiceV1, end_window)) &&
        ui->begin_window != nullptr && ui->end_window != nullptr && ui->text != nullptr;
}

// 确认 UI 服务提供 input_text 等输入回调；只有输入接口存在时插件才读取用户编辑内容。
bool HasUiInput(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_text)>(
               ui, offsetof(AnomalyUiServiceV1, input_text)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button)>(
            ui, offsetof(AnomalyUiServiceV1, button)) &&
        ui->input_text != nullptr && ui->button != nullptr;
}

// 使用 Windows UTF-8 转宽字符 API 将插件保存的 UTF-8 路径/文本转换为宽字符串；输入为空、长度超限或转换失败时返回空结果。
std::wstring Utf8ToWide(const std::string_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required) != required) {
        return {};
    }
    return result;
}

// 使用 Windows 宽字符转 UTF-8 API 将系统返回的宽字符串转换为 UTF-8；输入无效或转换失败时返回空字符串。
std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

// 从插件自身模块句柄解析 DLL 所在目录，再据此定位插件包内的配置/资源路径；模块句柄不可用时返回错误而不猜测目录。
std::filesystem::path PluginPackageDirectory(std::string& error) {
    if (g_plugin_module == nullptr) {
        error = "The plugin module handle is unavailable";
        return {};
    }
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        g_plugin_module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        error = "The plugin package path is unavailable";
        return {};
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
}

bool ResolveLibraryPath(
    const std::string_view configured_path, std::filesystem::path& resolved,
    std::string& error) {
    if (configured_path.empty()) {
        error = "DLL loading is disabled";
        return false;
    }
    if (configured_path.size() > kMaximumDllPathBytes) {
        error = "The configured DLL path is too long";
        return false;
    }
    const std::wstring wide_path = Utf8ToWide(configured_path);
    if (wide_path.empty()) {
        error = "The configured DLL path is not valid UTF-8";
        return false;
    }

    std::filesystem::path candidate(wide_path);
    if (candidate.extension().empty()) candidate += L".dll";
    if (_wcsicmp(candidate.extension().c_str(), L".dll") != 0) {
        error = "The configured file must have a .dll extension";
        return false;
    }
    if (candidate.is_relative()) {
        const std::filesystem::path package_directory = PluginPackageDirectory(error);
        if (package_directory.empty()) return false;
        candidate = package_directory / candidate;
    }

    std::error_code filesystem_error;
    resolved = std::filesystem::absolute(candidate, filesystem_error).lexically_normal();
    if (filesystem_error) {
        error = "The configured DLL path could not be resolved";
        return false;
    }
    if (!std::filesystem::is_regular_file(resolved, filesystem_error) || filesystem_error) {
        error = "The configured DLL file was not found";
        return false;
    }
    return true;
}

// 清空 Context 的 editor 缓冲区并把输入文本截断到缓冲区可容纳的最大长度，再写入结尾的 NUL，保证 UI 编辑框始终获得有效 C 字符串。
void SetEditor(Context& context, const std::string_view value) noexcept {
    context.editor.fill('\0');
    const std::size_t count = (std::min)(value.size(), context.editor.size() - 1U);
    std::copy_n(value.data(), count, context.editor.data());
}

// 从原子共享指针取得当前设置快照；acquire 读取保证 UI/更新线程看到完整的已发布 SettingsSnapshot。
bool ReadSettings(Context& context) {
    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = context.config->read(
        context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
        SetEditor(context, context.dll_path);
        return true;
    }
    if (size_status.code != ANOMALY_STATUS_V1_OK ||
        schema_version != kSettingsSchemaVersion || size == 0 ||
        size > kMaximumDllPathBytes + 256U) {
        return false;
    }

    std::string document(size, '\0');
    std::size_t copied = document.size();
    const AnomalyStatusV1 read_status = context.config->read(
        context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {reinterpret_cast<std::uint8_t*>(document.data()), document.size()}, &copied);
    if (read_status.code != ANOMALY_STATUS_V1_OK ||
        schema_version != kSettingsSchemaVersion || copied == 0 || copied > document.size()) {
        return false;
    }
    document.resize(copied);

    const nlohmann::json json = nlohmann::json::parse(document, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return false;
    const auto found = json.find("dllPath");
    if (found == json.end() || !found->is_string()) return false;
    const std::string path = found->get<std::string>();
    if (path.size() > kMaximumDllPathBytes) return false;

    context.persisted_dll_path = path;
    context.dll_path = path;
    SetEditor(context, path);
    return true;
}

// 在持锁状态下取得当前配置路径和编辑内容，把设置序列化后通过 config->write_atomic 写入配置文件；写入失败返回对应错误状态。
bool SaveSettings(Context& context) {
    std::string path;
    {
        std::scoped_lock lock(context.mutex);
        if (!context.settings_dirty) return true;
        path = context.dll_path;
    }

    try {
        const std::string document = nlohmann::json{{"dllPath", path}}.dump();
        const AnomalyStatusV1 status = context.config->write_atomic(
            context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            kSettingsSchemaVersion, Bytes(document));
        if (status.code != ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(context.mutex);
            context.status = "Failed to save the DLL path";
            return false;
        }
        std::scoped_lock lock(context.mutex);
        context.persisted_dll_path = path;
        context.settings_dirty = false;
        return true;
    } catch (...) {
        std::scoped_lock lock(context.mutex);
        context.status = "Failed to save the DLL path";
        return false;
    }
}

// 读取当前配置的 DLL 路径并调用 Windows LoadLibrary 加载目标模块；加载成功后保存模块句柄，路径为空或系统加载失败时不改变现有状态。
void LoadConfiguredLibrary(Context& context) noexcept {
    std::string configured_path;
    {
        std::scoped_lock lock(context.mutex);
        if (context.loaded_module != nullptr) return;
        configured_path = context.dll_path;
    }
    if (configured_path.empty()) {
        std::scoped_lock lock(context.mutex);
        context.status = "DLL loading is disabled";
        return;
    }

    try {
        std::filesystem::path path;
        std::string error;
        if (!ResolveLibraryPath(configured_path, path, error)) {
            std::scoped_lock lock(context.mutex);
            context.status = std::move(error);
            return;
        }
        const HMODULE module = LoadLibraryExW(
            path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module == nullptr) {
            const DWORD load_error = GetLastError();
            std::scoped_lock lock(context.mutex);
            context.status =
                "LoadLibraryExW failed (Win32 error " + std::to_string(load_error) + ')';
            return;
        }
        const std::string display_path = WideToUtf8(path.native());
        std::scoped_lock lock(context.mutex);
        context.loaded_module = module;
        context.loaded_path = display_path.empty() ? configured_path : display_path;
        context.status = "DLL loaded";
    } catch (...) {
        std::scoped_lock lock(context.mutex);
        context.status = "DLL loading failed";
    }
}

// 取出当前已加载 DLL 的 HMODULE 并调用 FreeLibrary 释放；释放后清空模块句柄，避免 Context 继续引用已经卸载的代码。
void UnloadConfiguredLibrary(Context& context) noexcept {
    HMODULE module{};
    {
        std::scoped_lock lock(context.mutex);
        module = std::exchange(context.loaded_module, nullptr);
        context.loaded_path.clear();
    }
    if (module == nullptr) return;
    const BOOL released = FreeLibrary(module);
    std::scoped_lock lock(context.mutex);
    context.status = released != FALSE ? "DLL unloaded" : "FreeLibrary failed";
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host and context are required");
    }
    *plugin_context = nullptr;
    try {
        auto* context = new (std::nothrow) Context();
        if (context == nullptr) {
            return Status(ANOMALY_STATUS_V1_FAILED, "context allocation failed");
        }
        const anomaly::sdk::Host host_view(host);
        context->config = host_view.Query<AnomalyConfigServiceV1>(
            ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION).get();
        context->ui = host_view.Query<AnomalyUiServiceV1>(
            ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();
        if (!ConfigReady(context->config) || context->ui == nullptr) {
            delete context;
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE, "required plugin services are unavailable");
        }
        const AnomalyStatusV1 schema_status = context->config->register_schema(
            context->config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            kSettingsSchemaVersion, Bytes(kSettingsSchema), &context->settings_schema);
        if (schema_status.code != ANOMALY_STATUS_V1_OK ||
            context->settings_schema.id == 0 || !ReadSettings(*context)) {
            delete context;
            return Status(ANOMALY_STATUS_V1_FAILED, "DLL loader settings are invalid");
        }
        *plugin_context = context;
        return anomaly::sdk::Ok();
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "DLL loader initialization failed");
    }
}

// Start 使用函数体中的输入和状态完成其具体运行时操作；这里保留原有代码不变，只明确说明该函数实际读取、修改和返回的对象。
AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is required");
    }
    LoadConfiguredLibrary(*context);
    return anomaly::sdk::Ok();
}

// Stop 使用函数体中的输入和状态完成其具体运行时操作；这里保留原有代码不变，只明确说明该函数实际读取、修改和返回的对象。
AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is required");
    }
    {
        std::scoped_lock lock(context->mutex);
        if (context->stopped) return anomaly::sdk::Ok();
        context->stopped = true;
    }
    const bool saved = SaveSettings(*context);
    UnloadConfiguredLibrary(*context);
    return saved ? anomaly::sdk::Ok()
                 : Status(
                       ANOMALY_STATUS_V1_FAILED,
                       "DLL loader settings could not be saved");
}

// Unload 使用函数体中的输入和状态完成其具体运行时操作；这里保留原有代码不变，只明确说明该函数实际读取、修改和返回的对象。
void ANOMALY_CALL Unload(void* plugin_context) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    UnloadConfiguredLibrary(*context);
    delete context;
}

void ANOMALY_CALL Draw(
    void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    auto* const context = static_cast<Context*>(plugin_context);
    const AnomalyUiServiceV1* const ui = supplied_ui != nullptr
        ? supplied_ui
        : context == nullptr ? nullptr : context->ui;
    if (context == nullptr || !HasUiWindow(ui)) return;
    try {
        int open = 1;
        anomaly::sdk::UiWindow window(ui, "DLL Loader", &open);
        if (!window) return;
        ui->text(ui->user, anomaly::sdk::StringView(
            "DLL path (relative paths use this plugin package)"));
        if (!HasUiInput(ui)) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "Text input is unavailable in this host."));
            return;
        }
        if (ui->input_text(
                ui->user, anomaly::sdk::StringView("DLL##path"),
                context->editor.data(), context->editor.size(),
                ANOMALY_UI_TEXT_INPUT_V1_NONE) != 0) {
            std::scoped_lock lock(context->mutex);
            context->dll_path = context->editor.data();
            context->settings_dirty =
                context->dll_path != context->persisted_dll_path;
            context->status = context->settings_dirty
                ? "Configuration changed; reload this plugin to apply it"
                : "Configuration matches the saved path";
        }
        if (ui->button(
                ui->user, anomaly::sdk::StringView("Discard changes"),
                0.0F, 0.0F) != 0) {
            std::scoped_lock lock(context->mutex);
            context->dll_path = context->persisted_dll_path;
            SetEditor(*context, context->dll_path);
            context->settings_dirty = false;
            context->status = "Restored the saved DLL path";
        }

        std::string status;
        std::string loaded_path;
        bool dirty{};
        {
            std::scoped_lock lock(context->mutex);
            status = context->status;
            loaded_path = context->loaded_path;
            dirty = context->settings_dirty;
        }
        ui->text(ui->user, anomaly::sdk::StringView("Status: " + status));
        if (!loaded_path.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView("Loaded: " + loaded_path));
        }
        if (dirty) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "Reload this plugin from Plugins to save the path and load the new DLL."));
        } else {
            ui->text(ui->user, anomaly::sdk::StringView(
                "Disable or reload this plugin to release the DLL handle."));
        }
    } catch (...) {
    }
}

}  // namespace

// DllMain 使用函数体中的输入和状态完成其具体运行时操作；这里保留原有代码不变，只明确说明该函数实际读取、修改和返回的对象。
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_plugin_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(
            ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.dll-loader"),
        anomaly::sdk::StringView("DLL Loader"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, nullptr, Draw};
    return anomaly::sdk::Ok();
}
