---
title: Random native binding
description: Public creator reference for Random native binding.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Registered entry points and binding labels follow. Constructor, instance member and metamethod labels retain their original spelling. Consult the datatype guide for supported usage.

```json
{
  "Name": "Random",
  "Kind": "NativeBinding",
  "EntryPoint": "OpenLibRandom",
  "Source": "src/scripting/LibRandom.cpp",
  "BindingLabels": [
    "Clone",
    "NextInteger",
    "NextNumber",
    "NextUnitVector",
    "Shuffle",
    "new"
  ],
  "SignatureCoverage": "Signatures and overloads require behavioral audit; labels alone do not establish call semantics"
}
```

[Datatype behavior](../../api/datatypes-and-globals.mdx)
