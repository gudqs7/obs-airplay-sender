# Third-party notices

## doubletake

- Project: https://github.com/omarroth/doubletake
- Pinned commit: `5e2f1cbe3e1070bcefec8e549c7baf884727bc00`
- Copyright: doubletake contributors
- License: GNU Lesser General Public License v3.0 or later
- Source: fetched by `scripts/fetch-dependencies.ps1`, not vendored in this repository
- Local changes: `patches/doubletake-obs-airplay.patch`
- Used component: AirPlay mirroring sender protocol, FairPlay SAP, RTSP/RAOP transport, timing/RTP and mDNS discovery

The bridge executable combines the fetched doubletake source with the project-owned
adapter in `bridge/cmd/obsairplay-bridge`. The LGPLv3 and incorporated GPLv3 texts
are in `licenses/doubletake-LICENSE.txt` and `licenses/GPL-3.0.txt`.

## Ittiam libxaac

- Project: https://github.com/ittiam-systems/libxaac
- Pinned commit: `2fbadd57bca46693b7a077bcf1513131106d960c`
- Source: fetched by `scripts/fetch-dependencies.ps1`, not vendored in this repository
- License: Apache License 2.0
- Local changes: `patches/libxaac-eld-core.patch`
- Used component: AAC-ELD encoder in `obs-airplay-aac-eld.exe`

The local patch changes the no-MPS AAC-ELD path from 2:1 ELD-SBR to the core
encoder so one 1920-byte PCM frame produces one 480-sample raw access unit. The
encoder has passed Windows x64 build, independent decode, helper protocol and
Douyin receiver tests.

The Apache-2.0 license and NOTICE text are in `licenses/libxaac-LICENSE.txt`
and `licenses/libxaac-NOTICE.txt`. Review MPEG AAC patent licensing separately
before redistribution or commercial use.

## OBS Studio

- Project: https://github.com/obsproject/obs-studio
- License: GNU General Public License v2.0 or later
- Used component: OBS plugin API, encoder API and raw audio callback API
