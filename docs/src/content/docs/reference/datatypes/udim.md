---
title: UDim native binding
description: Public creator reference for UDim native binding.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Registered entry points and binding labels follow. Constructor, instance member and metamethod labels retain their original spelling. Consult the datatype guide for supported usage.

```json
{
  "Name": "UDim",
  "Kind": "NativeBinding",
  "EntryPoint": "OpenLibUDim",
  "Source": "src/scripting/LibUDim.cpp",
  "BindingLabels": [
    "Lerp",
    "Offset",
    "Scale",
    "__add",
    "__eq",
    "__sub",
    "__tostring",
    "__unm",
    "fromOffset",
    "fromScale",
    "new"
  ],
  "SignatureCoverage": "Signatures and overloads require behavioral audit; labels alone do not establish call semantics"
}
```

[Datatype behavior](../../api/datatypes-and-globals.mdx)
