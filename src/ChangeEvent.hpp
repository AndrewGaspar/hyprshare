#pragma once

// The `hyprshare>>changed` IPC event (socket2), for status bars.
//
// notifyChanged() may be called from anywhere, as often as is convenient (every capture request,
// every tick): it only schedules one check per event-loop iteration (doLater). That check compares a
// fingerprint of everything `sharestatus` reports that a bar cares about (capturing, sessions and
// their sharing flags, share mode, target, pause, per-capture state, the shared window's identity
// and title) with the last one posted, and posts `hyprshare>>changed` only if it differs. So: at most
// one event per iteration, none for no-op calls, and a re-query of `sharestatus` after each event
// always sees the state that triggered it.

namespace HyprShare {
    void notifyChanged();
    // Record the current state as already posted (at plugin init), so loading posts nothing.
    void primeChanged();
}
