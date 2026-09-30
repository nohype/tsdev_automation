// cache_backup.h
//
// Backup/restore engine for the TradeStation cache tree (<root>\Program\Cache).
// Pure filesystem module: wide Win32 + std::filesystem only — NO orlib20,
// no warehouse, no stderr output (progress is reported through an injected
// callback; rendering lives in the CLI layer).
//
// BACKUP LAYOUT CONTRACT (under "backup root",
// default %LOCALAPPDATA%\tsdev_cli\cache-backup\):
//   cache-backup\cache\       verbatim recursive copy of the source Cache tree
//   cache-backup\backup.info  UTF-8 key=value lines:
//                               format=1
//                               created=YYYY-MM-DD HH:MM:SS   (local time)
//                               source=<absolute source dir, UTF-8>
//                               files=<n>
//                               bytes=<n>
//                               complete=true
//   Optionally `live_source=true` (before complete=true) when at least one
//   file was captured from a live/locked source — via a VSS snapshot
//   (VssSnapshot, see vss_snapshot.h) after CopyFileExW and the
//   FILE_FLAG_BACKUP_SEMANTICS fallback both failed with a lock-class error
//   (ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION), or via the
//   backup-semantics ACL bypass. It is an honest "possibly torn capture"
//   disclosure; absent otherwise. Restore does NOT use VSS (it writes into
//   a freshly created directory).
// One backup exists at a time. A new backup is built in
// cache-backup\staging-<pid>\ and swapped into place only after the copy and
// backup.info write fully succeed; the previous backup (if any) is moved to
// cache-backup\old-<pid>\ during the swap.
//
// SWAP / CRASH-WINDOW SEMANTICS (honest contract):
//   Promote order is: park old pair -> old-*, move staging cache\ -> cache\,
//   move staging backup.info -> backup.info LAST (backup.info is the commit
//   marker), delete old-*. A crash can therefore leave:
//     - staging-* junk (deleted by the next backup's residue sweep)
//     - old-* holding a COMPLETE parked backup while the root shows no or a
//       partial visible backup (recovered by the sweep: moved back to
//       cache\ + backup.info when the root lacks a complete visible pair)
//     - a new cache\ with a missing/stale backup.info (an incomplete visible
//       backup — never reported complete because complete=true lives in
//       backup.info)
//   INVARIANT: a backup that READS complete (cache\ + backup.info with
//   complete=true) is always whole; the sweep keeps at most the visible
//   pair. Absolute "no window exists" cannot be guaranteed — recovery via
//   the sweep is the mechanism that restores it.
// Restore likewise replaces Cache transactionally: the existing Cache is
// renamed to Cache.tsdev-restore-<pid>-<tick> before the copy and renamed
// back on failure; a crashed restore's parked dir is recovered (destination
// absent) or swept (destination present) at the start of the next restore.

#ifndef TSDEV_CACHE_BACKUP_H
#define TSDEV_CACHE_BACKUP_H

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tsdev {

// One file discovered by enumerateTree(); relativePath uses '\' separators
// relative to the enumerated root.
struct CacheFileEntry {
    std::wstring relativePath;
    uint64_t sizeBytes;
};

struct CacheEnumerateResult {
    bool ok;
    std::vector<CacheFileEntry> files;
    uint64_t totalBytes;
    DWORD win32Error;
    std::wstring errorPath;
};

// Distinguishable outcomes; the CLI maps NotFound → exit 5, InUse → exit 16.
enum class CacheOpStatus {
    Ok,
    NotFound,   // backup.info absent / incomplete, or source tree missing
    InUse,      // a destination file is locked by another process
    Error       // any other failure (see win32Error / errorPath / message)
};

struct CacheOpResult {
    CacheOpStatus status;
    uint64_t files;         // files copied (backup) or restored (restore)
    uint64_t bytes;         // bytes copied / restored
    double elapsedSecs;
    std::wstring errorPath; // offending path (e.g. first locked file on InUse)
    DWORD win32Error;
    std::string message;
    // True when the op recovered crash residue (a complete parked old-*
    // backup restored to visibility on backup; a parked Cache.tsdev-restore-*
    // dir moved back to an absent destination on restore).
    bool recovered;
    // True when at least one file required the FILE_FLAG_BACKUP_SEMANTICS
    // per-file copy fallback (ACL-denied source, elevated caller).
    bool usedBackupSemantics;
    // True when at least one file was copied through a VSS volume snapshot
    // because it was share/lock-denied (backup only; always false on
    // restore).
    bool usedVss;
};

// Progress callback: forwarded once per file at minimum and once per
// CopyFileExW chunk (the CLI layer does its own redraw throttling).
using CacheProgress =
    std::function<void(uint64_t bytesDone, uint64_t bytesTotal,
                       const std::wstring& currentFile)>;

// Parsed backup.info. exists == false when backup.info is absent; complete
// == false when it is absent or complete!=true.
struct BackupInfo {
    bool exists;
    bool complete;
    std::string created;     // verbatim "YYYY-MM-DD HH:MM:SS"
    uint64_t files;
    uint64_t bytes;
    std::wstring source;     // UTF-8 decoded
    // Parsed "format=" value; 0 when absent or unparseable. The only format
    // this engine writes and restores is 1.
    int format;
};

// Default backup root: %LOCALAPPDATA%\tsdev_cli\cache-backup (LOCALAPPDATA
// env var first, FOLDERID_LocalAppData fallback). Does NOT create the dir.
// Empty on failure.
std::wstring cacheBackupRoot();

// Recursive walk of `root` producing the file list, total bytes and count.
// ok=false when `root` is missing/not a directory or enumeration fails.
CacheEnumerateResult enumerateTree(const std::wstring& root);

// Queries free space on the volume containing (or nearest existing ancestor
// of) pathOnVolume. false = the query itself failed (distinguishable from
// mere insufficiency). `outAvailable` receives the measured free bytes
// (0 on query failure).
bool queryVolumeFreeSpace(const std::wstring& pathOnVolume,
                          uint64_t& outAvailable);

// Free-space pre-flight for a planned copy of `neededBytes` onto the volume
// holding `pathOnVolume`: queryVolumeFreeSpace && avail >= neededBytes —
// a failed query therefore reads as false, same as before.
bool checkFreeSpace(const std::wstring& pathOnVolume, uint64_t neededBytes,
                    uint64_t& outAvailable);

// Backup `sourceCacheDir` into `backupRoot` per the layout contract above.
// Stages then swaps; on any failure the prior backup is left intact.
CacheOpResult backupCache(const std::wstring& sourceCacheDir,
                          const std::wstring& backupRoot,
                          const CacheProgress& progress);

// Restore the backup at `backupRoot` into `cacheDir` (the Cache dir itself).
// NotFound when no complete backup exists. InUse (with errorPath = first
// locked file) when any existing cacheDir file is exclusively locked, or when
// the Cache dir cannot be renamed aside. Transactional: on copy failure the
// previous Cache is renamed back.
CacheOpResult restoreCache(const std::wstring& backupRoot,
                           const std::wstring& cacheDir,
                           const CacheProgress& progress);

// Parse backupRoot\backup.info. Never throws; exists=false on absent file.
BackupInfo readBackupInfo(const std::wstring& backupRoot);

// Lowercase names of running TradeStation desktop processes found via a
// Toolhelp32 snapshot, filtered to {orchart.exe, tsdev.exe, whserver.exe}.
// Used for a warn-only "desktop running" notice on backup.
std::vector<std::wstring> runningTsProcessNames();

// Compression accounting for compressTreeInPlace. All counts are cumulative
// for one call.
struct CompressStats {
    uint64_t files;        // regular files visited
    uint64_t compressed;   // files FSCTL_SET_COMPRESSION succeeded on
    uint64_t skipped;      // visited files not compressed (attr/size/open/IOCTL)
    uint64_t bytesBefore;  // sum of logical sizes of compressed files
    uint64_t bytesAfter;   // sum of GetCompressedFileSizeW of compressed files
};

// Recursively NTFS-compresses `root` in place via
// DeviceIoControl(FSCTL_SET_COMPRESSION, COMPRESSION_FORMAT_LZNT1), one file
// at a time. Only regular files >= 64 KB are attempted (smaller files cannot
// gain meaningfully). Skipped silently: files already COMPRESSED, files with
// READONLY/SYSTEM/HIDDEN/ENCRYPTED/SPARSE/REPARSE attributes, and any open
// or per-file IOCTL failure. Exception: the FIRST filesystem-level rejection
// (ERROR_INVALID_FUNCTION / ERROR_NOT_SUPPORTED — the volume is not NTFS)
// stops the whole walk immediately. shouldStop() is checked between files;
// true ends the walk early. Compression is transparent to readers — a
// partially compressed tree is always valid and readable. `stats` (optional
// out-param) receives the per-call accounting.
void compressTreeInPlace(const std::wstring& root,
                         const std::function<bool()>& shouldStop,
                         CompressStats* stats = nullptr);

// Fire-and-forget NTFS compression of `root` (typically <backupRoot>\cache)
// on a detached worker thread at THREAD_PRIORITY_LOWEST +
// THREAD_MODE_BACKGROUND_BEGIN (low I/O priority so foreground ops are not
// slowed). The predicate never stops early — process exit is the stop; an
// abrupt exit mid-FSCTL is safe because a partially compressed tree is
// always valid/readable (NTFS guarantees). Never throws; any error ends the
// thread quietly.
void startBackupCompression(const std::wstring& root);

} // namespace tsdev

#endif // TSDEV_CACHE_BACKUP_H
