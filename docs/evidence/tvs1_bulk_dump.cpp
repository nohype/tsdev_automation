// tvs1_bulk_dump.cpp -- READ-ONLY live diagnostic (tmp only, not part of the
// build/regression). Dumps the raw _LoadMasterRecsEX@16 bulk master-record
// buffer for categories 0/1/2 and reports EVERY printable ASCII run >= 4 chars
// per record with its record-relative offset, so the true stored-name location
// (if any) can be identified. Performs NO warehouse mutations.
#include "../src/orlib_resolver.h"
#include "../src/tsdev_lib_test_hooks.h"
#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>

using namespace tsdev;

namespace {

typedef int (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                            const char* path, int filterMode);

struct LoadSehResult { int result; };
bool callLoadPOD(LoadMasterRecsExFn fn, int cat, HGLOBAL* outBuf,
                 LoadSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(cat, outBuf, (const char*)NULL, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

ComApi realComApi() { return ComApi{&CoInitializeEx, &CoUninitialize}; }

void hexdump(const unsigned char* p, size_t n, const char* label) {
    std::printf("  --- %s ---\n", label);
    for (size_t i = 0; i < n; i += 16) {
        std::printf("  %04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < n; j++)
            std::printf("%02x ", p[i + j]);
        std::printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            unsigned char c = p[i + j];
            std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        std::printf("|\n");
    }
}

// Print every printable ASCII run of length >= 4 with its start offset.
void scanStrings(const unsigned char* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (p[i] >= 0x20 && p[i] < 0x7f) {
            size_t j = i;
            while (j < n && p[j] >= 0x20 && p[j] < 0x7f) j++;
            if (j - i >= 4) {
                std::printf("    str@0x%03zx len=%3zu: \"%.*s\"\n",
                            i, j - i, (int)(j - i), (const char*)p + i);
            }
            i = j;
        } else {
            i++;
        }
    }
}

DWORD WINAPI run(LPVOID) {
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi = realComApi();
        cfg.throwAfterCom = false;
        lib = makeTsDevLibForTest(nullptr, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: TsDevLib ctor threw: %s\n", e.what());
        return 100;
    }
    if (!lib) { std::fprintf(stderr, "FATAL: null lib\n"); return 101; }

    LoadMasterRecsExFn loadFn = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        loadFn = reinterpret_cast<LoadMasterRecsExFn>(
            resolver.resolveExport("_LoadMasterRecsEX@16"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: resolver threw: %s\n", e.what());
        return 102;
    }
    std::printf("[*] _LoadMasterRecsEX@16 = %p\n", (void*)loadFn);

    struct Cat { int cat; const char* label; } cats[] = {
        { 0, "strategy (cat 0)" },
        { 1, "function (cat 1)" },
        { 2, "indicator family (cat 2)" },
    };
    const int kRec = 0x10A;
    for (const auto& c : cats) {
        HGLOBAL buf = NULL;
        std::printf("\n===== %s =====\n", c.label);
        LoadSehResult seh;
        if (!callLoadPOD(loadFn, c.cat, &buf, &seh)) {
            std::printf("  [SEH in _LoadMasterRecsEX]\n");
            continue;
        }
        int count = seh.result;
        std::printf("  count=%d buf=%p\n", count, (void*)buf);
        if (count <= 0 || !buf) { if (buf) GlobalFree(buf); continue; }
        char* recs = (char*)GlobalLock(buf);
        if (!recs) { GlobalFree(buf); continue; }

        for (int i = 0; i < count; i++) {
            const unsigned char* rec = (const unsigned char*)recs + (size_t)i * kRec;
            short progId = 0;
            memcpy(&progId, rec, 2);
            std::printf("  [rec %d] progId=%d\n", i, (int)progId);
            scanStrings(rec, kRec);
        }
        // Full hexdump: user probe records + a few built-ins for comparison.
        for (int i = 0; i < count; i++) {
            const unsigned char* rec = (const unsigned char*)recs + (size_t)i * kRec;
            short progId = 0;
            memcpy(&progId, rec, 2);
            bool want = (i < 3) || (progId == 10568) || (progId == 193) ||
                        (c.cat == 1);  // all function records
            if (!want) continue;
            char label[64];
            std::snprintf(label, sizeof(label), "record#%d progId=%d full hex",
                          i, (int)progId);
            hexdump(rec, kRec, label);
        }
        GlobalUnlock(buf);
        GlobalFree(buf);
    }
    std::printf("\n[*] done\n");
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    DWORD tid;
    HANDLE h = CreateThread(nullptr, 64 * 1024 * 1024, run, nullptr, 0, &tid);
    if (!h) return 11;
    DWORD w = WaitForSingleObject(h, 60000);
    if (w != WAIT_OBJECT_0) {
        std::fprintf(stderr, "FATAL: timeout/fail (wait=%lu)\n", w);
        return 98;
    }
    DWORD code = 1;
    GetExitCodeThread(h, &code);
    CloseHandle(h);
    return (int)code;
}
