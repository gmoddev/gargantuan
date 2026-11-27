# D05: Private Team Create authoring session

Status: experimental Foundation 1 candidate.

## Context

The earlier [Studio design](../../devdocs/FutureArchitecture/GuiAndStudio.md)
reserves a single ordered authoring history for eventual collaboration.
[Architectural boundaries](../../devdocs/FutureArchitecture/ArchitecturalDisciplineAndSubsystemBoundaries.md)
separate RemoteCollaborator attribution from future selective undo. This
decision makes those directions concrete for a private engineering slice.

## Decision

One Studio-owned private broker opens a project in one Engine EditorHost.
EditorHost owns the DataModel, frozen schema, object identity, revisions,
commits, history, source, and saves. Studio holds a disposable projection.
Offline Studio continues to use the direct child-process adapter and existing
request shape. Collaboration adds an optional transport to that same adapter.

The broker assigns one owner and up to seven editor seats. It binds IPv4
loopback, pairs a bounded invite with a session ID, and keeps private resume
tickets in the client adapter. Admission establishes the author. The broker
supplies this identity to the private EditorHost stream and applies its
method allowlist. Start and attach are explicit launch modes. The broker
holds one project lock among Team Create brokers until session retirement.

Accepted ordinary property, attribute, extension or custom field writes
commit in arrival order at the existing gateway. An explicit compare request
can retain the project revision precondition where the underlying command
supports it. Structural commands, prepared
batches, and compound transforms keep strict admission. Script commits use
the exact source version. The committed journal is the single catch-up
ordering. Each author has an independent snapshot anchor and cursor.
Team pages contain whole commits. A stale anchor or an oversized commit
requires a bounded resnapshot.

The existing history retains author metadata. Undo and redo advance its
global cursor only when the next entry belongs to that author. A later
author’s entry fences undo. Explicit open commit-only groups remain offline
authoring operations. Team operations use the existing one-request atomic
mutation paths.

The owner saves authoritative state using the same persistence path as
offline Studio. Ending the session seals the write path under the broker's
dispatch gate. Save and seal reports success only after persistence. The
same disposition can be retried after a lost reply. Session seats and
presence are ephemeral. Restart creates
a new session and reconstructs project state from the saved project. The
same project remains closed in independent offline editors during a Team
session. The experimental broker lease coordinates Team brokers.
Project v4 and its existing GASP persistence continue to own durable
authored state and resource references. The in-memory journal serves
connected editors. Crash replay and collaborative autosave are separate
recovery features. Local drafts use the working Studio process's existing
close and recovery behavior.

## Extension boundaries

The private loopback profile defers Viewer admission, per-editor revocation,
remote camera rendering, Play, asset publication, schema changes, packaging,
and network deployment. A future direct or self-hosted transport needs
authenticated encrypted channels and a reviewed identity/admission profile.
Managed hosting may supply identity, admission, discovery, and orchestration.
EditorHost retains authored state authority in each deployment.

Engine and Studio authoring roles are separate from gameplay players,
runtime server workloads, and package recipients. Package publication stays
behind Foundation 10A and Runtime Distribution policies. Node's workload
and player identities retain their existing scopes. Gantria can provide a
separate creator admission provider without serving DataModel mutations.

Local cameras, selections, project tabs, draft buffers, and docking remain
client state. The experimental editing profile grants the object tree,
inspector, retained source and committed project views. Collaborative
viewport and Play need independently owned render and runtime lifecycles.
A future Play path should capture an authoritative revision into a separate
test runtime. Runtime changes stay in that runtime.

Keep in-progress gestures local and submit a bounded ordinary field edit
or one atomic batch at the commit boundary. Revalidate captured identity,
schema and batch revision before committing after a remote change. Future
schema publication needs a write barrier, candidate reconstruction and
validation, compatible identity remapping, and a fresh client snapshot.
Asset publication needs host-side admission of content and durable identity.
Use the catalog's logical asset identity and its immutable content digest
for resolution. Record expected versions for reimport and replacement.
FileLink, project-relative paths, and external source mounts stay in the
host's file boundary. A future client proposes admitted bytes and a version,
while the host resolves dependencies, reports missing content, and records
the publication revision.

### Future real-time source editing

Foundation 1 retains exact source revisions, authoritative committed text,
and recoverable local drafts. A real-time layer can use a separate per-source
operation stream keyed by source identity and anchored to a committed source
version. The resulting text enters the existing source commit and transaction
path. Reconnect, deletion, schema changes, and undo still respect those
boundaries.

- Centralized operational transformation can reuse the session sequencer.
  It needs explicit insert/delete ordering and transformation tests for
  overlapping edits, reconnect, and undo. [Nichols et al., Jupiter](https://lively-kernel.org/repository/webwerkstatt/projects/Collaboration/paper/Jupiter.pdf)
- A sequence CRDT supports convergence across disconnected replicas. It
  needs durable element identities, causality metadata, a bounded retention
  strategy and document-level permissions. [Shapiro et al., Inria report](https://asc.di.fct.unl.pt/~nmp/pubs/tr-inria-2011.pdf)

Prototype and measure either design within the source contract when the
required offline behavior is defined. Object authoring continues to use
ordered field commits and validated transactions.

## Verification

Use focused Engine tests and an independent-process real Engine journey.
Compare adapter-only fixtures, native Studio journeys, and physical
network qualification within their original scopes. The initial acceptance
classification stays experimental and private.
