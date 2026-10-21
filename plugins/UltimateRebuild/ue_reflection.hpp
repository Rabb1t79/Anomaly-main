// UE 反射遍历（偏移全部来自 5.6.1 CppSDK dump，逐项核对）
//   UObject::Class 0x10 / Name 0x18 / Outer 0x20
//   UStruct::Children 0x48
//   UFunction::FunctionFlags 0xB0
//   GObjects 访问器：mov rax,[global]; mov rcx,[rax+rcx*8]; mov rax,[rcx+rdx*8]; ret
//   FName 匹配：用宿主 anomaly.ue5.names::find_utf8 得到 name_id，与 [obj+0x18] 比较
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace ue {

using GetObjFn = void* (*)(int chunk, int index);

struct Reader {
    const AnomalyUe5NamesServiceV1* names = nullptr;
    GetObjFn get_object = nullptr;
    std::uint32_t object_limit = 1500000;   // 上限保护，避免越界扫描

    static bool SafePtr(std::uintptr_t addr, std::uintptr_t* out) {
        if (addr == 0) return false;
        __try { *out = *reinterpret_cast<const std::uintptr_t*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }
    static bool SafeU32(std::uintptr_t addr, std::uint32_t* out) {
        if (addr == 0) return false;
        __try { *out = *reinterpret_cast<const std::uint32_t*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }
    bool NameId(const char* text, std::uint32_t* out) const {
        if (!names || !names->find_utf8 || !text || !out) return false;
        AnomalyStringViewV1 view{text, std::strlen(text)};
        return names->find_utf8(names->user, view, out).code == ANOMALY_STATUS_V1_OK;
    }
    void* ObjectAt(std::uint32_t index) const {
        if (!get_object) return nullptr;
        // 分块数组：chunk = index / 0x10000，chunk 内下标 = index % 0x10000
        return get_object(static_cast<int>(index >> 16), static_cast<int>(index & 0xFFFF));
    }
    // 读取对象 FName 的 ComparisonIndex
    bool ObjectNameId(void* object, std::uint32_t* out) const {
        return SafeU32(reinterpret_cast<std::uintptr_t>(object) + 0x18, out);
    }
    // 按 name_id 找类对象（调用方负责取得 id）
    void* FindClassById(std::uint32_t want) const {
        if (want == 0) return nullptr;
        for (std::uint32_t i = 0; i < object_limit; ++i) {
            void* obj = ObjectAt(i);
            if (!obj) continue;
            std::uint32_t name_id = 0;
            if (!ObjectNameId(obj, &name_id)) continue;
            if (name_id == want) return obj;
        }
        return nullptr;
    }
    // 按类名找 UClass：遍历对象，比较其 UClass 的 FName
    void* FindClass(const char* class_name) const {
        std::uint32_t want = 0;
        if (!NameId(class_name, &want)) return nullptr;
        for (std::uint32_t i = 0; i < object_limit; ++i) {
            void* obj = ObjectAt(i);
            if (!obj) continue;
            std::uint32_t name_id = 0;
            if (!ObjectNameId(obj, &name_id)) continue;
            if (name_id == want) return obj;
        }
        return nullptr;
    }
    // 在 UClass 的 Children 链（UStruct+0x48）里按函数名找 UFunction
    void* FindFunction(void* uclass, const char* function_name) const {
        std::uint32_t want = 0;
        if (!uclass || !NameId(function_name, &want)) return nullptr;
        std::uintptr_t child = 0;
        if (!SafePtr(reinterpret_cast<std::uintptr_t>(uclass) + 0x48, &child)) return nullptr;
        for (int guard = 0; guard < 4096 && child != 0; ++guard) {
            std::uint32_t name_id = 0;
            if (ObjectNameId(reinterpret_cast<void*>(child), &name_id) && name_id == want)
                return reinterpret_cast<void*>(child);
            std::uintptr_t next = 0;
            if (!SafePtr(child + 0x28, &next)) return nullptr;   // UField::Next
            child = next;
        }
        return nullptr;
    }
};

}  // namespace ue
