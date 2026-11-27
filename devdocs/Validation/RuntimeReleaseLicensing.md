# Player and Server release licensing

Player and Server each carry the complete Engine MPL 2.0, including its warranty disclaimer, and directions for retrieving the matching covered source in `notices/Gargantuan.txt`. The native packager copies the notices as required files. The creator supplies any credits, licenses, source or other materials required by their own game, assets and third-party code. Distributing or running these game packages does not require Studio acceptance.

## Prepare the covered source

Choose an immutable commit that contains the sources and patches used for the shipped role. Review the actual source inputs and nested dependency revisions. From the Engine repository run the following command, substituting the actual full revision and paths.

```text
python tools/distribution/build_runtime_source.py <repository> <full-revision> <source.zip> --build-root <actual-build-source-root>
```

Configure the build with `GARGANTUAN_EXTERNAL_DISTRIBUTION=ON`, `GARGANTUAN_COVERED_SOURCE_ARCHIVE=<absolute-path-to-source.zip>` and `GARGANTUAN_RUNTIME_SOURCE_REVISION=<full-revision>`. Both native stage targets embed the ZIP in the existing required Engine notice. Its instructions recover the exact ZIP offline to an exclusively created temporary file and print its location. Executing them a second time produces a separate file and preserves the game manifest. The archive lists the editable Engine files and upstream dependency revisions. Review custom build inputs and any changed dependencies against their own terms.

An external build can instead use `GARGANTUAN_COVERED_SOURCE_INSTRUCTIONS=<text-file>` for a tested accompanying archive or accessible download. Include the actual revision and patches. Exercise the route as a recipient. The development fallback identifies a clean local revision or marks an unidentifiable build internal. External staging requires an explicit source route.

## Windows runtime

Windows x64 games use the separately acquired Microsoft Visual C++ v14 runtime. Each role records a minimum derived from its MSVC toolset and supplies Microsoft’s official download route in `notices/MSVC-Runtime.txt`. Users or administrators install the prerequisite under Microsoft's terms. Offline deployment requires preprovisioning it on the destination. SDL, Protocol Buffers and any configured product-specific crypto libraries must still load from the game’s reviewed runtime. The host checks the loaded Microsoft libraries against the system directory.

Neither a Microsoft redistributable installer nor individual Microsoft runtime DLLs belong in these game exports. An incremental native stage removes obsolete runtime entries owned by its preceding generated manifest. A conflicting unowned file is preserved and requires a different staging directory. Microsoft documents supported architectures, version compatibility and central installation. [Supported VC++ runtimes](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/) and [Visual C++ distribution guidance](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files).

## Check the distributable

1. Build both roles. Check the actual compiler, binary imports, link inputs, backend selections and nested library options. Compare the notice paths with the enabled features, including HIDAPI, image codecs, the fonts and shaping libraries, crypto and any server-specific providers. Keep the effective licenses, attributable credits and warranty language intact.
2. For each exported game, run the native packager's `validate` and `inspect` commands from a fresh extraction. Run Player and Server from relocated game directories with a minimal PATH. Exercise the packaged multiplayer features before treating a pair as compatible.
3. Generate the file inventory with `python tools/distribution/release_inventory.py capture <game-directory> Player` or `Server`. Record each component’s name, version, origin, license, distribution basis and exact delivered files and notices. Include static code against the executable, game content and wrappers or other separately distributed files. Keep private build receipts outside the deliverable.
4. Fill in the inventory’s `Source.Revision`, `RecipientDirections`, `RetrievalEvidence` and the retrieved ZIP’s `ArchiveSha256` when applicable. Follow the printed directions from the unpacked game. Compare its source with the build evidence and run `python tools/distribution/release_inventory.py verify <game-directory> <role> --review <inventory.json> --retrieved-source <recovered-source.zip-or-recursive-checkout>`.
5. Record the final archive hashes and compare the final extracted files against the reviewed inventory. If game assets require their own credits or source, put those materials in the distribution and state their scope. Keep any extra files outside a native game directory whose manifest enforces a closed file layout.

Public Actions workflows publish explicitly named diagnostic reports. The private release materials and review receipts establish the contents of a selected release. When a workflow adds an upload, `python tools/distribution/test_release_compliance.py` checks the allowed evidence paths and source recovery.
