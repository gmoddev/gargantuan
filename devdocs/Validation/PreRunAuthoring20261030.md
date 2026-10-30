# PreRun authoring native checkpoint — 2026-10-30

Base: `8338623b3e1a55ef3924e7a82b39c87cd7f49f5a`.
Branch: `feature/prerun-authoring`.

The native Windows worker (MSVC 19.44, Release, GNS disabled) passes the new
PreRun EditorHost group (42 checks), existing foundation and PreRun bootstrap
groups. The new group also passes five consecutive focused repetitions after
correcting the observed deadline-interrupt stack overflow. Required hosted
MSVC 19.50+, Linux sanitizers and GNS checks remain pending at this checkpoint.

Coverage includes all three Schema interfaces, disposable validation without
source/world/registry publication, syntax errors, duplicate definitions, invalid
inheritance, timeout, memory exhaustion and allocating GC churn, conflict tokens,
external/racing writers, temporary cleanup, exact 256 KiB input, malformed UTF-8
and Base64, unknown paths, live project identity, save/reopen/custom-class
authoring, pending-source Play/package rejection, active Play denial, linked
source escape rejection and Save As location fencing. The Windows symlink test
executed rather than taking its explicit unavailable branch.

The initially intermittent native failure was attributed with CDB and a Release
link map: Interrupt → lua_break → luaG_runerrorL → pusherror → lua_pushstring →
luaC_step recursively re-entered the expired callback until stack overflow.
The native correction follows pinned Luau's GC/safepoint interrupt convention,
disarms before raising an execution-budget error and preserves existing limits.
Both successful and failing historical logs are retained; failures are not
reclassified as passes. The temporary link-map option was removed before the
final Release validation. No debugger or task test process remains.

Raw worker evidence is under `C:\Sandbox\Codex\Logs\prerun-authoring`:
`debug-stack-*.log`, `prerun-test.map`, `deadline-build.log`,
`deadline-tests.log` and `deadline-repeat.log`. Native desktop Studio controls,
typed Studio integration and hosted CI remain separate acceptance gates.

No physical networking, Foundation 3L/KI-006 campaign, 3M, deployment, publication
or automatic merge was performed. Only task-owned temporary projects and
debugger/test processes were cleaned up.
