# Foundation 3L qualified scale content

These two files are copied byte for byte from the accepted
`foundation3l-qualified-scale-b9f8ded29/fixture/content` package. The same
content digest appears in the accepted 32-client package descriptor and both
player/server deployment manifests. Foundation 3L acceptance source
`b420a7fd1567632b376760e65fc71386c9bddcf4` used the same 512-object,
273,032-byte Version 4 content workload.

- `scale.instance.json`: 273,032 bytes, SHA-256
  `c4f5af6f55981de25acc92e9b6ac7247c8e35b67b6e047519b5d59d35d5ec5f0`.
- `content.manifest.json`: 494 bytes, SHA-256
  `d2d32c7d7e5ae93ea0693caa05e66c87c0f70e96047fa858619e44b1d3ef41a6`.

The qualified package producer checks both hashes and deserializes the content
under the active runtime schema before building. Current general scale fixtures
continue to serialize the current Version 5 schema. These pinned bytes retain
the accepted D03 workload and digest while new default Part controls and
Version 5 document-local NodeIds evolve elsewhere.
