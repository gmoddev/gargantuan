# Documentation integrity

Run from the repository root with Python 3.12 or newer:

```sh
python tools/docs/CheckDocumentation.py
python tools/docs/CheckDocumentation.py --luau-compile luau-compile
python -m unittest discover -s tools/docs -p "test_*.py"
```

`luau-compile` is the real compiler. `--null` compiles the standalone examples
and Luau/Lua fenced blocks in current-authoritative guidance without executing
game code, opening windows or requiring Engine/Node infrastructure. CI obtains
it from the pinned Engine Luau submodule, building only the small compiler CLI.
Syntax success is not gameplay execution or full Engine typechecking; native
bindings and example setup receive independent source review.

To refresh derived artifacts after a deliberate documentation/API change:

```sh
python tools/docs/CheckDocumentation.py --update-api --inventory --report
```

The generator writes only `docs/src/data/PublicApi.json` and
`devdocs/DocumentationReconstruction/{Inventory,LinkFindings}.json`. Coordinate
these paths when using a shared worktree. The declaration scanner does not run
project Luau; unsupported declaration syntax and duplicate keys fail rather
than silently disappear. It excludes commented-out members and records each
declaration's UTF-8/LF-normalized content hash so Windows CRLF checkouts and
Linux CI agree. These are not binary qualification hashes. Native headers are indexed separately; constructor
globals and custom native datatype bindings are outside class extraction.

The inventory covers tracked Markdown/MDX/reStructuredText/AsciiDoc, README,
license and authors files throughout the repository, including samples, tools,
tests and vendored notices. Git submodule internals retain their own ownership.
`LastPathCommit` refers to the fixed integrated Engine baseline and is not a
claim that its author reverified architecture. `LastMeaningfulUpdate` stays
unknown where there is no evidence of such review. Classification rules are in
the checker; the seven possible statuses do not imply every class is populated.

The fast check fails missing current links/anchors, navigation targets, literal
source paths, inventory coverage/classification and declaration-catalog drift.
It rejects specific obsolete
current assertions, while historical/accepted/future records may mention old
systems. Non-current link debt is reported by `--report`, with original paths
and context retained. CI rejects drift from the reviewed historical-link report;
intentional report updates require an explicit `--report` refresh. Historical
debt is not silently reclassified as current validation.
External HTTP availability is excluded from deterministic offline CI.

The Astro link transformer maps checkout-relative content links to canonical
site routes and non-site repository targets to GitHub. The built-site verifier
checks that every content page is emitted, then checks generated local routes,
anchor targets and repository-path URLs.

## Build the site

Use Node **24** and Bun **1.3.13** with the existing lockfile:

```sh
cd docs
bun install --frozen-lockfile
bun run build
cd ..
node tools/docs/CheckSite.mjs
```

The Documentation workflow runs these checks for pull requests and main pushes.
It does not reserve shared native workers. Pages publication remains a separate
explicit `main` workflow dispatch with `deploy=true`.
