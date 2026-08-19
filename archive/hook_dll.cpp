// hook_dll.cpp - DLL that hooks orlib20 functions and logs calls
// Build: cmake --build build --config Release --target hook_dll

#include <cstdio>
#include <cstring>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

static const char* LOG_PATH = "tmp\\re_artifacts\\dll_hook_log.txt";
static CRITICAL_SECTION g_logLock;
static bool g_initialized = false;

// Original bytes for each hooked function
struct HookInfo {
    const char* name;
    void* addr;
    unsigned char origBytes[16];
    int patchSize;  // usually 5 for JMP
    bool installed;
};

static HookInfo g_hooks[16];
static int g_numHooks = 0;

static void logf(const char* fmt, ...) {
    EnterCriticalSection(&g_logLock);
    FILE* f = fopen(LOG_PATH, "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fflush(f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

// Find orlib20.dll base address
static HMODULE findOrlib() {
    HMODULE hMods[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
        for (DWORD i = 0; i < cbNeeded / sizeof(HMODULE); i++) {
            char modName[MAX_PATH];
            if (GetModuleBaseNameA(GetCurrentProcess(), hMods[i], modName, MAX_PATH)) {
                if (_stricmp(modName, "orlib20.dll") == 0) return hMods[i];
            }
        }
    }
    return NULL;
}

// Hook installation: patch first bytes with JMP to handler
static bool installHook(HookInfo* h, void* handler) {
    DWORD oldProtect;
    if (!VirtualProtect(h->addr, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    // Save original bytes
    memcpy(h->origBytes, h->addr, 16);
    // Write JMP (E9 rel32)
    unsigned char jmp[5];
    jmp[0] = 0xE9;
    int rel = (int)handler - (int)h->addr - 5;
    memcpy(jmp + 1, &rel, 4);
    memcpy(h->addr, jmp, 5);
    h->patchSize = 5;
    h->installed = true;
    DWORD dummy;
    VirtualProtect(h->addr, 16, oldProtect, &dummy);
    return true;
}

// Unhook (restore original bytes)
static void unhook(HookInfo* h) {
    if (!h->installed) return;
    DWORD oldProtect;
    VirtualProtect(h->addr, 16, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(h->addr, h->origBytes, h->patchSize);
    DWORD dummy;
    VirtualProtect(h->addr, 16, oldProtect, &dummy);
    h->installed = false;
}

// Rehook
static void rehook(HookInfo* h, void* handler) {
    if (h->installed) return;
    DWORD oldProtect;
    VirtualProtect(h->addr, 16, PAGE_EXECUTE_READWRITE, &oldProtect);
    unsigned char jmp[5];
    jmp[0] = 0xE9;
    int rel = (int)handler - (int)h->addr - 5;
    memcpy(jmp + 1, &rel, 4);
    memcpy(h->addr, jmp, 5);
    h->installed = true;
    DWORD dummy;
    VirtualProtect(h->addr, 16, oldProtect, &dummy);
}

// Trampolines for each hooked function
static unsigned char g_trampoline[16][32];  // trampoline space for each hook

static void buildTrampoline(HookInfo* h, int idx) {
    // Copy original bytes to trampoline
    memcpy(g_trampoline[idx], h->origBytes, h->patchSize);
    // Add JMP back to original+patchSize
    g_trampoline[idx][h->patchSize] = 0xE9;
    int rel = (int)h->addr + h->patchSize - (int)&g_trampoline[idx][h->patchSize + 1] - 4;
    memcpy(g_trampoline[idx] + h->patchSize + 1, &rel, 4);
    // Make trampoline executable
    DWORD oldProtect;
    VirtualProtect(g_trampoline[idx], 32, PAGE_EXECUTE_READWRITE, &oldProtect);
}

// Hook handlers - each is a __stdcall function with the right signature

// VerifyModule_New handler
typedef int (__stdcall *VerifyModule_New_t)(int, int, int, int, void*);
static VerifyModule_New_t g_origVerify = NULL;

static int __stdcall hook_VerifyModule_New(int ctx, int whType, int progId, int isMatch, void* compileCtx) {
    logf("[VerifyModule_New] ctx=%d WHType=%d ProgId=%d isMatch=%d compileCtx=0x%p\n",
        ctx, whType, progId, isMatch, compileCtx);

    // Unhook, call original, rehook
    unhook(&g_hooks[0]);
    int ret = ((VerifyModule_New_t)g_hooks[0].addr)(ctx, whType, progId, isMatch, compileCtx);
    rehook(&g_hooks[0], (void*)hook_VerifyModule_New);
    // Note: rehook writes JMP to hook_VerifyModule_New, but we're currently
    // executing in hook_VerifyModule_New, so this is fine - the JMP will
    // be in place for the NEXT call.

    logf("[VerifyModule_New] returned %d\n", ret);
    return ret;
}

// StartCompilingProcess handler
typedef void (__stdcall *StartCompilingProcess_t)(void);
static void __stdcall hook_StartCompilingProcess() {
    logf("[StartCompilingProcess]\n");
    unhook(&g_hooks[1]);
    ((StartCompilingProcess_t)g_hooks[1].addr)();
    rehook(&g_hooks[1], (void*)hook_StartCompilingProcess);
}

// EndCompilingProcess handler
typedef void (__stdcall *EndCompilingProcess_t)(void);
static void __stdcall hook_EndCompilingProcess() {
    logf("[EndCompilingProcess]\n");
    unhook(&g_hooks[2]);
    ((EndCompilingProcess_t)g_hooks[2].addr)();
    rehook(&g_hooks[2], (void*)hook_EndCompilingProcess);
}

// SaveModule_New handler
typedef int (__stdcall *SaveModule_New_t)(int, int, void*, HWND, void*);
static int __stdcall hook_SaveModule_New(int whType, int progId, void* arg3, HWND hwnd, void* buf) {
    logf("[SaveModule_New] WHType=%d ProgId=%d arg3=0x%p hwnd=0x%p buf=0x%p\n",
        whType, progId, arg3, hwnd, buf);
    unhook(&g_hooks[3]);
    int ret = ((SaveModule_New_t)g_hooks[3].addr)(whType, progId, arg3, hwnd, buf);
    rehook(&g_hooks[3], (void*)hook_SaveModule_New);
    logf("[SaveModule_New] returned %d\n", ret);
    return ret;
}

// SaveTextEX handler
typedef int (__stdcall *SaveTextEX_t)(const char*, int, int, void*);
static int __stdcall hook_SaveTextEX(const char* src, int whType, int progId, void* buf) {
    const char* srcPreview = src ? src : "(null)";
    char preview[81];
    strncpy(preview, srcPreview, 80);
    preview[80] = 0;
    logf("[SaveTextEX] src=\"%.80s\" WHType=%d ProgId=%d buf=0x%p\n",
        preview, whType, progId, buf);
    unhook(&g_hooks[4]);
    int ret = ((SaveTextEX_t)g_hooks[4].addr)(src, whType, progId, buf);
    rehook(&g_hooks[4], (void*)hook_SaveTextEX);
    logf("[SaveTextEX] returned %d\n", ret);
    return ret;
}

// CreateEntryEX handler
typedef short (__stdcall *CreateEntryEX_t)(int, int, const char*, const char*, int, int*);
static short __stdcall hook_CreateEntryEX(int whType, int subType, const char* name,
    const char* secondary, int flags, int* errCode) {
    logf("[CreateEntryEX] WHType=%d subType=%d name=\"%s\" secondary=\"%s\" flags=%d\n",
        whType, subType, name ? name : "(null)", secondary ? secondary : "(null)", flags);
    unhook(&g_hooks[5]);
    short ret = ((CreateEntryEX_t)g_hooks[5].addr)(whType, subType, name, secondary, flags, errCode);
    rehook(&g_hooks[5], (void*)hook_CreateEntryEX);
    logf("[CreateEntryEX] returned %d (err=%d)\n", ret, errCode ? *errCode : -1);
    return ret;
}

// save_dispatch handler (internal function at RVA 0x1087D0)
// It's a __stdcall with 5 args (ret 0x14)
typedef int (__stdcall *SaveDispatch_t)(int, int, int, int, int);
static int __stdcall hook_SaveDispatch(int arg0, int arg1, int arg2, int arg3, int arg4) {
    logf("[save_dispatch] ctx=%d WHType=%d ProgId=%d subtype=%d flag=%d\n",
        arg0, arg1, arg2, arg3, arg4);
    unhook(&g_hooks[6]);
    int ret = ((SaveDispatch_t)g_hooks[6].addr)(arg0, arg1, arg2, arg3, arg4);
    rehook(&g_hooks[6], (void*)hook_SaveDispatch);
    logf("[save_dispatch] returned %d\n", ret);
    return ret;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_logLock);

        // Write header to log
        FILE* f = fopen(LOG_PATH, "w");
        if (f) {
            fprintf(f, "=== hook_dll loaded at %s ===\n", __DATE__ " " __TIME__);
            fclose(f);
        }

        HMODULE hOrlib = findOrlib();
        if (!hOrlib) {
            logf("FATAL: orlib20.dll not found in process\n");
            return TRUE;
        }
        char* base = (char*)hOrlib;
        logf("orlib20.dll base = 0x%p\n", base);

        // Set up hooks
        // Hook 0: VerifyModule_New
        g_hooks[0].name = "VerifyModule_New";
        g_hooks[0].addr = (void*)GetProcAddress(hOrlib, "_VerifyModule_New@20");
        if (g_hooks[0].addr) { installHook(&g_hooks[0], (void*)hook_VerifyModule_New); g_numHooks++; }

        // Hook 1: StartCompilingProcess
        g_hooks[1].name = "StartCompilingProcess";
        g_hooks[1].addr = (void*)GetProcAddress(hOrlib, "StartCompilingProcess");
        if (g_hooks[1].addr) { installHook(&g_hooks[1], (void*)hook_StartCompilingProcess); g_numHooks++; }

        // Hook 2: EndCompilingProcess
        g_hooks[2].name = "EndCompilingProcess";
        g_hooks[2].addr = (void*)GetProcAddress(hOrlib, "EndCompilingProcess");
        if (g_hooks[2].addr) { installHook(&g_hooks[2], (void*)hook_EndCompilingProcess); g_numHooks++; }

        // Hook 3: SaveModule_New
        g_hooks[3].name = "SaveModule_New";
        g_hooks[3].addr = (void*)GetProcAddress(hOrlib, "_SaveModule_New@20");
        if (g_hooks[3].addr) { installHook(&g_hooks[3], (void*)hook_SaveModule_New); g_numHooks++; }

        // Hook 4: SaveTextEX
        g_hooks[4].name = "SaveTextEX";
        g_hooks[4].addr = (void*)GetProcAddress(hOrlib, "_SaveTextEX@16");
        if (g_hooks[4].addr) { installHook(&g_hooks[4], (void*)hook_SaveTextEX); g_numHooks++; }

        // Hook 5: CreateEntryEX
        g_hooks[5].name = "CreateEntryEX";
        g_hooks[5].addr = (void*)GetProcAddress(hOrlib, "_CreateEntryEX@24");
        if (g_hooks[5].addr) { installHook(&g_hooks[5], (void*)hook_CreateEntryEX); g_numHooks++; }

        // Hook 6: save_dispatch (internal, RVA 0x1087D0)
        g_hooks[6].name = "save_dispatch";
        g_hooks[6].addr = base + 0x1087D0;
        if (g_hooks[6].addr) { installHook(&g_hooks[6], (void*)hook_SaveDispatch); g_numHooks++; }

        logf("Installed %d hooks\n", g_numHooks);
        g_initialized = true;

    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_initialized) {
            for (int i = 0; i < g_numHooks; i++) {
                unhook(&g_hooks[i]);
            }
            logf("Unhooked %d functions\n", g_numHooks);
            DeleteCriticalSection(&g_logLock);
        }
    }
    return TRUE;
}
