// Frida trace script — trace LoadList internals to find where enumeration fails.
function resolveOrdinal(modName, ordinal) {
    var mod = Process.findModuleByName(modName);
    var base = mod.base;
    var peOff = base.add(0x3c).readU32();
    var optHdr = base.add(peOff).add(24);
    var exportDirRva = optHdr.add(96).readU32();
    var exportDirSize = optHdr.add(100).readU32();
    var exportDir = base.add(exportDirRva);
    var ordinalBase = exportDir.add(16).readU32();
    var addrFuncs = base.add(exportDir.add(28).readU32());
    var fnRva = addrFuncs.add((ordinal - ordinalBase) * 4).readU32();
    return base.add(fnRva);
}

function main() {
    var ORELUI = "ORELUI20.dll";
    var ORLIB = "ORLIB20.dll";
    var orelMod = Process.findModuleByName(ORELUI);
    var orlibMod = Process.findModuleByName(ORLIB);
    var base = orelMod.base;
    console.log("[*] ORELUI20.dll base = " + base);
    console.log("[*] ORLIB20.dll base = " + (orlibMod ? orlibMod.base : "NOT FOUND"));

    var pCtor = resolveOrdinal(ORELUI, 17);
    var pLoadList = resolveOrdinal(ORELUI, 1291);
    var pDtor = resolveOrdinal(ORELUI, 166);
    var pCatiCtor = base.add(0x44820);

    // Internal functions to trace
    var pCatiInit = base.add(0x44ab0);     // FUN_10044ab0 — init type-info container
    var pEnumAll = base.add(0x74470);      // FUN_10074470 — enumerate all types
    var pEnumType = base.add(0x74310);     // FUN_10074310 — enumerate one type
    var pAppListInit = base.add(0x72eb0);  // FUN_10072eb0 — init CELApplicationList
    var pGetItemCount = base.add(0x73590); // guess: CELApplicationList::GetItemCount (need to find)
    var pPopulateTi = base.add(0x44b90);   // FUN_10044b90 — populate CELTechInfo

    // ORLIB20 ordinals
    var pOrdinal10 = orlibMod ? resolveOrdinal(ORLIB, 10) : null; // get library object

    console.log("[*] Ordinal_10 (ORLIB20) = " + pOrdinal10);
    console.log("[*] FUN_10044ab0 (cati init) = " + pCatiInit);
    console.log("[*] FUN_10074470 (enum all) = " + pEnumAll);
    console.log("[*] FUN_10074310 (enum type) = " + pEnumType);
    console.log("[*] FUN_10072eb0 (appList init) = " + pAppListInit);
    console.log("[*] FUN_10044b90 (populate TI) = " + pPopulateTi);

    // --- Hook internal functions ---
    // Hook FUN_10044ab0 (cati init)
    Interceptor.attach(pCatiInit, {
        onEnter: function(args) {
            console.log("  [TRACE] FUN_10044ab0(cati init) called, this=" + this.context.ecx);
        },
        onLeave: function(retval) {
            console.log("  [TRACE] FUN_10044ab0 returned");
        }
    });

    // Hook Ordinal_10 (get library)
    if (pOrdinal10) {
        Interceptor.attach(pOrdinal10, {
            onEnter: function(args) {
                console.log("  [TRACE] Ordinal_10(0) called, arg=" + args[0]);
            },
            onLeave: function(retval) {
                console.log("  [TRACE] Ordinal_10 returned " + retval);
            }
        });
    }

    // Hook FUN_10072eb0 (appList init)
    Interceptor.attach(pAppListInit, {
        onEnter: function(args) {
            console.log("  [TRACE] FUN_10072eb0(appList init) called, arg=" + args[0]);
        }
    });

    // Hook FUN_10074470 (enum all)
    Interceptor.attach(pEnumAll, {
        onEnter: function(args) {
            // __thiscall: ECX=lib, stack: [type, &appList]
            console.log("  [TRACE] FUN_10074470(enum all) called, ECX(lib)=" + this.context.ecx + " type=" + args[0] + " appList=" + args[1]);
        },
        onLeave: function(retval) {
            console.log("  [TRACE] FUN_10074470 returned " + retval);
        }
    });

    // Hook FUN_10074310 (enum one type)
    Interceptor.attach(pEnumType, {
        onEnter: function(args) {
            console.log("  [TRACE] FUN_10074310(enum type) called, ECX(lib)=" + this.context.ecx + " type=" + args[0] + " appList=" + args[1]);
        },
        onLeave: function(retval) {
            console.log("  [TRACE] FUN_10074310 returned " + retval);
        }
    });

    // Hook FUN_10044b90 (populate CELTechInfo)
    Interceptor.attach(pPopulateTi, {
        onEnter: function(args) {
            console.log("  [TRACE] FUN_10044b90(populate TI) called, ti=" + args[0] + " cat=" + args[1] + " type=" + args[2]);
        }
    });

    // --- Now run the enumeration ---
    var mapBuf = Memory.alloc(0x40);
    for (var i = 0; i < 0x40; i += 4) { mapBuf.add(i).writeU32(0); }
    var dlgBuf = Memory.alloc(0x400);
    for (var i = 0; i < 0x400; i += 4) { dlgBuf.add(i).writeU32(0); }
    var catiBuf = Memory.alloc(0x28);
    for (var i = 0; i < 0x28; i += 4) { catiBuf.add(i).writeU32(0); }

    var catiCtor = new NativeFunction(pCatiCtor, 'void', ['pointer'], 'thiscall');
    try { catiCtor(catiBuf); } catch (e) { console.log("cati ctor err: " + e.message); }

    var ctor = new NativeFunction(pCtor, 'void', ['pointer', 'pointer', 'pointer'], 'thiscall');
    try { ctor(dlgBuf, mapBuf, ptr(0)); console.log("[+] dialog ctor OK"); }
    catch (e) { console.log("[-] dialog ctor: " + e.message); return; }

    console.log("[*] === Calling LoadList type=0 ===");
    var loadList = new NativeFunction(pLoadList, 'void', ['pointer', 'int', 'pointer'], 'thiscall');
    try {
        loadList(dlgBuf, 0, catiBuf);
        console.log("[+] LoadList OK");
    } catch (e) { console.log("[-] LoadList: " + e.message); }

    // Check CMap with corrected offsets
    console.log("[*] Cati CMap at +0xc (corrected offsets):");
    var cmapBase = catiBuf.add(0x0c);
    console.log("  vtable=" + cmapBase.readPointer());
    console.log("  m_pHashTable=" + cmapBase.add(0x04).readPointer());
    console.log("  m_nHashTableSize=" + cmapBase.add(0x08).readU32());
    console.log("  m_nCount=" + cmapBase.add(0x0c).readU32());

    console.log("[*] Dialog CMap at +0x3bc (corrected offsets):");
    var cmapBase2 = dlgBuf.add(0x3bc);
    console.log("  vtable=" + cmapBase2.readPointer());
    console.log("  m_pHashTable=" + cmapBase2.add(0x04).readPointer());
    console.log("  m_nHashTableSize=" + cmapBase2.add(0x08).readU32());
    console.log("  m_nCount=" + cmapBase2.add(0x0c).readU32());

    var dtor = new NativeFunction(pDtor, 'void', ['pointer'], 'thiscall');
    try { dtor(dlgBuf); } catch (e) {}
    console.log("[*] DONE");
}

main();
