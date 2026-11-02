// whserver_names.h
//
// WHServer IWHouse COM stored-name enumeration -- the name source behind
// TsDevLib::listItems. The 0x10A-byte master record returned by
// _LoadMasterRecsEX@16 contains NO stored name (root-cause proven: only a
// ProgId at 0x00 and a DESCRIPTION field at +0x17), so the WHServer name
// index is the ONLY authoritative stored-name source. This module wraps the
// live-proven name-index mechanism (tmp/enum_mechanism_report.md Sec. 2;
// activation chain: docs/KNOWLEDGE_BASE.md Sec. 17):
//
//   ProgramsCountByType(Type=expanded whType) -> I4   (stored item count)
//   NextId(Type)                              -> I4   (exclusive upper bound
//                                                      of the sparse ProgId
//                                                      space)
//   ProgramById(Type, Id)                     -> IProgram (vacant Id ->
//                                                      null/empty result)
//   IProgram::Name                            -> BSTR (verbatim stored name,
//                                                      case preserved)
//
// All four members are resolved BY NAME via IDispatch::GetIDsOfNames against
// the WHServer typeinfo -- zero hardcoded dispids, zero hardcoded RVAs. The
// only non-typelib constants are the custom factory IID + its vtable slot
// (the project's proven COM activation contract) and the stable CLSIDs/IIDs.
// A live WHServer.exe is required (SCM auto-respawns it).
//
// Testability split (no TradeStation needed to test the logic):
//   IWhNameSource    - minimal seam supplying count(type) and the name-by-id
//                      walk primitives.
//   collectTypeNames - PURE orchestration over the seam (whType mapping,
//                      sparse-id walk, count-consistency report, failure
//                      semantics) -- the unit-tested part.
//   WhComNameSource  - the real COM provider binding the seam to the proven
//                      chain. Binds on construction, on the creating STA
//                      thread only (own-proxy QI from another apartment
//                      returns RPC_E_WRONG_THREAD -- never marshal).
//
// 32-bit only (WHServer/orlib20 are 32-bit).

#ifndef TSDEV_WHSERVER_NAMES_H
#define TSDEV_WHSERVER_NAMES_H

#include <string>
#include <vector>
#include <windows.h>
#include <ole2.h>     // IUnknown / IDispatch / BSTR / DISPID (COM provider members)

#include "tsdev_lib.h"  // ComponentType (the warehouse-type SSOT)

namespace tsdev {

// Minimal enumeration seam over the WHServer COM calls. Every method returns
// false ONLY on a real failure (COM activation failure, RPC/Invoke error,
// SEH in a COM call); "no such id" / "empty type" are NOT failures. The
// `whType` parameters are the EXPANDED whType values (the I2 argument the
// typelib members take): Strategy=0, Function=1, Indicator=2, ShowMe=1282,
// TradingApp=5122.
class IWhNameSource {
public:
    virtual ~IWhNameSource() {}
    // ProgramsCountByType(whType) -> outCount (stored item count).
    virtual bool getTypeCount(int whType, int& outCount) = 0;
    // NextId(whType) -> outNextId (exclusive upper bound of the id space;
    // ids are sparse, so most ids below it are vacant).
    virtual bool getNextId(int whType, int& outNextId) = 0;
    // ProgramById(whType, id) -> IProgram::Name. outFound=false (and
    // outName cleared) for a vacant id; the call itself still succeeded.
    virtual bool getProgramName(int whType, int id, bool& outFound,
                                std::string& outName) = 0;
};

// Pure orchestration (no COM / orlib20 / TradeStation dependency): fills
// `outNames` (cleared first) with the verbatim stored names of ALL items of
// `type`, in ascending-ProgId walk order -- NO sorting, NO deduplication.
//
//   * whType mapping: every source call receives static_cast<int>(type) --
//     the EXPANDED whType, the exact I2 parameter the typelib members take.
//   * empty type: a source count <= 0 is SUCCESS with zero names (the id
//     walk is skipped).
//   * id walk: ids 1 .. NextId-1; vacant ids (null/empty ProgramById) are
//     skipped -- the id space is sparse and gaps are normal.
//   * count consistency: outNames.size() is the AUTHORITATIVE element count.
//     When it disagrees with the source count (count > returned included),
//     the call still SUCCEEDS with the walked names and a Warning is logged.
//   * failure: any source error (false return, or an exception thrown by the
//     source) returns false with `outNames` left EMPTY -- any partial names
//     collected before the failure are discarded.
bool collectTypeNames(IWhNameSource& source, ComponentType type,
                      std::vector<std::string>& outNames);

// Real COM provider (production). Binds the proven activation chain on
// construction -- CoGetClassObject(CLSID_Control {7CEEA90C-...}) -> custom
// factory IID {7CEEACBC-...} vtbl+0x20 create -> IControl::WHouseEx
// (DISPATCH_PROPERTYGET, Create=1, myWorkDir) -> IWHouse IDispatch -- and
// resolves the three IWHouse member dispids by name once. `myWorkDir` is a
// HARD precondition: it must be the RESOLVED TradeStation MyWork directory
// (WHouseEx Create=1 must never run against an unresolved/guessed path).
// Same-thread contract: construct, use, and destroy on ONE STA thread (the
// TsDevLib owning thread). CoInitializeEx here only confirms the apartment
// (S_OK/S_FALSE expected -- the TsDevLib COM apartment guard initialized
// it); CoUninitialize is NEVER called (the apartment belongs to the guard).
// Every COM call is SEH-wrapped (returns false, never crashes through).
// The constructor throws std::runtime_error (with the failing stage and the
// HRESULT) when the chain cannot be bound.
class WhComNameSource : public IWhNameSource {
public:
    explicit WhComNameSource(const std::wstring& myWorkDir);
    ~WhComNameSource() override;

    WhComNameSource(const WhComNameSource&) = delete;
    WhComNameSource& operator=(const WhComNameSource&) = delete;

    bool getTypeCount(int whType, int& outCount) override;
    bool getNextId(int whType, int& outNextId) override;
    bool getProgramName(int whType, int id, bool& outFound,
                        std::string& outName) override;

private:
    // Binds the proven activation chain (KB Sec. 17) and resolves the three
    // IWHouse member dispids by name. Returns false with `detail` set to the
    // failing stage + HRESULT (no throw here; the ctor throws).
    bool bind(std::string& detail);
    // Idempotent teardown: drops every held COM ref + BSTR. Release() return
    // values are ignored (RPC errors on an already-dead server are expected
    // here) and never reported.
    void release();

    std::wstring m_myWorkDir;
    IUnknown*    m_factory = nullptr;
    IUnknown*    m_ctl     = nullptr;
    IDispatch*   m_dctrl   = nullptr;
    IDispatch*   m_whouse  = nullptr;
    BSTR         m_outName = nullptr;  // factory-assigned out-name (owned)
    DISPID       m_idCount  = DISPID_UNKNOWN;  // ProgramsCountByType
    DISPID       m_idNextId = DISPID_UNKNOWN;  // NextId
    DISPID       m_idById   = DISPID_UNKNOWN;  // ProgramById
};

} // namespace tsdev

#endif // TSDEV_WHSERVER_NAMES_H