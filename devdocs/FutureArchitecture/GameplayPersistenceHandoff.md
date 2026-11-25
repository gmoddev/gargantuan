# Provider-neutral gameplay persistence

The original design handoff below preceded the Foundation 11 implementation.
The [current provider and lifecycle contract](../CurrentArchitecture/GameplayPersistence.md)
describes the candidate behavior. It uses bounded operation polling and
therefore leaves general yielding continuation retirement with its existing
owner. The [Foundation 11 qualification record](../Validation/Foundation11Persistence.md)
tracks the remaining gates against exact source.

The Engine integration should expose a server-only service. The first useful
consumer is a player's inventory, loaded on join and updated with an expected
revision. Player object keys are appropriate for live state. Persistent player
keys must come from the authenticated native provider and subject. Offline
hosts need their own explicit profile namespace.

## Engine contract

* Define a native provider for get, conditional put and delete with bounded
  byte values and opaque revisions. Keep an unavailable provider as the
  default. Select the optional adapter and credentials in trusted host
  configuration. Deliver results on Main through a bounded completion queue.
* Capture the VM generation, Script continuation owner, deadline and operation
  identity before dispatch. Retire pending work on stop or owner cancellation.
  Deliver at most once and ignore stale completions. Depend on the qualified
  Script continuation correction before exposing yielding bindings.
* Snapshot buffer inputs at submission, and return separately owned buffers.
  For document operations admit only finite numbers, booleans, strings, null,
  lists and string-keyed objects, under the smaller Engine/provider bounds.
  Specify a stored value format independently of the Remote protocol. Live
  Instances, peer addresses, userdata and metatables have no durable meaning.
* Expose outcomes for success, absence, revision conflict, denied, unavailable
  before submission, unknown mutation outcome, deadline and cancellation.
  A timed-out write may have committed. Reconcile it by reading the revision.
  An update callback may retry a conflict within a bounded attempt count and
  deadline. It must be safe to run again.
* Cap per-world requests, pending bytes, completions per step and per-owner
  work. Report operation, duration, outcome and bounded correlation metadata.
  Omit keys, values, player subjects and credentials from diagnostics.

## Adapter and acceptance

At Node `cd6294818670f3ddad5a4d93d949c711e495ec22`, the v1 protocol defines
key-value and document stores, authenticated tenant selection and provider
conformance tests. Zero expected revision creates an absent record. A
positive expected revision compares with the current one. Request IDs
correlate calls and cannot deduplicate an unknown write. Implement the
optional adapter against this pinned contract and revalidate it before
integration. [Node contract](https://github.com/gmoddev/gargantuan-node/blob/cd6294818670f3ddad5a4d93d949c711e495ec22/docs/datastore-future-contract.md)

Run one Engine conformance suite against an in-memory fake, an unavailable
provider and the optional adapter. Cover compare-and-swap contention, lost
write acknowledgement, cancel before and after dispatch, owner destruction,
world restart, account isolation, queue pressure and client denial. Retain a
standalone offline startup test with no provider configured.
