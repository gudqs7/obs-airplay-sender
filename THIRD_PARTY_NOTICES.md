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

## Fraunhofer FDK AAC

- Project: https://github.com/mstorsjo/fdk-aac
- Pinned commit: `c41c3c7516ea8d0d78a53b96b30bed3a29745cc6`
- Source: fetched by `scripts/fetch-dependencies.ps1`, not vendored in this repository
- License: Fraunhofer FDK AAC Codec Library software license in
  `licenses/fdk-aac-NOTICE.txt` and `licenses/fdk-aac-xhe-aac-NOTICE.txt`
- Used component: AAC-ELD encoder in `obs-airplay-aac-eld.exe`

The pinned upstream source includes `METADATA`, which marks the package as
having special licensing conditions. Review the complete FDK AAC license and
upstream metadata before redistribution or commercial use.

The FDK AAC license does not grant patent rights. Distribution and commercial
use may require separate patent licensing. The complete license text must
accompany copies of the encoder and its source. Binary releases must also make
the corresponding FDK AAC source available.

## Evaluated AAC-ELD alternative

- Project: https://github.com/ittiam-systems/libxaac
- License: Apache License 2.0
- Status: not used by the current build

libxaac advertises AAC-ELD encoding support and is the leading candidate for a
future replacement. It has not yet been validated against the Douyin receiver
at the current AAC-ELD frame size and bitrate, so this project keeps the
already-tested FDK AAC path.

## OBS Studio

- Project: https://github.com/obsproject/obs-studio
- License: GNU General Public License v2.0 or later
- Used component: OBS plugin API, encoder API and raw audio callback API
