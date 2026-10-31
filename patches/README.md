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
