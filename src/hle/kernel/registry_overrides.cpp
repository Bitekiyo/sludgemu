// Registry functions as seen by the title. Console system software identifies the hardware model
// through one registry value, which the runtime's statically linked code reads directly; we serve
// that key virtually from the console profile. Everything else goes to the host registry.

#include <windows.h>

#include "common/log.h"
#include "core/console_profile.h"
#include "hle/kernel/overrides.h"

static const wchar_t k_oem_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";

// Our handle for the virtual key: the address of an object of ours, never a real registry handle.
static const u8 g_oem_key_tag = 0;
#define OEM_KEY ((HKEY) & g_oem_key_tag)

static bool is_oem_key(HKEY parent, LPCWSTR sub_key) {
    return parent == HKEY_LOCAL_MACHINE && sub_key && _wcsicmp(sub_key, k_oem_key) == 0;
}

static LSTATUS WINAPI hk_RegOpenKeyW(HKEY parent, LPCWSTR sub_key, PHKEY result) {
    if (is_oem_key(parent, sub_key)) {
        *result = OEM_KEY;
        return ERROR_SUCCESS;
    }
    return RegOpenKeyW(parent, sub_key, result);
}

static LSTATUS WINAPI hk_RegOpenKeyExW(HKEY parent, LPCWSTR sub_key, DWORD options, REGSAM sam,
                                       PHKEY result) {
    if (is_oem_key(parent, sub_key)) {
        *result = OEM_KEY;
        return ERROR_SUCCESS;
    }
    return RegOpenKeyExW(parent, sub_key, options, sam, result);
}

static LSTATUS WINAPI hk_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                                          LPDWORD size) {
    if (key != OEM_KEY)
        return RegQueryValueExW(key, name, reserved, type, data, size);
    if (!name || _wcsicmp(name, L"ConsoleMode") != 0)
        return ERROR_FILE_NOT_FOUND;

    DWORD value = console_profile().console_mode;
    static volatile long logged = 0;
    if (!InterlockedExchange(&logged, 1))
        LOG_I("registry", "title identifies the hardware: reporting %s (ConsoleMode 0x%lx)",
              console_profile().name, value);
    if (type)
        *type = REG_DWORD;
    if (!size)
        return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    DWORD capacity = *size;
    *size = sizeof(value);
    if (!data)
        return ERROR_SUCCESS;
    if (capacity < sizeof(value))
        return ERROR_MORE_DATA;
    memcpy(data, &value, sizeof(value));
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI hk_RegCloseKey(HKEY key) {
    if (key == OEM_KEY)
        return ERROR_SUCCESS;
    return RegCloseKey(key);
}

const HleExport g_registry_overrides[] = {
    HLE_EXPORT_AS(hk_RegOpenKeyW, "RegOpenKeyW"),
    HLE_EXPORT_AS(hk_RegOpenKeyExW, "RegOpenKeyExW"),
    HLE_EXPORT_AS(hk_RegQueryValueExW, "RegQueryValueExW"),
    HLE_EXPORT_AS(hk_RegCloseKey, "RegCloseKey"),
};
const u32 g_registry_override_count = ARRAY_COUNT(g_registry_overrides);
