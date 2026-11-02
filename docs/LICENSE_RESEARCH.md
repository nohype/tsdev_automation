# License Research - RC-23

Date: 2026-11-02
Status: RESEARCH ONLY. No license chosen. No LICENSE file added. No README reference added. Decision deferred to owner.

## Owner requirements

- R1: Future derivatives must mention (credit) the owner's name (attribution).
- R2: Owner takes NO responsibility for consequences of use (liability disclaimer / no warranty).
- R3: All derivatives of this project must remain open source (copyleft / share-alike on derivatives).
- R4: WE (project owners) want the option to offer future premium versions as closed-source offerings (dual-commercial licensing must stay possible).

## Core tension: R3 vs R4

R3 says "everyone's derivatives stay open." R4 says "we may ship a closed premium." These conflict UNLESS the project uses copyright-holder dual licensing:

- Copyleft (e.g. GPL) binds everyone EXCEPT the copyright holder. The owner, as sole rights holder, is not bound by their own license grant and may re-license the same code under separate commercial terms (the MySQL / Qt model).
- This only works if the owner actually HOLDS all rights. Any outside contribution not owned by the project breaks R4 for the contributed portion, because the owner cannot re-license code they do not own.
- Therefore R3 + R4 jointly REQUIRE a contribution policy from day one: either
  (a) full copyright assignment to the owner, or
  (b) a Contributor License Agreement (CLA) granting the owner an irrevocable, perpetual right to re-license contributions under any terms (including closed commercial), or
  (c) no outside contributions accepted (owner-only copyright).
- Without (a), (b), or (c), the project must pick R3 OR R4, not both. Permissive licenses (MIT/Apache) keep R4 trivially but abandon R3. Strong copyleft without a CLA keeps R3 but kills R4 as soon as one external contribution lands.

Practical note: dual licensing also requires the codebase to be clean of third-party copyleft code that the owner does not own (no GPL snippets from others unless re-licensable). Permissively-licensed dependencies (MIT/Apache) are normally fine; GPL dependencies would infect the open edition.

## Comparison

Legend: YES = satisfies, PARTIAL = satisfies with limits, NO = fails.

### 1. GPL-3.0-only

- R1 attribution: YES (via copyright-notice preservation). GPL-3.0 sec 4-5 require keeping copyright notices intact and marking modifications. There is no trademark-style "must display owner name in UI" clause, but downstream cannot strip the owner's copyright notices. That is the standard copyleft attribution mechanic.
- R2 disclaimer: YES. GPL-3.0 sec 15-16: no warranty, limitation of liability. Explicit.
- R3 copyleft: YES (strong). Any distributed derivative / combined work must be licensed under GPL-3.0. "Tivoization" clause (sec 6) also protects installation rights. Scope covers the whole combined work, not just changed files.
- R4 dual-commercial: YES, CONDITIONAL. Classic MySQL/Qt model: one codebase, GPL edition + paid commercial edition. Precondition: owner holds 100% copyright (assignment or CLA, see above). Effort: moderate (CLA administration, license headers, clean provenance).
- Notes: "GPL-3.0-only" (not "or-later") keeps upgrade control with the owner. Version 3 adds patent grant + anti-tivoization vs v2.

### 2. AGPL-3.0

- R1 attribution: YES. Same copyright-notice mechanics as GPL-3.0.
- R2 disclaimer: YES. Same sec 15-16 warranty/liability language as GPL-3.0.
- R3 copyleft: YES (strongest of the set). Everything GPL covers, PLUS sec 13 network-use trigger: offering the work over a network (SaaS) counts as distribution and triggers source disclosure. Closes the "run modified GPL code on a server without sharing" loophole.
- R4 dual-commercial: YES, CONDITIONAL. Same dual-license model as GPL, same 100%-copyright precondition (assignment/CLA). Commercial value proposition is arguably stronger (customers pay to avoid the network-disclosure duty), but community adoption friction is higher -- many companies ban AGPL code outright, which can reduce contributions and integration.
- Notes: choose over GPL-3.0-only only if hosted/network use of this tool is a realistic circumvention path for R3. For a local Windows CLI/automation tool, that threat is low, so AGPL buys little over GPL while costing adoption.

### 3. MPL-2.0 (Mozilla Public License 2.0)

- R1 attribution: PARTIAL. Requires preserving copyright notices (sec 3.4) but, like GPL, no prominent-credit clause.
- R2 disclaimer: YES. Sec 6: no warranty; sec 7: liability limitation. Explicit.
- R3 copyleft: PARTIAL (file-level / "weak" copyleft). Modified MPL files must stay MPL, but new files combined into a Larger Work may be under other terms -- INCLUDING proprietary terms (sec 3.3). A competitor can add closed files around the open core. Does NOT satisfy R3 as stated ("ALL derivatives remain open").
- R4 dual-commercial: YES, EASY. File-level scope plus sec 3.3 Larger-Work allowance means closed premium editions are straightforward even without full copyright consolidation. Precondition lighter than GPL.
- Verdict on requirements: FAILS R3. Disqualified as a standalone answer to this brief.

### 4. Apache-2.0

- R1 attribution: YES (strongest notice mechanic of the set). Sec 4 requires preserving copyright/attribution notices and NOTICE file; modified files must carry change notices. Still not a forced UI credit, but the best paper trail of the permissive options. Includes patent grant (sec 3) and patent-retaliation (sec 3).
- R2 disclaimer: YES. Sec 7-8: no warranty, liability limitation. Explicit.
- R3 copyleft: NO. Permissive. Derivatives (open or closed) allowed with only notice preservation. Does NOT satisfy R3.
- R4 dual-commercial: YES, TRIVIAL. Owner (and anyone else) can ship closed premium at any time; no CLA needed for R4 alone.
- Verdict: FAILS R3. Disqualified as a standalone answer.

### 5. MIT

- R1 attribution: PARTIAL (minimal). Only requires keeping the copyright notice + permission notice in copies. No change-marking, no NOTICE-file machinery, no patent grant.
- R2 disclaimer: YES. "AS IS ... IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE" -- explicit no-warranty/liability text.
- R3 copyleft: NO. Permissive, like Apache but simpler. Fails R3.
- R4 dual-commercial: YES, TRIVIAL. Same as Apache.
- Verdict: FAILS R3 (and gives weaker R1/R2 machinery than Apache-2.0). Disqualified as a standalone answer.

### 6. Dual-license model (open copyleft + commercial), e.g. MySQL / Qt

- This is not a single license; it is a BUSINESS configuration: License A (open, strong copyleft: normally GPL-3.0-only or AGPL-3.0) for the community edition + License B (paid, proprietary) for premium customers. Same code, two doors.
- R1: YES (inherited from License A mechanics + commercial contract can add explicit credit/branding terms).
- R2: YES (inherited disclaimers in License A + negotiated caps in License B contracts).
- R3: YES (as to the community line -- License A enforces it on all third parties).
- R4: YES -- that is the entire point of the model. The owner sells an exception to License A.
- Precondition (hard): owner must be able to grant License B. Requires ONE of: owner-only authorship, copyright assignment, or a CLA with re-licensing rights, BEFORE accepting outside contributions. Retroactive cleanup (re-implementing or re-licensing contributed code) is expensive; decide before the first external PR.
- Cost: CLA friction (some contributors refuse assignment), need for license headers on every file, provenance tracking, and refusal of inbound GPL code owned by third parties (it cannot go into the commercial edition).
- Variants: GPL+commercial (broadest compatibility, standard choice); AGPL+commercial (max SaaS capture, max adoption friction); "open core" (open core under GPL/MPL + closed add-ons) -- note open-core WITHOUT full-copyright consolidation only protects the core, not the add-ons' openness, so pair it with a CLA if R3 must cover everything.

## Summary matrix

| License/model        | R1 attribution        | R2 disclaimer | R3 derivatives stay open | R4 closed premium possible |
|----------------------|-----------------------|---------------|--------------------------|----------------------------|
| GPL-3.0-only         | YES (notice keep)     | YES           | YES (strong)             | YES if 100% copyright/CLA  |
| AGPL-3.0             | YES (notice keep)     | YES           | YES (strongest, +network)| YES if 100% copyright/CLA  |
| MPL-2.0              | PARTIAL (notice keep) | YES           | PARTIAL (file-level)     | YES (easy)                 |
| Apache-2.0           | YES (NOTICE + marking)| YES           | NO (permissive)          | YES (trivial)              |
| MIT                  | PARTIAL (minimal)     | YES           | NO (permissive)          | YES (trivial)              |
| Dual GPL+commercial  | YES                   | YES           | YES (community line)     | YES (its purpose)          |

Notes:
- R1 nuance: NO OSI license reviewed here forces downstream to display the owner name in product UI/marketing; all satisfy R1 only through copyright-notice preservation (+ Apache NOTICE). If the owner wants a forced visible credit or name-use rule, that needs an additional trademark/branding policy outside the copyright license.
- R2 nuance: all six options contain an explicit no-warranty / liability-limitation text. R2 does not discriminate.
- The discriminator is R3 vs R4: only strong copyleft (GPL/AGPL) satisfies R3, and only with copyright consolidation does it preserve R4.

## Recommendation

1. BEST FIT: GPL-3.0-only for the open edition + paid commercial license (MySQL/Qt dual-license model), with a CLA (or copyright assignment) in place before any outside contribution. Why: it is the only surveyed configuration satisfying all four requirements simultaneously with the lowest adoption friction among the strong-copyleft options.
2. RUNNER-UP: AGPL-3.0 + commercial dual license, same CLA precondition. Why: identical R1/R2/R4, strictly stronger R3 over networks -- take it instead of option 1 only if hosted/SaaS circumvention of R3 is a real threat; otherwise its corporate-ban friction outweighs the gain for a local Windows CLI tool.

Immediate prerequisites (no license decision yet, but needed to keep both options open):
- Accept no outside contributions until a CLA/assignment process exists, or R4 may be irreversibly lost for contributed code.
- Keep third-party GPL/AGPL code out of the tree; prefer MIT/Apache dependencies to avoid contaminating the future commercial edition.
- When the owner decides, record the exact SPDX identifier (e.g. `GPL-3.0-only`) and add headers + LICENSE file then -- explicitly out of scope for this research task.
