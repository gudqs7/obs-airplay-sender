# Dependency patches

`doubletake-obs-airplay.patch` applies to:

```text
https://github.com/omarroth/doubletake
5e2f1cbe3e1070bcefec8e549c7baf884727bc00
```

The patch keeps the verified Douyin-specific FairPlay, discovery, timing,
external audio and AAC-ELD integration changes while avoiding a vendored copy
of the LGPL source tree.

Use `scripts/fetch-dependencies.ps1`; do not apply the patch to a different
upstream revision without reviewing the affected files and rerunning the Go
compile checks.

`libxaac-eld-core.patch` applies to:

```text
https://github.com/ittiam-systems/libxaac
2fbadd57bca46693b7a077bcf1513131106d960c
```

It keeps ELD-SBR for MPS/ELDv2 but lets the no-MPS AAC-ELD path use the core
encoder. This is required by the Douyin/AirPlay 480-sample PCM to raw access
unit contract. `scripts/fetch-dependencies.ps1` fetches the pinned source and
applies this patch automatically.
