// ell_presence_poll.h
//
// Bounded poll-with-timeout for the post-verify .ell/.xml presence checks
// (Slice-2 hardening #3 + follow-up).
//
// Background: doInsert steps 5-6 (and the two modify post-verify checks)
// in src/tsdev_cli.cpp made a SINGLE ellFileExists()/xmlFileExists() call
// after verifyModule succeeded and failed loudly (exit 9) on a miss. The
// .ell writer is MDVault::SaveModule inside verifyModule
// (src/tsdev_lib.cpp), whose OS-level write can lag or fail while ELMDCore
// still reports success (Fault A family); the .xml artifact is written by
// the post-verify finalization path with the same lag exposure. A single
// immediate check can therefore false-negative on a transient that
// resolves a moment later.
//
// Fix: check-then-poll with a hard bound. The first check runs immediately
// (zero added latency when the file is already present -- the common case);
// only on a miss does the caller sleep kEllPresencePollIntervalMs and
// re-check, up to kEllPresencePollMaxSleeps sleeps (~10 s total). If the
// file is still absent after the budget, the caller fails LOUDLY with the
// unchanged exit-9 contract (same exit code, same message shape). The
// timeout never masks Fault A; it only absorbs the lagging-write transient.
//
// Scope: the four post-verify presence gates (doInsert steps 5-6 and the
// two modify post-verify checks, all identical single-shot shape). The
// pre-operation snapshot check (which only feeds the pre-op hash lambda,
// not a post-verify gate) is intentionally unchanged.
//
// The seam is dependency-free (header-only templates, no includes) so the
// pure unit test tests/test_ell_presence_poll.cpp can drive it with fake
// predicates and counting sleeps -- no orlib20, no TsDevLib, no TS install.

#ifndef TSDEV_ELL_PRESENCE_POLL_H
#define TSDEV_ELL_PRESENCE_POLL_H

namespace tsdev {

// Presence-poll budget: 500 ms interval, up to 20 sleeps -> ~10 s bound.
inline constexpr unsigned kEllPresencePollIntervalMs = 500;
inline constexpr int kEllPresencePollMaxSleeps = 20;

// pollUntilPresent: check-then-poll until exists() is true or the sleep
// budget is exhausted. At most maxSleeps sleeps and maxSleeps+1 exists()
// calls (bounded: no infinite wait). Returns true on the first present
// observation. checksOut (optional) receives the exists() call count.
template <typename ExistsFn, typename SleepFn>
inline bool pollUntilPresent(ExistsFn exists, SleepFn sleepFn, int maxSleeps,
                             int* checksOut = nullptr) {
    int checks = 0;
    for (int i = 0; i <= maxSleeps; ++i) {
        ++checks;
        if (exists()) {
            if (checksOut) {
                *checksOut = checks;
            }
            return true;
        }
        if (i < maxSleeps) {
            sleepFn();
        }
    }
    if (checksOut) {
        *checksOut = checks;
    }
    return false;
}

}  // namespace tsdev

#endif  // TSDEV_ELL_PRESENCE_POLL_H
