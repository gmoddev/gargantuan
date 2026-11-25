---
status: current
owner: runtime
related_code:
  - src/services/TweenService.cpp
  - src/classes/Tween.cpp
  - src/scripting/ScriptEngine.cpp
  - assets/services/TweenService.luau
---

# Property tween runtime

The protected DataModel service is a reflected entry point. Each ScriptEngine
allocates a scheduler on first use. Tween handles are VM-local shared userdata.
They carry immutable goal values, the target generation-safe ObjectId, and a
weak scheduler reference. Active playback holds the handle until termination.
Neither playback nor its pending completion holds the target or the creating
Script alive. Runtime handles and playback state have no GAS or wire format.

Engine advances the scheduler once on Main before `PreSimulation`. Each
playback gets a monotonic sequence and claims its target properties. Starting
a fresh playback validates and snapshots each property before replacing any
existing claims. It cancels older overlapping playbacks in sequence order,
including paused or delayed playbacks. Assigning a property outside TweenService
leaves the tween active and the next sample can write the property again.
A client domain cannot replace an authoritative playback within a shared
offline VM.
Different property names claim separate slots even if their native setters
affect shared semantic state, such as a position alias and the corresponding
transform. Use one authoritative transform goal for such properties.

At each step, the scheduler advances only sequences admitted before the step
began. It resolves the target against the current DataModel and uses the
original Play context for reflected reads and ordinary property mutation.
Resuming revalidates with the current caller context in the same domain.
Client access follows the current local PlayerGui owner, and the server
retains authority over replicated storage and StarterGui templates.
Mutation preserves existing authority, setter validation, revision, journals,
replication, physics, and notifications. Each sample prevalidates the
reflected values, then commits properties alphabetically through the normal
individual setter contracts. A state-dependent setter rejection or reentrant
cancellation can leave the accepted prefix in place. Easing curves such as
Back or Elastic can overshoot; bounds and validators still apply.

Terminal state and release of all claims precede completion dispatch. The
bounded completion queue is FIFO, and a callback that queues another completion
defers its dispatch to a later step. A synchronous property callback may
pause, cancel, replace or destroy the current playback while a setter is on
the stack. A retained handle protects the in-flight native call, and the
scheduler checks live sequence, state and target after each setter. Shutdown
stops dispatch and discards all remaining work before the ScriptEngine
disconnects Lua signal handlers.

A Tween created on a managed Script's original thread retains a weak Script
owner. Destruction retires its work at the next tween step and suppresses
its pending completions. A completed original coroutine can leave valid
tween playback running while its Script remains live. Creation from callbacks
and other threads without a managed owner uses target and VM lifetime. Broader
Script ownership across continuations remains part of the independent runtime
lifecycle work.

The scheduler bounds goals at 16 and total active plus pending playback work
at 1,024 per VM. Repeats and delays use elapsed-time arithmetic, so a large
delta never loops through missed cycles. The opt-in `TweenStep` work trace
reports elapsed time, sampled playback count and rejected sample/dispatch
count. The trace carries no target names, property values, or script text.
