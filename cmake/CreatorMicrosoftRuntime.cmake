# Match the redistributable runtime libraries and installers by file name.
set(GargantuanMicrosoftRuntimePattern "(^|/)(((msvcp|msvcr|vcruntime|concrt|vccorlib|vcomp|vcamp|mfc)[0-9][a-z0-9_]*|ucrtbase|api-ms-win-crt-[a-z0-9-]+)[.]dll|vc[_-]?redist[._-][a-z0-9._-]+[.]exe)$")
