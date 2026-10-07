// Adds kFeatureDLSS_RR to the features the game asks Streamline to load. Forza Horizon 6 passes a
// closed featuresToLoad list without DLSS-RR, so sl.dlss_d.dll is never loaded (3 oct 2026:
// slIsFeatureLoaded(kFeatureDLSS_RR) -> eErrorFeatureMissing).
//
// Timing: the game calls slInit milliseconds after loading sl.interposer.dll, and MFG Unlock hooks
// it from its LoadLibrary detour in that same window, so polling loses the race (first attempt was
// 63 ms late). A loader notification (LdrRegisterDllNotification) runs synchronously while
// sl.interposer.dll is being loaded, before LoadLibrary returns to the game or to MFG.
//
// Lifetime: this code lives in the first, temporary instance of the add-on, which ReShade unloads
// ~10 s later, while MFG may stack its Detours hook on top of ours. So the patch never points into
// this module directly: slInit jumps to a small page allocated near it that is never freed. The page
// holds an absolute jump through a pointer (first to slinit_hook, switched to the trampoline once
// slInit returned or the module unloads) and the trampoline itself. Whatever MFG relocates or
// restores afterwards keeps pointing at valid code.

#include "common.hpp"

#include <sl.h>
#include <sl_helpers.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace
{
    using slinit_fn = sl::Result (*)(const sl::Preferences &, uint64_t);

    struct UNICODE_STR { USHORT Length, MaximumLength; PWSTR Buffer; };
    struct DLL_LOADED_DATA { ULONG Flags; const UNICODE_STR *FullDllName; const UNICODE_STR *BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
    using notification_fn = VOID(CALLBACK *)(ULONG reason, const DLL_LOADED_DATA *data, PVOID context);
    using register_fn = LONG(NTAPI *)(ULONG flags, notification_fn callback, PVOID context, PVOID *cookie);
    using unregister_fn = LONG(NTAPI *)(PVOID cookie);

    struct patch_page
    {
        struct { uint8_t code[8]; volatile LONG64 target; } stub;   // FF 25 02 00 00 00 CC CC, then the aligned pointer
        uint8_t trampoline[32];   // original first instruction (or its jump) + jump back
    };

    PVOID g_cookie = nullptr;
    patch_page *g_page = nullptr;
    slinit_fn g_original = nullptr;
    std::atomic<bool> g_done { false };
    std::vector<sl::Feature> g_features;

    void write_abs_jump(uint8_t *at, uint64_t destination)
    {
        at[0] = 0xFF; at[1] = 0x25;
        memset(at + 2, 0, 4);
        memcpy(at + 6, &destination, 8);
    }

    void set_stub_target(uint64_t destination)
    {
        InterlockedExchange64(&g_page->stub.target, LONG64(destination));
    }

    void write_stub(uint64_t destination)
    {
        uint8_t *s = g_page->stub.code;
        s[0] = 0xFF; s[1] = 0x25;
        const int32_t disp = 2;   // rip after the instruction is stub + 6; pointer at stub + 8
        memcpy(s + 2, &disp, 4);
        s[6] = s[7] = 0xCC;
        g_page->stub.target = LONG64(destination);
    }

    // Detaches this module from the call path: slInit keeps jumping to the page, which now goes
    // straight to the trampoline.
    void pass_through()
    {
        if (g_page != nullptr)
            set_stub_target(reinterpret_cast<uint64_t>(g_page->trampoline));
        g_done = true;
    }

    sl::Result slinit_hook(const sl::Preferences &pref, uint64_t sdk_version)
    {
        sl::Preferences p = pref;
        std::string list;
        bool has_rr = false;
        for (uint32_t i = 0; p.featuresToLoad != nullptr && i < p.numFeaturesToLoad; ++i)
        {
            list += std::string(i ? " " : "") + sl::getFeatureAsStr(p.featuresToLoad[i]);
            has_rr |= p.featuresToLoad[i] == sl::kFeatureDLSS_RR;
        }
        std::string action;
        if (p.featuresToLoad == nullptr || p.numFeaturesToLoad == 0)
            action = "no list (all plugins load): unchanged";
        else if (has_rr)
            action = "already contains kFeatureDLSS_RR: unchanged";
        else
        {
            g_features.assign(p.featuresToLoad, p.featuresToLoad + p.numFeaturesToLoad);
            g_features.push_back(sl::kFeatureDLSS_RR);
            p.featuresToLoad = g_features.data();
            p.numFeaturesToLoad = uint32_t(g_features.size());
            action = "kFeatureDLSS_RR added";
        }
        rr::log_info("slInit: featuresToLoad=[" + list + "] flags=" + rr::hex(uint64_t(p.flags)) + " -> " + action);
        const sl::Result r = g_original(p, sdk_version);
        rr::log_info(std::string("slInit returned ") + sl::getResultAsStr(r));
        pass_through();
        return r;
    }

    patch_page *allocate_near(uint8_t *target)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const uint64_t granularity = si.dwAllocationGranularity;
        const uint64_t base = reinterpret_cast<uint64_t>(target) & ~(granularity - 1);
        for (uint64_t offset = granularity; offset < (1ull << 30); offset += granularity)
        {
            for (int sign = -1; sign <= 1; sign += 2)
            {
                const uint64_t address = sign < 0 ? base - offset : base + offset;
                if (void *p = VirtualAlloc(reinterpret_cast<void *>(address), sizeof(patch_page), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                    return static_cast<patch_page *>(p);
            }
        }
        return nullptr;
    }

    void *find_export(uint8_t *base, const char *name)
    {
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.VirtualAddress == 0)
            return nullptr;
        auto *exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
        auto *names = reinterpret_cast<DWORD *>(base + exp->AddressOfNames);
        auto *ordinals = reinterpret_cast<WORD *>(base + exp->AddressOfNameOrdinals);
        auto *functions = reinterpret_cast<DWORD *>(base + exp->AddressOfFunctions);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i)
            if (strcmp(reinterpret_cast<const char *>(base + names[i]), name) == 0)
                return base + functions[ordinals[i]];
        return nullptr;
    }

    void install(uint8_t *target)
    {
        g_page = allocate_near(target);
        if (g_page == nullptr)
        {
            rr::log_warn("slInit hook: no executable page within 1 GB of slInit");
            return;
        }
        memset(g_page, 0xCC, sizeof(patch_page));
        // Trampoline: the first instruction (5-byte mov, or a jump someone placed) then a jump back.
        static const uint8_t expected[5] = { 0x48, 0x89, 0x54, 0x24, 0x10 };   // mov [rsp+10h], rdx (sl.interposer 2.14.1)
        if (memcmp(target, expected, 5) == 0)
        {
            memcpy(g_page->trampoline, target, 5);
            write_abs_jump(g_page->trampoline + 5, reinterpret_cast<uint64_t>(target) + 5);
        }
        else if (target[0] == 0xE9)
        {
            int32_t rel;
            memcpy(&rel, target + 1, 4);
            write_abs_jump(g_page->trampoline, reinterpret_cast<uint64_t>(target) + 5 + rel);
        }
        else
        {
            char bytes[40];
            snprintf(bytes, sizeof(bytes), "%02x %02x %02x %02x %02x %02x", target[0], target[1], target[2], target[3], target[4], target[5]);
            rr::log_warn(std::string("slInit hook: unexpected prologue (") + bytes + "); not hooking");
            return;
        }
        write_stub(reinterpret_cast<uint64_t>(&slinit_hook));
        g_original = reinterpret_cast<slinit_fn>(g_page->trampoline);
        FlushInstructionCache(GetCurrentProcess(), g_page, sizeof(patch_page));

        // jmp rel32 to the stub, written with one aligned 8-byte exchange (slInit is 16-byte aligned).
        const int64_t rel = reinterpret_cast<int64_t>(g_page->stub.code) - (reinterpret_cast<int64_t>(target) + 5);
        DWORD old;
        if ((reinterpret_cast<uintptr_t>(target) & 7) != 0 || !VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old))
        {
            rr::log_warn("slInit hook: cannot patch (alignment or VirtualProtect)");
            return;
        }
        uint8_t patched[8];
        memcpy(patched, target, 8);
        patched[0] = 0xE9;
        const int32_t rel32 = int32_t(rel);
        memcpy(patched + 1, &rel32, 4);
        LONG64 value;
        memcpy(&value, patched, 8);
        InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(target), value);
        VirtualProtect(target, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, 8);
        rr::log_info("slInit hooked from the loader notification for sl.interposer.dll");
    }

    VOID CALLBACK on_dll_notification(ULONG reason, const DLL_LOADED_DATA *data, PVOID)
    {
        if (reason != 1 || g_page != nullptr || data == nullptr || data->BaseDllName == nullptr)   // 1 = loaded
            return;
        const UNICODE_STR &name = *data->BaseDllName;
        static const wchar_t wanted[] = L"sl.interposer.dll";
        if (name.Length != (sizeof(wanted) - sizeof(wchar_t)) || _wcsnicmp(name.Buffer, wanted, name.Length / sizeof(wchar_t)) != 0)
            return;
        if (auto *target = static_cast<uint8_t *>(find_export(static_cast<uint8_t *>(data->DllBase), "slInit")))
            install(target);
    }
}

namespace rr
{
    // Called from DllMain (attach). Only acts if Streamline is not loaded yet.
    void start_slinit_watcher()
    {
        if (GetModuleHandleW(L"sl.interposer.dll") != nullptr)
            return;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto reg = reinterpret_cast<register_fn>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        if (reg == nullptr || reg(0, on_dll_notification, nullptr, &g_cookie) != 0)
            g_cookie = nullptr;
    }

    // Called from DllMain (detach): nothing may keep pointing into this module afterwards.
    void stop_slinit_watcher()
    {
        if (g_cookie != nullptr)
        {
            auto unreg = reinterpret_cast<unregister_fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification"));
            if (unreg != nullptr)
                unreg(g_cookie);
            g_cookie = nullptr;
        }
        pass_through();
    }
}
