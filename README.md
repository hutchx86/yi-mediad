# yi-mediad (Yi camera media daemon)

A drop-in replacement for the stock **`rmm`** media daemon on Yi cameras running
on the Allwinner platform — specifically the **sun8iw19p1 / V831-class** SoC
(y623, h52ga, r35gb, …). It is the media producer for the sister project,
[**yi-protect**](https://github.com/hutchx86/yi-protect) (the UniFi Protect
bridge).

`mediad` owns sensor capture → ISP → hardware H.264 encode → the stock
`/dev/shm/fshare_frame_buf` ring, plus the mic AAC track. It is *not* a new
protocol: it publishes the same shared-memory ring the stock `rmm` does, so the
downstream `yi_protect_flv_bridge` / UniFi Protect path is untouched and the two are
interchangeable.

This repository is the daemon plus the SD-card overlay that installs it onto a
yi-protect card. The controller/bridge side lives in the sister project.

> **⚠ Proof of concept, not a product.** It runs on my own hardware. It is a
> moving target and expect rough edges.

> **Not affiliated with Yi / Kami.** "Yi" and "Kami" are trademarks of their
> respective owners, used here descriptively (this is camera-side software). No
> Yi/Kami (or Allwinner) firmware or binaries are distributed here. See
> [Legal](#legal).

> **AI-assisted development.** Large parts of this project were produced with
> LLMs (Claude Code and DeepSeek), always under strict human supervision,
> review, and real-hardware testing. Verify anything you rely on.

## Status

Working and validated on real hardware: sensor capture, ISP, hardware H.264
encode, the fshare ring, the on-camera vendor-tuning import, and the picture
control surface all function. Several sub-features carry their own status —
see [Features](#features).

## Features

- **Drop-in producer** — publishes H.264 NALs (Annex-B, SPS/PPS re-emitted per
  IDR) onto the stock fshare ring at the stock offset/header size, so stock
  `imggrabber` and the FLV bridge read it unchanged.
- **Two video channels** — HIGH (`vi_dev 0`: 2304×1296 on `gc3003`, 1920×1080
  on `gc2053`) and LOW (640×360, `vi_dev 1`), both 20 fps, the stock `rmm`
  two-vipp topology; `MEDIAD_LOW=0` for HIGH-only.
- **Hardware H.264 encode** — High profile, VBR, QP [10,40] (stock `rmm`'s own
  encoder values), a keyframe every 5 s on HIGH and 1 s on LOW. The engine is
  the clean-room **freecodec** implementation (statically linked) and its
  supporting framework the clean-room `libvenc_base.so` shipped in `yi-protect/lib/`;
  no vendor encoder blob is linked or shipped (see [Future](#future--in-progress)).
- **Bitrate** — HIGH 2.8 Mbps / LOW 0.7 Mbps by default; HIGH is adjustable from
  Protect (clamped to 4 Mbps) and persists across restarts. `MEDIAD_HIGH_BPS` /
  `MEDIAD_LOW_BPS` override at boot.
- **Mic audio** — direct ALSA capture → AAC-LC, 16 kHz mono, ADTS, published into
  the same ring (type `0x0100`), as stock `rmm` does. The AAC encoder is our own
  **freecodec** build over upstream FAAC.
- **ISP pipeline** — the clean-room `freewinner` libisp framework, 3A and
  register/config tiers, on the V831 (`sun8iw19p1`) ISP-521 ABI.
- **On-camera vendor tuning** — reads the camera's **own** OEM ISP config from
  `/home/app/rmm` at first init (test/3a/tunning/dynamic sections), remaps it to
  our struct, caches it on SD. **No blob or per-firmware address is compiled in.**
- **WDR + PLTM** — the camera's own WDR tuning drives the clean-room ISP tier;
  local tone mapping (PLTM) runs, with its presets read from the camera's `rmm`
  at first boot (not compiled in).
- **Night vision** — IR-cut filter + IR LED from `/dev/cpld_periph`, plus a
  day/night **ISP tuning swap**. Modes: day, night, **auto at a lux threshold**
  (hysteresis + debounce), or IR-cut only; `ir_led` 0–100.
- **Shutter exposure** — Protect Auto / Frame Capture / Best Low Light.
- **Control surface** — an `AF_UNIX` socket (used by yi-protect's client and
  settings page, and by the `mediad_ctl` CLI) and a `mediad.conf` (see
  [Control surface](#control-surface)).
- **Robustness** — self-watchdog (exit after 15 s with no frames so a wedged
  pipeline can't wedge the board); `oom_score_adj=-1000`; `nbufs=3` to fit the
  60 MB box.
- **Burned-in OSD** — date/time, camera name, a bitrate/stats line and a
  placeholder logo, rendered camera-side and pushed to the encoder's overlay
  engine (Protect's `ChangeOsdSettings`). The stock `rmm`'s bitmap watermark is
  not reproduced.

## Future / in progress

The goal is a build with no proprietary blob, and it is reached: the deploy
build (a bare `make`, which is also what `package.sh` runs) compiles no vendor
source, includes no vendor header and links no vendor object. Status by tier:

- **ISP 3A + register/config** — *done*. The AGPL-3.0-only clean-room
  **`freewinner`** tier (AE/AWB/AFS/ISO/GTM/PLTM plus the register/base layer)
  replaces every vendor algorithm-archive member; `libisp_algo_rtos.a`
  contributes no objects.
- **AAC** — *done*. Our **freecodec** encoder over upstream FAAC (LGPL);
  `libaacenc.a` is not linked.
- **H.264 encoder** — *done*. The clean-room **freecodec** engine (statically
  linked) and `libvenc_base.so` (built from `codec/src/base`) replace
  `libvenc_codec.so`/`libVE.so`. Residual gap vs the vendor engine: rate control
  / ME (roughly 2–4× the bits at the same QP).
- **MPP middleware** — *done*. The clean-room packages `fenc_` (the libcedarc
  encoder API), `fisp_` (the ISP runtime) and `fcap_` (the capture runtime)
  replace all twelve vendor files in `media_daemon/Makefile`'s `SRC_MPI` +
  `SRC_VENCODER`; `FREECODEC_HEADERS=1` resolves the daemon's own headers from
  freewinner's `mw_headers` (our declarations) instead of the SDK's.

A built `mediad` therefore links only our code plus the camera's own C/C++
runtime and ALSA. See [Legal](#legal).

## Requirements

- A supported Yi camera (see below) with an SD card already running **yi-protect**
  (this package overlays it).
- A build host with `git`, `curl`, `make`, `python3`, `patch`, `tar`, and the
  musl cross-toolchain (fetched for you, below).

## Supported hardware

Allwinner **sun8iw19**, ~60 MB RAM, BusyBox userland. Supported and tested:

| Model | Camera | Sensor | Notes |
| --- | --- | --- | --- |
| `y623` | Yi **Pro 2k** (PCB may read `y621`) | `gc3003_mipi` | 2304x1296 |
| `h52ga` | Yi **Dome Camera U** (Full HD) | `gc2053_mipi` | 1920x1080 |
| `h51ga` | Yi (2K stock) | `gc2053_mipi` | 1920x1080 (native; stock rmm upscales it to 2K) |
| `r35gb` | Yi **Dome Guard** | `gc2053_mipi` | 1920x1080; mounted rotated 180° (per-model orientation) |

Other sun8iw19 models may work: an unknown sensor gets a best-effort 16:9
geometry.

The **model** is read from the platform's `model_suffix` file (written by
yi-protect) and the **sensor** from the live ISP
(`/sys/class/video4linux/v4l-subdev*/name`, e.g. `gc3003_mipi`, `gc2053_mipi`).
Geometry is keyed on the **model** (a built-in `g_models` capture/encode row),
because several models share one sensor yet stream different sizes: `r35gb` and
`h52ga` are both `gc2053_mipi`, and h51ga's stock rmm output is 2K while h52ga's
is 1080p. The sensor row is only the fallback for an unlisted model; mounting
orientation is likewise per model.

## How it works

The camera keeps its stock vendor firmware and kernel. `mediad` replaces only the
stock media daemon; it drives the hardware encoder through our clean-room H.264
engine and publishes the same ring. Its ISP, capture, encoder and interface code
are all clean-room (`freewinner`/`freecodec`); the SDK is fetched at build time
but contributes no object to the binary.

| File | Role |
| --- | --- |
| `main.c` | VI/ISP bring-up, channel config, bitrate, frame throttle. |
| `fshare.{c,h}` | The stock ring producer. |
| `mediad_venc.{c,h}` | Our H.264 encoder channel over the clean-room freecodec engine. |
| `mediad_audio.{c,h}` | Mic capture (ALSA) → AAC-LC (freecodec) → ring. |
| `audio_codec.c` | Enables the SoC codec hub/mic mixer controls before capture. |
| `talkback.c` | Desktop talkback: FIFO → ALSA speaker playback. |
| `osd.c`, `osd_font.h` | Burned-in OSD (date/name/logo/bitrate); font: Terminus Bold (OFL, `fonts/OFL-Terminus.txt`). |
| `isp_config.c`, `stub_audio_components.c` | ISP tuning import + control appliers. |
| `isp_control.{c,h}` | Control socket, `mediad.conf`, night vision, optional slider page. |
| `rmm_tuning.{c,h}` | On-camera tuning extraction from `/home/app/rmm` (+ SD cache). |
| `stock_reg.c` | Optional stock register-table replay wrapper. |
| `tools/`, `rmm_extract.c` | Extractor CLI, `mediad_ctl`, layout/build tools. |
| `../vin_crop_shim/` | Optional `LD_PRELOAD` capture crop; not used by the default per-model geometry (the gc2053 models capture native 1920x1080). |

## Build

```
./build.sh          # once: fetch toolchain, SDK, FAAC, ... into ./repos
make -C media_daemon           # deploy build -> media_daemon/mediad
./package.sh        # deploy build + SD-card package -> dist/
```

A bare `make` is the deploy build; `package.sh` runs the same flags (with its
own `BUILD`/`TARGET` names and a reproducible build stamp) and then assembles
`dist/`. Build knobs, including the vendor comparison builds: see
[docs/build-knobs.md](docs/build-knobs.md).

The clean-room tiers come from
[**freewinner**](https://github.com/hutchx86/freewinner) (`FW_ROOT`): the
`./freewinner` git submodule when it is checked out (`build.sh` initialises it
when the checkout has one), otherwise a sibling checkout at
`../../freewinner/freewinner-git` relative to this repo. Override with
`make FW_ROOT=/path/to/freewinner`.

`build.sh` fetches the musl cross toolchain, the V831 SDK (sparse), FAAC, the
Melis RTOS ISP archive and a link-time `libasound.so` into the shared `repos/` and
`media_daemon/prebuilt/` (both gitignored), and applies an SDK patch used
only by the vendor comparison builds. The deploy build links none of the SDK:
the RTOS archive is reduced to an empty one. The Allwinner sources and binaries
are fetched for interoperability and are **not** redistributed here. See
[Legal](#legal).

No tuning is compiled in. On first boot mediad reads the camera's own ISP tuning
and 3A tables (including the PLTM presets) from `/home/app/rmm` and caches them
under `/tmp/sd/yi-protect/isp_cfg/` (`freeisp_tables.bin` and the day/night blobs);
later boots read the cache. `MEDIAD_NO_RMM_TUNING=1` disables this.

## Package (`dist/`)

`package.sh` produces:

```
dist/
  install-mediad.sh          installer
  README.md                  package/README.md
  licenses/                  LICENSE, LICENSE-EXCEPTION, NOTICE, OFL-Terminus.txt,
                             COPYING-FAAC, SOURCE.txt (exact source revisions)
  yi-protect/bin/mediad           stripped deploy build
  yi-protect/etc/mediad.conf      default settings (installed only if absent)
  yi-protect/script/mediad.sh     start/stop/restart/status/candidate
  yi-protect/lib/libvenc_base.so  clean-room encoder support (found via $ORIGIN/../lib)
  yi-protect/lib/vin_crop_shim.so optional capture crop (LD_PRELOAD; not used by default)
```

Capture/encode geometry is keyed on the model in the daemon (`g_models`), so no
per-model `.env` ships; a `yi-protect/etc/mediad.<model>.env` still overrides if you
drop one in.

## Install

Which flow you follow depends on the camera's starting point; both are covered
in full in [package/README.md](package/README.md):

- **No yi-protect yet** — install yi-protect first (see its README), then either
  overlay this package onto the card or build a `mediad`-bundled yi-protect image.
- **Already running yi-protect** on the stock `rmm` — overlay this package onto
  the card.

Either way: build with `./build.sh && ./package.sh`, copy `dist/` to the card as
its own folder (do not overwrite the card's `yi-protect/`), run
`sh /tmp/sd/mediad-dist/install-mediad.sh` on the camera, and reboot. The
installer copies `yi-protect/{bin,etc,script,lib}`, sets `IS_MEDIAD=yes`, and
reroutes `init.sh`/`watchdog.sh` to `mediad.sh start`, keeping `.pre-mediad`
backups. If yi-protect is already mediad-aware it only copies files.
`mediad.sh candidate <file>` installs a new binary and starts it, with no
automatic rollback.

## Control surface

`mediad` listens on an `AF_UNIX` stream socket (`/tmp/mediad_ctl.sock`,
`MEDIAD_CTL_SOCK`), one request line and one reply line each: `set <key> <n>`,
`get <key>`, `list`, `reset`, `ping`, `pin <key> <n>`, `unpin <key>`,
`pinned <key>`. yi-protect's client forwards Protect's picture settings there
(with `IS_MEDIAD=yes`), and `mediad_ctl` (built next to `mediad`, not in the
package) is a CLI for it. Keys include `brightness`, `contrast`, `saturation`,
`hue`, `sharpness`, `denoise`, `tdf`, `nr2d`, `cnr`, `venc3d`, `exposure`,
`aebias`, `gamma`, `pltm`, `wdr`/`hdr`, `flicker`/`frequency`, `mirror`, `flip`,
`bitrate`, `nightvision`, `night_lux`, `ir_cut`, `ir_led`, `shutter` and the
`osd*` switches (`mediad_ctl list` prints them). Picture levels are 0–100
with 50 = stock.

- **Web UI:** yi-protect's settings page (`http://<camera>/`, port `WEBUI_PORT`
  in `yi-protect.cfg`, default 80, 0 = off) edits these controls and saves them as
  pinned values. mediad also has a minimal built-in slider page, off by default
  (`webui=1`, `webui_port=8099` in `mediad.conf`).
- **`mediad.conf`** (`/tmp/sd/yi-protect/etc/mediad.conf`, `MEDIAD_CONF`): `key=value`
  lines are applied at boot and **pinned**, so Protect's connect-time re-assert
  cannot override them; `pin_<key>=value` sets a boot value without pinning.
- **Environment:** encoder, geometry and bring-up knobs are environment
  variables, set in `yi-protect/etc/mediad.env`: see [docs/env.md](docs/env.md).

## Legal

- **Not affiliated with, or endorsed by, Yi Technology / Kami.** "Yi" and "Kami"
  are trademarks of their respective owners. (This is camera-side software; the
  UniFi Protect side is [yi-protect](https://github.com/hutchx86/yi-protect).)
- **No Yi/Kami firmware or binaries are distributed here**, and no Allwinner
  binaries either. The `package.sh` deploy build compiles **no** vendor
  Allwinner source and includes no vendor header: the ISP 3A/register tier, AAC,
  H.264, the VENC/ISP/capture middleware and the daemon's own interface headers
  are all clean-room (AGPL-3.0-only `freewinner`/`freecodec`). The SDK is still
  fetched at build time for the vendor comparison builds, but contributes no
  object to the shipped binary. This repo does **not** distribute any Allwinner
  source or binary. See [`NOTICE`](NOTICE).
- Intended for interoperability and personal use on hardware you own. Reverse
  engineering may be restricted in your jurisdiction; you are responsible for how
  you use it.

## Disclaimer

**This is a proof-of-concept project, not a production-ready system.** Review
and verify everything yourself. **This software is provided "as is", without
warranty of any kind.** It runs custom binaries on your camera and
modifies what it launches: you can **brick the camera**, lose recordings, and
void its warranty. By using it you accept full responsibility for any damage,
data loss, downtime, or other consequences. **The authors and contributors are
not responsible or liable for any loss or damage arising from its use.**

## License

AGPL-3.0-only, with an additional permission (GNU AGPL section 7) for linking
the optional Allwinner vendor encoder libraries: see [LICENSE](LICENSE),
[LICENSE-EXCEPTION](LICENSE-EXCEPTION) and [NOTICE](NOTICE). Third-party
components fetched at build time (the Lindenis/Allwinner SDK, FAAC, alsa-lib)
carry their own terms; the clean-room `freewinner`/`freecodec` tiers are
AGPL-3.0-only as well.

## Credits

- **[lindenis-org](https://github.com/lindenis-org)** — the Lindenis V831
  Allwinner SDK (`eyesee-mpp`) and the musl cross-toolchain this builds against.
- **[roleoroleo](https://github.com/roleoroleo)** — the `fshare` shared-memory
  framing and the original video-push path were reverse engineered with reference
  to his **rRTSPServer** (GPLv3); the on-device boot pattern follows
  **yi-hack-Allwinner-v2**.
- [**freewinner**](https://github.com/hutchx86/freewinner) / **freecodec**
  (AGPL-3.0-only) — the clean-room ISP 3A/register tier, the H.264 encoder and
  `libvenc_base`, linked into the deploy build.
- [**yi-protect**](https://github.com/hutchx86/yi-protect) — the UniFi Protect
  bridge this daemon feeds.

Full attribution is in [`NOTICE`](NOTICE).
