#pragma once
#include "cwsdk.h"
#include <cstring>
#include <cstdint>
#include "FieldBossRules.h"
namespace fieldglow {
// Native light vector entries are 48 bytes. Only the RGB is changed.
struct Light {
    int64_t position[3];
    float radius, color[3], distanceSquared;
    uint32_t padding;
};
static_assert(sizeof(Light) == 48 && offsetof(Light, color) == 28);
static bool installed = false;
static void (*append)(void *, const Light *) = nullptr;
static void glow(void *collector, const Light *input) {
    // This call site is exclusively the native named-boss light branch.
    // Apply the same hue to native and promoted bosses across reloads.
    Light tinted = *input;
    fieldboss::purple(tinted.color);
    append(collector, &tinted);
}
inline bool initialize() {
    if (installed)
        return true;
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    auto site = (unsigned char *)(base + 0xd1882);
    int32_t original = 0;
    memcpy(&original, site + 1, 4);
    // Verified named-boss light append call. Fail closed if another mod owns it.
    if (site[0] != 0xe8 || (uintptr_t)(site + 5) + original != base + 0xe2bd0)
        return false;
    append = (void (*)(void *, const Light *))(base + 0xe2bd0);
    unsigned char *relay = nullptr;
    for (uintptr_t offset = 0x10000; offset < 0x70000000 && !relay; offset += 0x10000) {
        for (int direction : {1, -1}) {
            auto address = direction > 0 ? base + offset : base - offset;
            relay = (unsigned char *)VirtualAlloc((void *)address, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                                  PAGE_READWRITE);
            if (relay)
                break;
        }
    }
    if (!relay)
        return false;
    const unsigned char code[] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(relay, code, sizeof(code));
    auto target = (uintptr_t)&glow;
    memcpy(relay + 6, &target, 8);
    DWORD old;
    if (!VirtualProtect(relay, 0x1000, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, 14);
    intptr_t distance = (intptr_t)relay - (intptr_t)(site + 5);
    if (distance < INT32_MIN || distance > INT32_MAX ||
        !VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    unsigned char call[5] = {0xe8};
    int32_t rel = (int32_t)distance;
    memcpy(call + 1, &rel, 4);
    memcpy(site, call, 5);
    DWORD ignored;
    VirtualProtect(site, 5, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    installed = true;
    return true;
}
} // namespace fieldglow
