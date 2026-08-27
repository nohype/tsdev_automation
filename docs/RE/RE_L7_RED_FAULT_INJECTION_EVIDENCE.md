# TVS-L.7 RED Fault-Injection Evidence — Historical/Transient Recorded Run

> This tracked file records one historical/transient recorded run only; it is not a current run and not current production-acceptance proof.
> Exact ProgIds, hashes, exit codes, and outcomes below are unverified outside that recorded run.
> Routine `-IncludeRedFaultInjection` runs write run-specific gitignored `tmp/` evidence; `-UpdateTrackedEvidence` authorizes selecting this tracked destination, but an existing destination is never overwritten.
> This evidence document is assembled after cleanup. Operation-state measurements are retained as recorded and are explicitly measurements from before sanitation; the legacy fields identified below were not captured in the required order.
> Qualification: the legacy `*ExistedBeforeSanitation`, `OrphanExistedBeforeSanitation`, `Read-only attribute restored in finally`, and `Cleanup verdict (pre-sanitation)` fields below were populated after restore/delete by the old ordering. They are invalid as pre-sanitation evidence and are not acceptance evidence. No replacement values are inferred here.
> The current harness records guarded pre-sanitation production state, sanitation actions, and post-sanitation hygiene as separate fields. This historical record does not contain those correctly ordered fields; the per-case placeholders below remain explicitly unverified rather than reconstructing values.
> Textual stdout/stderr in this retained/generated Markdown is PowerShell-rendered and may contain encoding artifacts; it is not a byte-exact transcript. Underlying native bytes are not verified.
> Produced by `tests/test_unknown_link_regression.ps1 -IncludeRedFaultInjection`.
> Handoff §6 Evidence: exit codes, hashes, GUID occurrence counts, get-source output,
> read-only-command capture, and invariant failures. Raw logs remain transient.
> Generated: 2026-08-24T11:35:31.9100338+02:00

## L7.1 -- Modify / read-only ELL (Fault A: stale-ELL modify; detect-and-fail; historical/transient recorded run)

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

### Operation measurements captured before sanitation (pre/post-operation ELL/XML SHA-256 hashes)
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

### Production operation desired-state invariant result (recorded before finally sanitation)
- Production desired-state passed: **True**
- Invariant failures: 

### Legacy pre-sanitation fields (invalid; old ordering; not acceptance evidence)
- ELL existed before sanitation: **UNVERIFIED (legacy value retained: False; old ordering; not a measurement)**
- XML existed before sanitation: **UNVERIFIED (legacy value retained: False; old ordering; not a measurement)**
- Orphan existed before sanitation: **UNVERIFIED (legacy value retained: False; old ordering; not a measurement)**
- Read-only attribute restored in finally: **UNVERIFIED (legacy value retained: True; captured after the old ordering; not a measurement)**
- Cleanup verdict (pre-sanitation): **UNVERIFIED (legacy value retained: True; computed after the old ordering; not a measurement)**

### Correctly ordered sanitation lifecycle fields
- Pre-sanitation guarded production snapshot (ELL/XML existence, read-only state, SHA-256, warehouse state): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.
- Sanitation-action fields (read-only restoration, sanitation delete, and artifact-removal outcomes): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.
- Post-sanitation hygiene fields (warehouse/artifact presence, read-only state, and hashes): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.

---

## L7.2 -- Upsert-existing / read-only ELL (Fault A variant; detect-and-fail; historical/transient recorded run)

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

### Operation measurements captured before sanitation (pre/post-operation ELL/XML SHA-256 hashes)
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

### Production operation desired-state invariant result (recorded before finally sanitation)
- Production desired-state passed: **True**
- Invariant failures: 

### Legacy pre-sanitation fields (invalid; old ordering; not acceptance evidence)
- ELL existed before sanitation: **UNVERIFIED (legacy value retained: False; old ordering; not a measurement)**
- XML existed before sanitation: **UNVERIFIED (legacy value retained: False; old ordering; not a measurement)**
- Orphan existed before sanitation: **UNVERIFIED (legacy value retained: False; old ordering; not a measurement)**
- Read-only attribute restored in finally: **UNVERIFIED (legacy value retained: True; captured after the old ordering; not a measurement)**
- Cleanup verdict (pre-sanitation): **UNVERIFIED (legacy value retained: True; computed after the old ordering; not a measurement)**

### Correctly ordered sanitation lifecycle fields
- Pre-sanitation guarded production snapshot (ELL/XML existence, read-only state, SHA-256, warehouse state): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.
- Sanitation-action fields (read-only restoration, sanitation delete, and artifact-removal outcomes): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.
- Post-sanitation hygiene fields (warehouse/artifact presence, read-only state, and hashes): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.

---

## L7.3 -- Delete / read-only ELL (Fault B: orphan ELL false success; detect-and-report; historical/transient recorded run)

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

### Operation measurements captured before sanitation (pre/post-operation ELL/XML SHA-256 hashes)
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

### Warehouse / XML / ELL presence after production delete, before sanitation (Fault B)
- Warehouse entry present after delete: False
- XML present after delete: False
- ELL present after delete (orphan): True
- Exact `failed to delete ... error=5` line captured: `tsdev: WARNING ΓÇö failed to delete C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin\elmodule_0_7679.ell (error=5)`

### Production operation desired-state invariant result (recorded before finally sanitation)
- Production desired-state passed: **True**
- Invariant failures: 

### Legacy pre-sanitation fields (invalid; old ordering; not acceptance evidence)
- ELL existed before sanitation: **UNVERIFIED (legacy value retained: True; old ordering; not a measurement)**
- XML existed before sanitation: **UNVERIFIED (legacy value retained: False; old ordering; not a measurement)**
- Orphan existed before sanitation: **UNVERIFIED (legacy value retained: True; old ordering; not a measurement)**
- Read-only attribute restored in finally: **UNVERIFIED (legacy value retained: True; captured after the old ordering; not a measurement)**
- Cleanup verdict (pre-sanitation): **UNVERIFIED (legacy value retained: False; computed after the old ordering; not a measurement)**

### Correctly ordered sanitation lifecycle fields
- Pre-sanitation guarded production snapshot (ELL/XML existence, read-only state, SHA-256, warehouse state): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.
- Sanitation-action fields (read-only restoration, sanitation delete, and artifact-removal outcomes): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.
- Post-sanitation hygiene fields (warehouse/artifact presence, read-only state, and hashes): **UNVERIFIED** — not captured in this historical run; no replacement values inferred.

---

