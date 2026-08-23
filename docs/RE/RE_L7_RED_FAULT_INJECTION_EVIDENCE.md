# TVS-L.7 RED Fault-Injection Evidence

> Tracked durable summary produced by `tests/test_unknown_link_regression.ps1 -IncludeRedFaultInjection`.
> Handoff §6 Evidence: exit codes, hashes, GUID occurrence counts, get-source output,
> read-only-command capture, and invariant failures. Raw logs remain transient.
> Generated: 2026-08-24T11:35:31.9100338+02:00

## L7.1 -- Modify / read-only ELL (Fault A: stale-ELL modify; detect-and-fail; GREEN after L.9)

- **Disposable item identifier:** TS_AUTO_L7_MOD_RO_20260824_113226
- **Component type:** strategy
- **ProgId:** 7677
- **Operation:** modify

### Read-only-attribute command capture
- Command: `attrib +R "C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin\elmodule_0_7677.ell"`
- Exit code: 0
- IsReadOnly after: True
- Stdout: ``
- Stderr: ``

### Pre/post-operation ELL/XML SHA-256 hashes
| Artifact | Pre SHA-256 | Post SHA-256 | Changed |
|----------|-------------|--------------|---------|
| ELL | 98CB0271CFDC8A70B6B4E6D2C797569EA41FABB9B8C1B63F387DF9ACF7A748A1 | 98CB0271CFDC8A70B6B4E6D2C797569EA41FABB9B8C1B63F387DF9ACF7A748A1 | False |
| XML | 5C49FC712ED9AE65283B648F11E1C04F8A3D4C1730A3FE5B0CC738D56D877823 | F8A870EA06F755F7520E33FEB196818497CD4CFE655852EDA859DB69E1AC25A9 | True |

### New XML GUID and occurrence count in post-operation ELL (Fault A)
- New XML GUID: `{666D9060-AC9E-4C8A-8242-20FA6A159DFA}`
- GUID occurrence count in post-op ELL: 0

### get-source return value (Fault A)
- get-source exit code: 0
- get-source JSON success: True
- get-source returned source length: 57
- get-source returned source matches NEW (modified) source: True
- get-source returned source matches OLD (pre-modify) source: False

### CLI exit / JSON success / stderr / stdout
- Exit code: 9
- JSON success: False
- Stderr: `tsdev: TS install path resolved via registry (Installing): C:\Program Files (x86)\TradeStation 9.5`
- Stdout: `{"op":"modify","type":"strategy","name":"TS_AUTO_L7_MOD_RO_20260824_113226","success":false,"exit_code":9,"error":{"code":"artifact_missing","message":"persistence integrity check FAILED ΓÇö stale .ell (Fault A): the .ell hash is unchanged or the post-op XML GUID is not present exactly once (mixed-endian) in the .ell; the artifacts are inconsistent and the item is runtime-unlinkable [verdict=FaultA_StaleEll, type=strategy, name=\"TS_AUTO_L7_MOD_RO_20260824_113226\", ProgId=7677, .ell=C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_7677.ell, .xml=C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_7677.xml]"}}`

### Desired-state invariant result
- Desired-state passed: **True**
- Invariant failures: 

### Production vs sanitation state before cleanup
- ELL existed before sanitation: False
- XML existed before sanitation: False
- Orphan existed before sanitation: False
- Read-only attribute restored in finally: True
- Cleanup verdict (pre-sanitation): True

---

## L7.2 -- Upsert-existing / read-only ELL (Fault A variant; detect-and-fail; GREEN after L.9)

- **Disposable item identifier:** TS_AUTO_L7_UPS_RO_20260824_113226
- **Component type:** strategy
- **ProgId:** 7678
- **Operation:** upsert

### Read-only-attribute command capture
- Command: `attrib +R "C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin\elmodule_0_7678.ell"`
- Exit code: 0
- IsReadOnly after: True
- Stdout: ``
- Stderr: ``

### Pre/post-operation ELL/XML SHA-256 hashes
| Artifact | Pre SHA-256 | Post SHA-256 | Changed |
|----------|-------------|--------------|---------|
| ELL | 2B3126F157DB243919BF83E52A48C3CA5C6A8E0F5B063CD721600F58AFDA1A9C | 2B3126F157DB243919BF83E52A48C3CA5C6A8E0F5B063CD721600F58AFDA1A9C | False |
| XML | 446DE7EAB9123A00B648476A5C178A1B4103812A6A1551DD9CD171F5488B9991 | 5B76D00F97C18AD4DEA1C98DE449807CA9027B1BAF528A5FAD32CE9010FC0E6F | True |

### New XML GUID and occurrence count in post-operation ELL (Fault A)
- New XML GUID: `{46712454-6FCC-40D7-A46E-CBDE1691BBF3}`
- GUID occurrence count in post-op ELL: 0

### get-source return value (Fault A)
- get-source exit code: 0
- get-source JSON success: True
- get-source returned source length: 57
- get-source returned source matches NEW (modified) source: True
- get-source returned source matches OLD (pre-modify) source: False

### CLI exit / JSON success / stderr / stdout
- Exit code: 9
- JSON success: False
- Stderr: `tsdev: TS install path resolved via registry (Installing): C:\Program Files (x86)\TradeStation 9.5`
- Stdout: `{"op":"upsert","type":"strategy","name":"TS_AUTO_L7_UPS_RO_20260824_113226","success":false,"exit_code":9,"error":{"code":"artifact_missing","message":"persistence integrity check FAILED ΓÇö stale .ell (Fault A): the .ell hash is unchanged or the post-op XML GUID is not present exactly once (mixed-endian) in the .ell; the artifacts are inconsistent and the item is runtime-unlinkable [verdict=FaultA_StaleEll, type=strategy, name=\"TS_AUTO_L7_UPS_RO_20260824_113226\", ProgId=7678, .ell=C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_7678.ell, .xml=C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_7678.xml]"}}`

### Desired-state invariant result
- Desired-state passed: **True**
- Invariant failures: 

### Production vs sanitation state before cleanup
- ELL existed before sanitation: False
- XML existed before sanitation: False
- Orphan existed before sanitation: False
- Read-only attribute restored in finally: True
- Cleanup verdict (pre-sanitation): True

---

## L7.3 -- Delete / read-only ELL (Fault B: orphan ELL false success; detect-and-report; GREEN after L.12)

- **Disposable item identifier:** TS_AUTO_L7_DEL_RO_20260824_113226
- **Component type:** strategy
- **ProgId:** 7679
- **Operation:** delete

### Read-only-attribute command capture
- Command: `attrib +R "C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin\elmodule_0_7679.ell"`
- Exit code: 0
- IsReadOnly after: True
- Stdout: ``
- Stderr: ``

### Pre/post-operation ELL/XML SHA-256 hashes
| Artifact | Pre SHA-256 | Post SHA-256 | Changed |
|----------|-------------|--------------|---------|
| ELL | 4D641712ABB9A3356302769C87ABFC229A0BBE0F6EB24AC74CE17174C7F53A5C | 4D641712ABB9A3356302769C87ABFC229A0BBE0F6EB24AC74CE17174C7F53A5C | False |
| XML | 30EC748B125EFAA786F61D6E658A5BE3C396D75E5BD53064A8DADD6AD3ADFA99 |  | n/a |

### CLI exit / JSON success / stderr / stdout
- Exit code: 6
- JSON success: False
- Stderr: `tsdev: TS install path resolved via registry (Installing): C:\Program Files (x86)\TradeStation 9.5
tsdev: WARNING ΓÇö failed to delete C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin\elmodule_0_7679.ell (error=5)`
- Stdout: `{"op":"delete","type":"strategy","name":"TS_AUTO_L7_DEL_RO_20260824_113226","success":false,"exit_code":6,"error":{"code":"entry_op_failure","message":"deleteEntry partially failed for strategy \"TS_AUTO_L7_DEL_RO_20260824_113226\" (ProgId=7679): warehouse entry was removed, but disk cleanup failed ΓÇö failed to delete C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_7679.ell (Windows error=5)"}}`

### Warehouse / XML / ELL presence after delete (Fault B)
- Warehouse entry present after delete: False
- XML present after delete: False
- ELL present after delete (orphan): True
- Exact `failed to delete ... error=5` line captured: `tsdev: WARNING ΓÇö failed to delete C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin\elmodule_0_7679.ell (error=5)`

### Desired-state invariant result
- Desired-state passed: **True**
- Invariant failures: 

### Production vs sanitation state before cleanup
- ELL existed before sanitation: True
- XML existed before sanitation: False
- Orphan existed before sanitation: True
- Read-only attribute restored in finally: True
- Cleanup verdict (pre-sanitation): False

---

