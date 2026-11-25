# Persistent quest profile

Install `Profile.luau` as a server ModuleScript named `Profile`. Install
`Server.luau` next to it as an ordinary server Script. Enable local Play data
or configure a dedicated Server provider. Both use the same scripts. The
sample stores a versioned inventory, currency, settings, and a one-time quest
reward marker in one document. The fixed server-authored development key
deliberately lets local reconnects and independent test servers see the same
record. A real project supplies a trusted, authenticated profile key.

1. Start Disposable Play. The first admission logs `Granted`, 25 currency and
   one item. A second Disposable play starts a new store.
2. Start Persistent Play, stop, and start again. The second admission logs
   `AlreadyGranted` with the same payload. Inspect the exact Document record
   in Studio under `PersistentQuestProfiles` and `DevelopmentExplorer`.
3. Run two test servers using the same isolated Node DataStore tenant and
   PostgreSQL database. The first successful compare-and-swap grants the
   reward. The competing operation reads fresh after a conflict and observes
   the stored marker. A restart or reconnect leaves currency and inventory
   unchanged.
4. Run without a provider or pause a test provider. Expect an explicit
   `Unavailable` or deadline outcome. When a fixture drops a write response
   after commit, the sample reads the same profile and accepts
   `Reconciled` only after finding the marker. An inconclusive read leaves
   `UnknownOutcome` and submits no repeat mutation.

The [public API](../../docs/src/content/docs/api/networking-and-persistence.mdx)
and [deployment contract](../../devdocs/CurrentArchitecture/GameplayPersistence.md)
describe provider selection, limits, and safe cancellation.
