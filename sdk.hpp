#pragma once
#include <windows.h>
#include <cstdint>
#include <array>

struct Vector3 { float x, y, z; };
struct Matrix4x4 { float m[16]; };

// Структуры PEB для маскировки модуля
typedef struct _UNICODE_STRING_COMPLETE {
    USHORT Length; USHORT MaximumLength; PWSTR  Buffer;
} UNICODE_STRING_COMPLETE, *PUNICODE_STRING_COMPLETE;

typedef struct _LDR_DATA_TABLE_ENTRY_COMPLETE {
    LIST_ENTRY InLoadOrderLinks; LIST_ENTRY InMemoryOrderLinks; LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase; PVOID EntryPoint; ULONG SizeOfImage;
    UNICODE_STRING_COMPLETE FullDllName; UNICODE_STRING_COMPLETE BaseDllName;
} LDR_DATA_TABLE_ENTRY_COMPLETE, *PLDR_DATA_TABLE_ENTRY_COMPLETE;

typedef struct _PEB_LDR_DATA_COMPLETE {
    ULONG Length; BOOLEAN Initialized; HANDLE SsHandle;
    LIST_ENTRY InLoadOrderModuleList; LIST_ENTRY InMemoryOrderModuleList; LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA_COMPLETE, *PPEB_LDR_DATA_COMPLETE;

typedef struct _PEB_COMPLETE {
    BOOLEAN InheritedAddressSpace; BOOLEAN ReadImageFileExecOptions; BOOLEAN BeingDebugged;
    union {
        BOOLEAN BitField;
        struct { BOOLEAN ImageUsesLargePages : 1; } Flags;
    } CrossProcessFlags;
    HANDLE Mutant; PVOID ImageBaseAddress; PPEB_LDR_DATA_COMPLETE Ldr;
} PEB_COMPLETE, *PPEB_COMPLETE;

// Структуры Schema System
struct SchemaClassFieldData_t {
    const char* m_pszName;
    char pad_0x08[8];
    uint32_t m_nOffset;
    char pad_0x14[4];
};

struct SchemaClassBinding_t {
    char pad_0x00[8];
    const char* m_pszName;
    char pad_0x10[8];
    int16_t m_nFieldCount;
    char pad_0x1A[6];
    SchemaClassFieldData_t* m_pFields;
};

// === КОМПИЛЯТОРНЫЙ XOR С ГАРАНТИРОВАННЫМ СТИРАНИЕМ ИЗ RAM ===
constexpr char CryptKey() {
    return static_cast<char>((__TIME__[7] * 13) + (__TIME__[4] * 7) + 0x5A);
}

template <std::size_t N>
class XorStr {
private:
    std::array<char, N> encrypted_{};
public:
    constexpr XorStr(const char (&str)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            encrypted_[i] = static_cast<char>(str[i] ^ CryptKey());
        }
    }

    // Временный контейнер, который живет только внутри круглых скобок вызова функции
    struct Storage {
        std::array<char, N> data;
        ~Storage() { SecureZeroMemory(data.data(), N); } // Стирает строку из ОЗУ сразу после использования
    };

    Storage decrypt() const {
        Storage result{};
        for (std::size_t i = 0; i < N; ++i) {
            result.data[i] = static_cast<char>(encrypted_[i] ^ CryptKey());
        }
        return result;
    }
};

// Макрос автоматически управляет временем жизни временного объекта
#define E_STR(str) ([]() { constexpr XorStr<sizeof(str)> value(str); return value.decrypt(); }()).data.data()
