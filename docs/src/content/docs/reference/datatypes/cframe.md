---
title: CFrame native binding
description: Public creator reference for CFrame native binding.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Registered entry points and binding labels follow. Constructor, instance member and metamethod labels retain their original spelling. Consult the datatype guide for supported usage.

```json
{
  "Name": "CFrame",
  "Kind": "NativeBinding",
  "EntryPoint": "OpenLibCFrame",
  "Source": "src/scripting/LibCFrame.cpp",
  "BindingLabels": [
    "AngleBetween",
    "Angles",
    "FuzzyEq",
    "GetComponents",
    "Inverse",
    "Lerp",
    "LookVector",
    "Orthonormalize",
    "PointToObjectSpace",
    "PointToWorldSpace",
    "Position",
    "RightVector",
    "Rotation",
    "ToAxisAngle",
    "ToEulerAngles",
    "ToEulerAnglesXYZ",
    "ToEulerAnglesYXZ",
    "ToObjectSpace",
    "ToOrientation",
    "ToWorldSpace",
    "UpVector",
    "VectorToObjectSpace",
    "VectorToWorldSpace",
    "X",
    "XVector",
    "Y",
    "YVector",
    "Z",
    "ZVector",
    "__add",
    "__mul",
    "__sub",
    "__tostring",
    "new"
  ],
  "SignatureCoverage": "Signatures and overloads require behavioral audit; labels alone do not establish call semantics"
}
```

[Datatype behavior](../../api/datatypes-and-globals.mdx)
