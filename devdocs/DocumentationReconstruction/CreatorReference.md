# Creator reference coverage

```json
{
  "SourceRevision": "830f5f26ea5642c9a6d7e281db8df0fb3982e5df",
  "Availability": "Qualified unreleased candidate",
  "Classes": 64,
  "Services": 18,
  "Properties": 311,
  "Methods": 190,
  "Signals": 63,
  "Descriptions": 564,
  "NativeBindings": 20,
  "Enums": 37,
  "EnumItems": 343,
  "GeneratedPages": 140,
  "StructuralDeclarationCoverage": "All declared members",
  "BehavioralCoverage": "Descriptions counted separately; native overloads and missing policy metadata remain backlog",
  "Examples": "See docs/examples/README.md for execution scope",
  "GlobalLibraries": 3,
  "StandaloneExampleFiles": 13,
  "ExampleTypeChecking": "Not performed against generated Engine definitions"
}
```

Regenerate with `python -B tools/docs/GenerateReference.py`. Check drift with `--check`. PublicApi.json remains the declaration catalog. NativeApi.json supplies native registration evidence. Page URLs and qualified member headings are reusable by Studio hover and signature help. Unregistered internal headers are excluded. Native binding labels still need an overload and type audit. Source revision is the qualified candidate, not a released version.
