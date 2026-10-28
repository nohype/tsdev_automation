// cache_backup.cpp
//
// Backup/restore engine for <root>\Program\Cache — see cache_backup.h for the
// layout contract and the "a visible backup is always complete" invariant.
// Pure Win32 + std::filesystem (kernel32 / shell32 / ole32 toolhelp only).

#include "cache_backup.h"
#include "vss_snapshot.h"

#include <shlobj.h>
#include <tlhelp32.h>
#include <winioctl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace tsdev {
namespace {

std::wstring toLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], len);
    return out;
}

std::string localTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

double elapsedSecs(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - start)
        .count();
}

CacheOpResult failResult(CacheOpStatus status, const std::wstring& path,
                         DWORD err, const std::string& msg,
                         std::chrono::steady_clock::time_point start,
                         uint64_t files, uint64_t bytes) {
    CacheOpResult r{};
    r.status = status;
    r.files = files;
    r.bytes = bytes;
    r.elapsedSecs = elapsedSecs(start);
    r.errorPath = path;
    r.win32Error = err;
    r.message = msg;
    return r;
}

// Context forwarded to the CopyFileExW callback.
struct CopyCtx {
    const CacheProgress* progress;
    uint64_t baseDone;    // bytes already copied before the current file
    uint64_t totalBytes;
    const std::wstring* currentFile;
};

DWORD CALLBACK copyProgressCb(LARGE_INTEGER /*totalFileSize*/,
                              LARGE_INTEGER totalTransferred,
                              LARGE_INTEGER /*streamSize*/,
                              LARGE_INTEGER /*streamTransferred*/,
                              DWORD /*streamNo*/, DWORD /*cbReason*/,
                              HANDLE /*src*/, HANDLE /*dst*/, LPVOID data) {
    const CopyCtx* ctx = static_cast<const CopyCtx*>(data);
    if (ctx->progress && *ctx->progress) {
        (*ctx->progress)(ctx->baseDone +
                             static_cast<uint64_t>(totalTransferred.QuadPart),
                         ctx->totalBytes, *ctx->currentFile);
    }
    return PROGRESS_CONTINUE;
}

// Per-file fallback for ACL-denied copies: reached only when CopyFileExW
// failed with ERROR_ACCESS_DENIED. Elevated callers hold SE_BACKUP_NAME, so
// opening the source with FILE_FLAG_BACKUP_SEMANTICS bypasses the ACL; the
// destination is written normally. Share-locked sources (ERROR_SHARING_
// VIOLATION, 32) are NOT handled here — a later slice adds VSS.
bool copyFileBackupSemantics(const std::wstring& src,
                             const std::wstring& dst, uint64_t baseDone,
                             uint64_t totalBytes, const std::wstring& rel,
                             const CacheProgress& progress, DWORD* err) {
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    HANDLE out = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        CloseHandle(in);
        return false;
    }
    char buf[64 * 1024];
    uint64_t moved = 0;
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buf, sizeof(buf), &got, nullptr)) {
            *err = GetLastError();
            ok = false;
            break;
        }
        if (got == 0) break;
        DWORD put = 0;
        if (!WriteFile(out, buf, got, &put, nullptr) || put != got) {
            *err = GetLastError();
            ok = false;
            break;
        }
        moved += put;
        if (progress) progress(baseDone + moved, totalBytes, rel);
    }
    if (ok) {
        // Mirror CopyFileExW: carry over all three timestamps and the
        // attribute mask so the fallback path yields a verbatim copy too.
        FILETIME ct, at, wt;
        if (GetFileTime(in, &ct, &at, &wt)) {
            SetFileTime(out, &ct, &at, &wt);
        }
        const DWORD attrs = GetFileAttributesW(src.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            SetFileAttributesW(dst.c_str(), attrs);
        }
    }
    CloseHandle(out);
    CloseHandle(in);
    return ok;
}

// Copy `files` from `srcRoot` into `dstRoot` (creating dirs first), reporting
// progress. On failure fills *errPath / *err (and optionally *errMsg with a
// verbatim message) and returns false. Sets *usedBackupSemantics when the ACL
// fallback path copied >= 1 file. When `vss` is non-null (backup path only —
// restore passes null), lock-class failures (ERROR_SHARING_VIOLATION /
// ERROR_LOCK_VIOLATION, including a failed backup-semantics retry) fall back
// to copying the file through `vss`, which is created LAZILY on first need
// and released by the caller; *usedVss is set when >= 1 file went through it.
bool copyTree(const std::wstring& srcRoot, const std::wstring& dstRoot,
              const std::vector<CacheFileEntry>& files, uint64_t totalBytes,
              const CacheProgress& progress, std::wstring* errPath,
              DWORD* err, bool* usedBackupSemantics, VssSnapshot* vss,
              bool* usedVss, std::string* errMsg) {
    uint64_t done = 0;
    for (const auto& f : files) {
        const std::wstring src = srcRoot + L"\\" + f.relativePath;
        const std::wstring dst = dstRoot + L"\\" + f.relativePath;
        const fs::path dstDir = fs::path(dst).parent_path();
        std::error_code ec;
        fs::create_directories(dstDir, ec);
        if (ec) {
            *errPath = dstDir.wstring();
            *err = static_cast<DWORD>(ec.value());
            return false;
        }
        CopyCtx ctx{&progress, done, totalBytes, &f.relativePath};
        if (!CopyFileExW(src.c_str(), dst.c_str(), copyProgressCb, &ctx,
                         nullptr, 0)) {
            DWORD copyErr = GetLastError();
            bool copied = false;
            DWORD fbErr = copyErr;
            // ACL fallback first — cheaper than a volume snapshot.
            if (copyErr == ERROR_ACCESS_DENIED &&
                copyFileBackupSemantics(src, dst, done, totalBytes,
                                        f.relativePath, progress, &fbErr)) {
                copied = true;
                *usedBackupSemantics = true;
            }
            const bool lockClass =
                copyErr == ERROR_SHARING_VIOLATION ||
                copyErr == ERROR_LOCK_VIOLATION ||
                fbErr == ERROR_SHARING_VIOLATION ||
                fbErr == ERROR_LOCK_VIOLATION;
            if (!copied && lockClass && vss) {
                if (!vss->active()) {
                    std::string verr;
                    if (!vss->create(srcRoot, &verr)) {
                        if (errMsg) {
                            *errMsg =
                                "cache backup failed: " +
                                wideToUtf8(f.relativePath) +
                                " is locked and volume snapshot unavailable"
                                " (" +
                                verr +
                                ") -- close TradeStation Desktop and retry";
                        }
                        *errPath = dst;
                        *err = fbErr;
                        return false;
                    }
                }
                const std::wstring shadowSrc = vss->shadowPath(src);
                // Same dest-write + same progress callback path: only the
                // source operand changes to the snapshot device.
                if (!shadowSrc.empty() &&
                    CopyFileExW(shadowSrc.c_str(), dst.c_str(),
                                copyProgressCb, &ctx, nullptr, 0)) {
                    copied = true;
                    *usedVss = true;
                } else {
                    const DWORD se =
                        shadowSrc.empty() ? 0 : GetLastError();
                    if (errMsg) {
                        char detail[96];
                        std::snprintf(detail, sizeof(detail),
                                      "shadow copy failed (err=%lu)",
                                      static_cast<unsigned long>(se));
                        *errMsg =
                            "cache backup failed: " +
                            wideToUtf8(f.relativePath) +
                            " is locked and volume snapshot unavailable (" +
                            detail +
                            ") -- close TradeStation Desktop and retry";
                    }
                    *errPath = dst;
                    *err = se;
                    return false;
                }
            }
            if (!copied) {
                if (copyErr == ERROR_ACCESS_DENIED) *err = fbErr;
                else *err = copyErr;
                *errPath = dst;
                return false;
            }
        }
        done += f.sizeBytes;
        if (progress) progress(done, totalBytes, f.relativePath);
    }
    return true;
}

// Write a backup.info (UTF-8 key=value) at `infoPath`. Returns false on
// failure (GetLastError-style code in *err). When `liveSource` is true a
// `live_source=true` line is appended — an honest disclosure that >= 1 file
// was captured from a live/locked source (VSS snapshot or backup-semantics
// bypass) and may be torn relative to the writer's in-memory state.
bool writeBackupInfo(const std::wstring& infoPath, const std::wstring& source,
                     uint64_t files, uint64_t bytes, DWORD* err,
                     bool liveSource) {
    HANDLE h = CreateFileW(infoPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    std::ostringstream ss;
    ss << "format=1\n"
       << "created=" << localTimestamp() << "\n"
       << "source=" << wideToUtf8(source) << "\n"
       << "files=" << files << "\n"
       << "bytes=" << bytes << "\n";
    if (liveSource) ss << "live_source=true\n";
    ss << "complete=true\n";
    const std::string text = ss.str();
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()),
                        &written, nullptr) &&
              written == text.size();
    if (!ok) *err = GetLastError();
    CloseHandle(h);
    return ok;
}

// Move `from` to `to` (both must be same volume — they always are here).
// Deliberately non-clobbering (no MOVEFILE_REPLACE_EXISTING): failing when
// `to` exists is load-bearing — the swap/rollback logic must never overwrite
// a surviving backup or destination. Returns GetLastError(), 0 on success.
DWORD movePath(const std::wstring& from, const std::wstring& to) {
    if (!MoveFileW(from.c_str(), to.c_str())) {
        return GetLastError();
    }
    return 0;
}

void removeTreeBestEffort(const std::wstring& path) {
    std::error_code ec;
    fs::remove_all(fs::path(path), ec);
}

std::wstring infoPathOf(const std::wstring& backupRoot) {
    return backupRoot + L"\\backup.info";
}

std::wstring cacheDirOf(const std::wstring& backupRoot) {
    return backupRoot + L"\\cache";
}

// True when `dir` holds a complete parked backup: a cache\ directory plus a
// backup.info that parses with complete=true.
bool parkedBackupComplete(const std::wstring& dir) {
    std::error_code ec;
    if (!fs::is_directory(fs::path(dir + L"\\cache"), ec)) return false;
    BackupInfo info = readBackupInfo(dir);
    return info.exists && info.complete;
}

// True when the visible pair at `backupRoot` is a complete backup.
bool rootBackupComplete(const std::wstring& backupRoot) {
    std::error_code ec;
    if (!fs::is_directory(fs::path(cacheDirOf(backupRoot)), ec)) return false;
    BackupInfo info = readBackupInfo(backupRoot);
    return info.exists && info.complete;
}

} // namespace

std::wstring cacheBackupRoot() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base;
    if (len > 0 && len < MAX_PATH) {
        base.assign(buf, len);
    } else {
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                           &known))) {
            base = known;
            CoTaskMemFree(known);
        }
    }
    if (base.empty()) return std::wstring();
    if (base.size() > 1 && base.back() == L'\\' &&
        base[base.size() - 2] != L':') {
        base.pop_back();
    }
    return base + L"\\tsdev_cli\\cache-backup";
}

CacheEnumerateResult enumerateTree(const std::wstring& root) {
    CacheEnumerateResult r{};
    r.ok = false;
    r.totalBytes = 0;
    r.win32Error = 0;

    std::error_code ec;
    if (!fs::is_directory(fs::path(root), ec)) {
        r.errorPath = root;
        r.win32Error = ec ? static_cast<DWORD>(ec.value())
                          : ERROR_PATH_NOT_FOUND;
        return r;
    }
    const fs::path rootPath(root);
    const size_t prefixLen = rootPath.wstring().size() + 1;
    for (fs::recursive_directory_iterator it(rootPath, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::wstring rel = it->path().wstring().substr(prefixLen);
        uint64_t size = it->file_size(ec);
        if (ec) {
            r.errorPath = it->path().wstring();
            r.win32Error = static_cast<DWORD>(ec.value());
            return r;
        }
        r.files.push_back(CacheFileEntry{rel, size});
        r.totalBytes += size;
    }
    if (ec) {
        r.errorPath = root;
        r.win32Error = static_cast<DWORD>(ec.value());
        return r;
    }
    r.ok = true;
    return r;
}

bool queryVolumeFreeSpace(const std::wstring& pathOnVolume,
                          uint64_t& outAvailable) {
    outAvailable = 0;
    // Walk up to the deepest existing ancestor — the backup root (and maybe
    // its parents) may not exist yet when this pre-flight runs.
    fs::path probe(pathOnVolume);
    std::error_code ec;
    while (!fs::exists(probe, ec)) {
        fs::path parent = probe.parent_path();
        if (parent == probe || parent.empty()) return false;
        probe = parent;
        ec.clear();
    }
    ULARGE_INTEGER freeToCaller{};
    if (!GetDiskFreeSpaceExW(probe.wstring().c_str(), &freeToCaller, nullptr,
                             nullptr)) {
        return false;
    }
    outAvailable = static_cast<uint64_t>(freeToCaller.QuadPart);
    return true;
}

bool checkFreeSpace(const std::wstring& pathOnVolume, uint64_t neededBytes,
                    uint64_t& outAvailable) {
    return queryVolumeFreeSpace(pathOnVolume, outAvailable) &&
           outAvailable >= neededBytes;
}

CacheOpResult backupCache(const std::wstring& sourceCacheDir,
                          const std::wstring& backupRoot,
                          const CacheProgress& progress) {
    const auto start = std::chrono::steady_clock::now();
    const DWORD pid = GetCurrentProcessId();

    CacheEnumerateResult en = enumerateTree(sourceCacheDir);
    if (!en.ok) {
        CacheOpStatus st = (en.win32Error == ERROR_PATH_NOT_FOUND ||
                            en.win32Error == ERROR_FILE_NOT_FOUND ||
                            en.errorPath == sourceCacheDir)
                               ? CacheOpStatus::NotFound
                               : CacheOpStatus::Error;
        return failResult(st, en.errorPath, en.win32Error,
                          "cannot enumerate source cache", start, 0, 0);
    }

    std::error_code ec;
    fs::create_directories(fs::path(backupRoot), ec);
    if (ec) {
        return failResult(CacheOpStatus::Error, backupRoot,
                          static_cast<DWORD>(ec.value()),
                          "cannot create backup root", start, 0, 0);
    }

    // Residue sweep + recovery (see header crash-window semantics). A prior
    // crash can leave staging-* junk, foreign entries, or a COMPLETE parked
    // backup under old-* while the root lacks a complete visible pair.
    // Recover such a parked backup first (re-parking any partial visible
    // residue aside so the non-clobbering recovery moves cannot collide),
    // then delete every entry that is not the visible cache\/backup.info —
    // EXCEPT a complete parked old-* that could not be recovered: it is the
    // last surviving copy and must be kept, with a residue note surfaced.
    bool recovered = false;
    bool recoveryAttempted = false;
    std::wstring sweepSkipped; // first complete parked backup kept by sweep
    const std::wstring curCache = cacheDirOf(backupRoot);
    const std::wstring curInfo = infoPathOf(backupRoot);
    {
        std::error_code ec;
        if (!rootBackupComplete(backupRoot)) {
            for (fs::directory_iterator it(backupRoot, ec), end;
                 !ec && it != end; it.increment(ec)) {
                std::error_code ec2;
                const std::wstring name = it->path().filename().wstring();
                if (name.rfind(L"old-", 0) != 0 ||
                    !it->is_directory(ec2) ||
                    !parkedBackupComplete(it->path().wstring()))
                    continue;
                recoveryAttempted = true;
                // Every rescue move is checked — a failed rescue means the
                // non-clobbering recovery moves below cannot proceed either.
                bool rescueOk = true;
                if (fs::exists(fs::path(curCache), ec2) ||
                    fs::exists(fs::path(curInfo), ec2)) {
                    const std::wstring park = backupRoot + L"\\old-rescue-" +
                                              std::to_wstring(pid);
                    removeTreeBestEffort(park);
                    fs::create_directories(fs::path(park), ec2);
                    if (fs::exists(fs::path(curCache), ec2) &&
                        movePath(curCache, park + L"\\cache") != 0)
                        rescueOk = false;
                    if (fs::exists(fs::path(curInfo), ec2) &&
                        movePath(curInfo, park + L"\\backup.info") != 0)
                        rescueOk = false;
                }
                const std::wstring dir = it->path().wstring();
                recovered = rescueOk &&
                            movePath(dir + L"\\cache", curCache) == 0 &&
                            movePath(dir + L"\\backup.info", curInfo) == 0;
                break; // one recovery is enough; the sweep drops the rest
            }
        }
        ec.clear();
        for (fs::directory_iterator it(backupRoot, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            const std::wstring name = it->path().filename().wstring();
            if (name == L"cache" || name == L"backup.info") continue;
            if (recoveryAttempted && !recovered &&
                name.rfind(L"old-", 0) == 0 && it->is_directory(ec2) &&
                parkedBackupComplete(it->path().wstring())) {
                if (sweepSkipped.empty())
                    sweepSkipped = it->path().wstring();
                continue;
            }
            removeTreeBestEffort(it->path().wstring());
        }
    }

    const std::wstring staging =
        backupRoot + L"\\staging-" + std::to_wstring(pid);
    removeTreeBestEffort(staging);
    const std::wstring stagingCache = staging + L"\\cache";
    fs::create_directories(fs::path(stagingCache), ec);
    if (ec) {
        removeTreeBestEffort(staging);
        return failResult(CacheOpStatus::Error, stagingCache,
                          static_cast<DWORD>(ec.value()),
                          "cannot create staging dir", start, 0, 0);
    }

    std::wstring errPath;
    DWORD err = 0;
    bool usedBackupSemantics = false;
    bool usedVss = false;
    std::string copyErrMsg;
    // Lazily-created VSS snapshot for lock-class source copies. Lives for
    // the rest of this backup call; released on every exit path via RAII.
    VssSnapshot vss;
    if (!copyTree(sourceCacheDir, stagingCache, en.files, en.totalBytes,
                  progress, &errPath, &err, &usedBackupSemantics, &vss,
                  &usedVss, &copyErrMsg)) {
        removeTreeBestEffort(staging);
        return failResult(CacheOpStatus::Error, errPath, err,
                          copyErrMsg.empty() ? "copy into staging failed"
                                             : copyErrMsg,
                          start, 0, 0);
    }
    const std::wstring stagingInfo = staging + L"\\backup.info";
    if (!writeBackupInfo(stagingInfo, sourceCacheDir, en.files.size(),
                         en.totalBytes, &err,
                         usedVss || usedBackupSemantics)) {
        removeTreeBestEffort(staging);
        return failResult(CacheOpStatus::Error, stagingInfo, err,
                          "cannot write backup.info", start, 0, 0);
    }

    // Swap: park any previous backup under old-<pid>, then promote the staged
    // cache\ FIRST and backup.info LAST — backup.info is the commit marker,
    // so a crash mid-promote never yields a visible backup that reads
    // complete-but-wrong. Every move result is checked; old-<pid> is deleted
    // only after the new pair is fully in place or the rollback fully
    // restored the prior pair.
    const std::wstring oldDir =
        backupRoot + L"\\old-" + std::to_wstring(pid);
    removeTreeBestEffort(oldDir);
    bool parked = false;
    if (fs::exists(fs::path(curCache), ec) ||
        fs::exists(fs::path(curInfo), ec)) {
        fs::create_directories(fs::path(oldDir), ec);
        if (fs::exists(fs::path(curCache), ec)) {
            err = movePath(curCache, oldDir + L"\\cache");
            if (err != 0) {
                removeTreeBestEffort(staging);
                removeTreeBestEffort(oldDir);
                return failResult(CacheOpStatus::Error, curCache, err,
                                  "cannot move prior backup aside", start,
                                  0, 0);
            }
        }
        if (fs::exists(fs::path(curInfo), ec)) {
            err = movePath(curInfo, oldDir + L"\\backup.info");
            if (err != 0) {
                // Undo the half-park: move cache\ back so the visible pair
                // is whole again. Only drop oldDir when that succeeded.
                bool restored = false;
                if (fs::exists(fs::path(oldDir + L"\\cache"), ec))
                    restored =
                        movePath(oldDir + L"\\cache", curCache) == 0;
                removeTreeBestEffort(staging);
                if (restored) {
                    removeTreeBestEffort(oldDir);
                    return failResult(
                        CacheOpStatus::Error, curInfo, err,
                        "cannot move prior backup.info aside; prior backup "
                        "restored",
                        start, 0, 0);
                }
                return failResult(
                    CacheOpStatus::Error, oldDir, err,
                    std::string("cannot move prior backup.info aside; "
                                "residue remains at ") +
                        wideToUtf8(oldDir),
                    start, 0, 0);
            }
        }
        parked = true;
    }
    err = movePath(stagingCache, curCache);
    const bool cachePromoted = (err == 0);
    if (err == 0) err = movePath(stagingInfo, curInfo);
    if (err != 0) {
        // Promote failed. Remove a promoted-but-uncommitted cache\ (it would
        // otherwise sit next to a missing/stale backup.info), then roll the
        // parked backup back. old-<pid> is deleted ONLY when the prior state
        // is fully restored; if rollback itself fails, old-<pid> stays — it
        // IS the surviving backup, and the next backup's sweep recovers it.
        if (cachePromoted) removeTreeBestEffort(curCache);
        std::error_code ec2;
        bool rollbackOk = !fs::exists(fs::path(curCache), ec2) &&
                          !fs::exists(fs::path(curInfo), ec2);
        if (parked) {
            if (fs::exists(fs::path(oldDir + L"\\cache"), ec2))
                rollbackOk =
                    (movePath(oldDir + L"\\cache", curCache) == 0) &&
                    rollbackOk;
            if (fs::exists(fs::path(oldDir + L"\\backup.info"), ec2))
                rollbackOk =
                    (movePath(oldDir + L"\\backup.info", curInfo) == 0) &&
                    rollbackOk;
        }
        removeTreeBestEffort(staging);
        if (rollbackOk) {
            removeTreeBestEffort(oldDir);
            return failResult(CacheOpStatus::Error, curCache, err,
                              "cannot swap staged backup into place; prior "
                              "backup restored",
                              start, 0, 0);
        }
        const std::wstring residue = parked ? oldDir : curCache;
        return failResult(CacheOpStatus::Error, residue, err,
                          std::string("cannot swap staged backup into "
                                      "place; residue remains at ") +
                              wideToUtf8(residue),
                          start, 0, 0);
    }
    removeTreeBestEffort(staging);
    removeTreeBestEffort(oldDir);

    CacheOpResult r{};
    r.status = CacheOpStatus::Ok;
    r.files = en.files.size();
    r.bytes = en.totalBytes;
    r.elapsedSecs = elapsedSecs(start);
    r.win32Error = 0;
    r.recovered = recovered;
    r.usedBackupSemantics = usedBackupSemantics;
    r.usedVss = usedVss;
    if (!sweepSkipped.empty()) {
        r.message =
            "recovery of a parked backup failed; surviving backup kept at " +
            wideToUtf8(sweepSkipped);
    }
    return r;
}

CacheOpResult restoreCache(const std::wstring& backupRoot,
                           const std::wstring& cacheDir,
                           const CacheProgress& progress) {
    const auto start = std::chrono::steady_clock::now();
    const DWORD pid = GetCurrentProcessId();

    BackupInfo info = readBackupInfo(backupRoot);
    if (!info.exists || !info.complete) {
        return failResult(CacheOpStatus::NotFound, infoPathOf(backupRoot), 0,
                          "no complete backup found", start, 0, 0);
    }
    // A backup written by a future/foreign format must never restore
    // silently — still NotFound, but the message names the format.
    if (info.format != 1) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "unsupported backup format %d",
                      info.format);
        return failResult(CacheOpStatus::NotFound, infoPathOf(backupRoot), 0,
                          msg, start, 0, 0);
    }

    const std::wstring srcTree = cacheDirOf(backupRoot);
    CacheEnumerateResult en = enumerateTree(srcTree);
    if (!en.ok) {
        return failResult(CacheOpStatus::Error, en.errorPath, en.win32Error,
                          "cannot enumerate backup cache tree", start, 0, 0);
    }

    // Integrity cross-check: the enumerated cache\ must match what
    // backup.info committed (files=/bytes=). A torn or partially deleted
    // backup reads complete in the info but cannot restore faithfully.
    if (en.files.size() != info.files || en.totalBytes != info.bytes) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "backup is incomplete: expected %llu/%llu, found "
                      "%llu/%llu",
                      static_cast<unsigned long long>(info.files),
                      static_cast<unsigned long long>(info.bytes),
                      static_cast<unsigned long long>(en.files.size()),
                      static_cast<unsigned long long>(en.totalBytes));
        return failResult(CacheOpStatus::Error, srcTree, 0, msg, start, 0, 0);
    }

    std::error_code ec;

    // Sweep Cache.tsdev-restore-* parked dirs left at the destination's
    // parent by a crashed restore. If the destination is absent, the newest
    // parked dir IS the surviving cache — move it back (best-effort, never
    // fail the op over sweep problems). Otherwise the residue is deleted.
    bool recovered = false;
    {
        const fs::path parent = fs::path(cacheDir).parent_path();
        const std::wstring prefix =
            fs::path(cacheDir).filename().wstring() + L".tsdev-restore-";
        std::error_code ec2;
        const bool destExists = fs::exists(fs::path(cacheDir), ec2);
        fs::path best;
        fs::file_time_type bestTime{};
        bool haveBest = false;
        for (fs::directory_iterator it(parent, ec2), end;
             !ec2 && it != end; it.increment(ec2)) {
            std::error_code ec3;
            if (it->path().filename().wstring().rfind(prefix, 0) != 0 ||
                !it->is_directory(ec3))
                continue;
            if (destExists) {
                removeTreeBestEffort(it->path().wstring());
                continue;
            }
            CacheEnumerateResult parkedEnum =
                enumerateTree(it->path().wstring());
            if (!parkedEnum.ok || parkedEnum.files.empty()) {
                removeTreeBestEffort(it->path().wstring());
                continue;
            }
            const fs::file_time_type t = it->last_write_time(ec3);
            if (!haveBest || (!ec3 && t > bestTime)) {
                best = it->path();
                bestTime = t;
                haveBest = true;
            }
        }
        // Recover the newest surviving parked dir, then sweep the rest.
        if (!destExists && haveBest) {
            recovered = movePath(best.wstring(), cacheDir) == 0;
            for (fs::directory_iterator it(parent, ec2), end;
                 !ec2 && it != end; it.increment(ec2)) {
                std::error_code ec3;
                if (it->path().filename().wstring().rfind(prefix, 0) == 0 &&
                    it->is_directory(ec3))
                    removeTreeBestEffort(it->path().wstring());
            }
        }
    }

    // Lock probe: an open file does NOT block a directory rename on NTFS —
    // this is a policy gate ("cache in use -> do not swap underneath the
    // process"), not a correctness precondition for the rename below.
    if (fs::is_directory(fs::path(cacheDir), ec)) {
        CacheEnumerateResult dest = enumerateTree(cacheDir);
        if (dest.ok) {
            for (const auto& f : dest.files) {
                const std::wstring p = cacheDir + L"\\" + f.relativePath;
                // Desired-access DELETE + share-mode 0: an existing handle
                // that does not grant FILE_SHARE_DELETE (the "desktop holds
                // it open" case) fails with SHARING_VIOLATION, while
                // readonly files still probe fine — the readonly attribute
                // does not block a DELETE open, and the rename-aside path
                // handles them. (Desired-access 0 does NOT work here: it is
                // empirically not subject to share-mode conflicts.)
                HANDLE h = CreateFileW(p.c_str(), DELETE, 0, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
                if (h == INVALID_HANDLE_VALUE) {
                    return failResult(CacheOpStatus::InUse, p, GetLastError(),
                                      "cache file is in use; close "
                                      "TradeStation desktop",
                                      start, 0, 0);
                }
                CloseHandle(h);
            }
        }
    }

    // Transactional replace: park the existing Cache, copy, drop the parked
    // copy on success / rename it back on failure.
    const std::wstring parked =
        cacheDir + L".tsdev-restore-" + std::to_wstring(pid) + L"-" +
        std::to_wstring(GetTickCount64());
    bool hadOld = fs::exists(fs::path(cacheDir), ec);
    if (hadOld) {
        DWORD err = movePath(cacheDir, parked);
        if (err != 0) {
            return failResult(CacheOpStatus::InUse, cacheDir, err,
                              "cannot rename existing Cache aside; close "
                              "TradeStation desktop",
                              start, 0, 0);
        }
    }
    fs::create_directories(fs::path(cacheDir), ec);
    if (ec) {
        const DWORD mkErr = static_cast<DWORD>(ec.value());
        if (hadOld && movePath(parked, cacheDir) != 0) {
            return failResult(
                CacheOpStatus::Error, parked, mkErr,
                std::string("cannot create destination cache dir; previous "
                            "cache left at ") +
                    wideToUtf8(parked),
                start, 0, 0);
        }
        return failResult(CacheOpStatus::Error, cacheDir, mkErr,
                          "cannot create destination cache dir", start, 0, 0);
    }

    std::wstring errPath;
    DWORD err = 0;
    bool usedBackupSemantics = false;
    bool usedVss = false; // restore writes into a fresh dir — NO VSS
    if (!copyTree(srcTree, cacheDir, en.files, en.totalBytes, progress,
                  &errPath, &err, &usedBackupSemantics, nullptr, &usedVss,
                  nullptr)) {
        // Roll back honestly: the partial new cache must be gone before the
        // parked one can move back (movePath is non-clobbering). Report what
        // ACTUALLY happened, including residue, never a blanket "restored".
        removeTreeBestEffort(cacheDir);
        if (hadOld) {
            std::error_code ec2;
            const bool restored =
                !fs::exists(fs::path(cacheDir), ec2) &&
                movePath(parked, cacheDir) == 0;
            return failResult(
                CacheOpStatus::Error, errPath, err,
                restored
                    ? std::string(
                          "restore copy failed; previous cache restored")
                    : std::string(
                          "restore copy failed; previous cache left at ") +
                          wideToUtf8(parked),
                start, 0, 0);
        }
        return failResult(CacheOpStatus::Error, errPath, err,
                          "restore copy failed", start, 0, 0);
    }
    if (hadOld) removeTreeBestEffort(parked);

    CacheOpResult r{};
    r.status = CacheOpStatus::Ok;
    r.files = en.files.size();
    r.bytes = en.totalBytes;
    r.elapsedSecs = elapsedSecs(start);
    r.win32Error = 0;
    r.recovered = recovered;
    r.usedBackupSemantics = usedBackupSemantics;
    return r;
}

BackupInfo readBackupInfo(const std::wstring& backupRoot) {
    BackupInfo info{};
    info.exists = false;
    info.complete = false;
    info.files = 0;
    info.bytes = 0;
    info.format = 0;

    const std::wstring path = infoPathOf(backupRoot);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return info;
    DWORD sz = GetFileSize(h, nullptr);
    std::string text;
    if (sz != INVALID_FILE_SIZE && sz > 0) {
        text.resize(sz);
        DWORD read = 0;
        if (!ReadFile(h, &text[0], sz, &read, nullptr) || read != sz) {
            text.clear();
        }
    }
    CloseHandle(h);
    // The file exists even when empty/torn/unreadable: exists=true,
    // complete=false. Malformed lines and missing keys are simply skipped.
    info.exists = true;
    if (text.empty()) return info;

    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "format") info.format = std::atoi(val.c_str());
        else if (key == "created") info.created = val;
        else if (key == "files") info.files = std::strtoull(val.c_str(), nullptr, 10);
        else if (key == "bytes") info.bytes = std::strtoull(val.c_str(), nullptr, 10);
        else if (key == "source") info.source = utf8ToWide(val);
        else if (key == "complete") info.complete = (val == "true");
    }
    return info;
}

std::vector<std::wstring> runningTsProcessNames() {
    std::vector<std::wstring> found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok;
         ok = Process32NextW(snap, &pe)) {
        const std::wstring name = toLower(pe.szExeFile);
        if (name == L"orchart.exe" || name == L"tsdev.exe" ||
            name == L"whserver.exe") {
            found.push_back(name);
        }
    }
    CloseHandle(snap);
    return found;
}

void compressTreeInPlace(const std::wstring& root,
                         const std::function<bool()>& shouldStop,
                         CompressStats* stats) {
    CompressStats acc{};
    // Attributes that disqualify a file from in-place compression outright.
    const DWORD kSkipAttrs =
        FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_READONLY |
        FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN |
        FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE |
        FILE_ATTRIBUTE_REPARSE_POINT;

    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::path(root), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (shouldStop && shouldStop()) break;
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        ++acc.files;
        const std::wstring path = it->path().wstring();
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES ||
            (attrs & kSkipAttrs) != 0) {
            ++acc.skipped;
            continue;
        }
        const uint64_t size = it->file_size(ec2);
        if (ec2 || size < 64 * 1024) {
            ++acc.skipped;
            continue;
        }
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            ++acc.skipped;
            continue;
        }
        const USHORT fmt = COMPRESSION_FORMAT_LZNT1;
        DWORD ret = 0;
        const BOOL ok =
            DeviceIoControl(h, FSCTL_SET_COMPRESSION,
                            const_cast<USHORT*>(&fmt), sizeof(fmt),
                            nullptr, 0, &ret, nullptr);
        if (!ok) {
            const DWORD err = GetLastError();
            CloseHandle(h);
            ++acc.skipped;
            if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED)
                break; // not an NTFS volume — no point continuing the walk
            continue;
        }
        DWORD hi = 0;
        const DWORD lo = GetCompressedFileSizeW(path.c_str(), &hi);
        acc.bytesBefore += size;
        if (lo != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
            acc.bytesAfter +=
                (static_cast<uint64_t>(hi) << 32) | lo;
        }
        ++acc.compressed;
        CloseHandle(h);
    }
    if (stats) *stats = acc;
}

void startBackupCompression(const std::wstring& root) {
    try {
        std::thread([root] {
            try {
                HANDLE self = GetCurrentThread();
                SetThreadPriority(self, THREAD_PRIORITY_LOWEST);
                SetThreadPriority(self, THREAD_MODE_BACKGROUND_BEGIN);
                compressTreeInPlace(root, [] { return false; }, nullptr);
                SetThreadPriority(self, THREAD_MODE_BACKGROUND_END);
            } catch (...) {
                // Opportunistic compression: any error ends the thread quietly.
            }
        }).detach();
    } catch (...) {
        // Thread construction itself may throw; compression is opportunistic.
    }
}

} // namespace tsdev
