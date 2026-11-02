// Frida enumeration FINAL — correct CAssoc layout + full type mapping.
// Corrections to static RE discovered during dynamic verification:
// 1. CAssoc layout: { pNext@0, value@4, key@8 } (swapped from standard MFC)
// 2. FUN_10044820 takes 4 params: (this, category, subtype, index) — not just (this)
// 3. LoadList must be called per-type with packed value from DAT_1008fd04, not type=0
// 4. CELTechInfo+0x08 is category (low byte); full type = (subtype@+0x0a << 8) | type@+0x08
function resolveOrdinal(modName, ordinal) {
    var mod = Process.findModuleByName(modName);
    var base = mod.base;
    var peOff = base.add(0x3c).readU32();
    var optHdr = base.add(peOff).add(24);
    var exportDirRva = optHdr.add(96).readU32();
    var exportDir = base.add(exportDirRva);
    var ordinalBase = exportDir.add(16).readU32();
    var addrFuncs = base.add(exportDir.add(28).readU32());
    var fnRva = addrFuncs.add((ordinal - ordinalBase) * 4).readU32();
    return base.add(fnRva);
}

function readCString(addr) {
    try {
        var dataPtr = addr.readPointer();
        if (dataPtr.isNull()) { return "<null>"; }
        var nDataLen = dataPtr.sub(8).readU32();
        if (nDataLen < 0 || nDataLen > 65536) { nDataLen = 260; }
        return dataPtr.readUtf16String(nDataLen);
    } catch (e) { return "<err>"; }
}

// CAssoc: pNext@0, value@4 (CELTechInfo*), key@8 (WORD recordId)
function walkCMap(mapAddr) {
    var results = [];
    try {
        var hashTablePtr = mapAddr.add(0x04).readPointer();
        var hashSize = mapAddr.add(0x08).readU32();
        if (hashSize === 0 || hashTablePtr.isNull()) { return results; }
        for (var i = 0; i < hashSize; i++) {
            var pAssoc = hashTablePtr.add(i * 4).readPointer();
            while (!pAssoc.isNull()) {
                var value = pAssoc.add(0x04).readPointer();
                var key = pAssoc.add(0x08).readU32() & 0xffff;
                results.push({ key: key, value: value });
                pAssoc = pAssoc.readPointer();
            }
        }
    } catch (e) {}
    return results;
}

var fullTypeNames = {
    0x0000: "Strategy", 0x0001: "Function", 0x0002: "Indicator", 0x0003: "ActivityBar",
    0x0004: "Function(Builtin)", 0x0402: "PaintBar", 0x0502: "ShowMe",
    0x0a02: "VolatilityModel", 0x0c02: "PricingModel", 0x0e02: "BidAskModel",
    0x1002: "SearchStrategy", 0x1202: "ProbabilityMap", 0x1402: "TradingApp"
};
function fullTypeName(fullType) {
    return fullTypeNames[fullType] || ("FullType_0x" + fullType.toString(16));
}

function main() {
    var ORELUI = "ORELUI20.dll";
    var mod = Process.findModuleByName(ORELUI);
    var base = mod.base;
    console.log("[*] ORELUI20.dll base = " + base);

    var pCtor = resolveOrdinal(ORELUI, 17);
    var pLoadList = resolveOrdinal(ORELUI, 1291);
    var pDtor = resolveOrdinal(ORELUI, 166);
    var pCatiCtor = base.add(0x44820);

    // Read type table
    var typeTableAddr = base.add(0x8fd04);
    var typeEntries = [];
    console.log("[*] Type category table (DAT_1008fd04):");
    for (var i = 0; i < 13; i++) {
        var packed = typeTableAddr.add(i * 4).readU32();
        var cat = packed & 0xff;
        var sub = (packed >> 8) & 0xff;
        var name = fullTypeName(packed);
        console.log("  [" + i + "] packed=0x" + packed.toString(16) + " => " + name);
        typeEntries.push({ packed: packed, category: cat, subtype: sub, index: i });
    }

    var mapBuf = Memory.alloc(0x40);
    for (var i = 0; i < 0x40; i += 4) { mapBuf.add(i).writeU32(0); }
    var dlgBuf = Memory.alloc(0x400);
    for (var i = 0; i < 0x400; i += 4) { dlgBuf.add(i).writeU32(0); }
    var catiBuf = Memory.alloc(0x28);

    var ctor = new NativeFunction(pCtor, 'void', ['pointer', 'pointer', 'pointer'], 'thiscall');
    ctor(dlgBuf, mapBuf, ptr(0));
    console.log("[+] Dialog constructed (NULL CWnd* — no crash)");

    var catiCtor = new NativeFunction(pCatiCtor, 'pointer', ['pointer', 'int', 'int', 'int'], 'thiscall');
    var loadList = new NativeFunction(pLoadList, 'void', ['pointer', 'int', 'pointer'], 'thiscall');

    var allFound = [];

    // Enumerate per-type (skip type[9]=0 which enumerates all with power-type filter)
    // Actually, type[9] packed=0 triggers the "all types" path. Let me handle it specially.
    for (var ti = 0; ti < typeEntries.length; ti++) {
        var entry = typeEntries[ti];
        for (var i = 0; i < 0x28; i += 4) { catiBuf.add(i).writeU32(0); }
        try { catiCtor(catiBuf, entry.category, entry.subtype, entry.index); } catch(e) { continue; }
        try { loadList(dlgBuf, entry.packed, catiBuf); } catch(e) { continue; }
        var recs = walkCMap(catiBuf.add(0x0c));
        if (recs.length > 0) {
            console.log("[+] Type[" + ti + "] " + fullTypeName(entry.packed) + " (packed=0x" + entry.packed.toString(16) + "): " + recs.length + " items");
        }
        for (var j = 0; j < recs.length; j++) {
            var celTi = recs[j].value;
            try {
                var cat = celTi.add(0x08).readU16();
                var sub = celTi.add(0x0a).readU16();
                var fullType = (sub << 8) | cat;
                var rid = celTi.add(0x0c).readU16();
                var name = readCString(celTi.add(0x10));
                var progId = readCString(celTi.add(0x1c));
                allFound.push({ fullType: fullType, cat: cat, sub: sub, rid: rid, name: name, progId: progId, typeEntry: ti });
            } catch (e) {}
        }
    }

    // Also built-in functions (type 4) — InitLists does this for category==1
    for (var i = 0; i < 0x28; i += 4) { catiBuf.add(i).writeU32(0); }
    try { catiCtor(catiBuf, 4, 0, 1); loadList(dlgBuf, 4, catiBuf); } catch(e) {}
    var recs4 = walkCMap(catiBuf.add(0x0c));
    console.log("[+] Built-in Functions (type 4): " + recs4.length + " items");
    for (var j = 0; j < recs4.length; j++) {
        var celTi = recs4[j].value;
        try {
            var cat = celTi.add(0x08).readU16();
            var sub = celTi.add(0x0a).readU16();
            var fullType = (sub << 8) | cat;
            var name = readCString(celTi.add(0x10));
            var progId = readCString(celTi.add(0x1c));
            allFound.push({ fullType: fullType, cat: cat, sub: sub, rid: 0, name: name, progId: progId, typeEntry: -1 });
        } catch (e) {}
    }

    var dtor = new NativeFunction(pDtor, 'void', ['pointer'], 'thiscall');
    try { dtor(dlgBuf); console.log("[+] Dialog destroyed (no crash)"); } catch (e) {}

    console.log("[*] === RESULTS ===");
    console.log("[*] Total items enumerated: " + allFound.length);

    // E2eTvs7_Enum items
    var enumItems = allFound.filter(function(r) { return r.name && r.name.indexOf("E2eTvs7_Enum") === 0; });
    console.log("[*] E2eTvs7_Enum items found: " + enumItems.length);
    for (var i = 0; i < enumItems.length; i++) {
        var r = enumItems[i];
        console.log("  ENUM_MATCH: name=\"" + r.name + "\" fullType=0x" + r.fullType.toString(16) + " (" + fullTypeName(r.fullType) + ") progId=\"" + r.progId + "\"");
    }

    // Verbatim name check: E2eTvs7_EnumFn_MixedCase should be exactly that (mixed case preserved)
    var fnItem = enumItems.find(function(r) { return r.name === "E2eTvs7_EnumFn_MixedCase"; });
    if (fnItem) {
        console.log("[*] VERBATIM_NAME_CHECK: inserted 'E2eTvs7_EnumFn_MixedCase', enumerated as '" + fnItem.name + "' — " + (fnItem.name === "E2eTvs7_EnumFn_MixedCase" ? "MATCH (case preserved)" : "MISMATCH"));
    }

    // Type summary by full type
    var types = {};
    for (var i = 0; i < allFound.length; i++) {
        var t = allFound[i].fullType;
        if (!types[t]) types[t] = 0;
        types[t]++;
    }
    console.log("[*] Type summary (by full type):");
    var sortedTypes = Object.keys(types).map(function(k) { return parseInt(k); }).sort(function(a,b) { return a-b; });
    for (var i = 0; i < sortedTypes.length; i++) {
        var t = sortedTypes[i];
        console.log("  0x" + t.toString(16) + " (" + fullTypeName(t) + "): " + types[t] + " items");
    }
}

main();
