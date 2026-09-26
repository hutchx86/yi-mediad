# yi-mediad (Yi camera media daemon)

A drop-in replacement for the stock **`rmm`** media daemon on Yi cameras running
on the Allwinner platform — specifically the **sun8iw19p1 / V833-class** SoC
(y623, h52ga, r35gb, …). It is the media producer for the sister project,
[**yi-protect**](https://github.com/hutchx86/yi-protect) (the UniFi Protect
bridge).

`mediad` owns sensor capture → ISP → hardware H.264 encode → the stock
`/dev/shm/fshare_frame_buf` ring, plus the mic AAC track. It is *not* a new
protocol: it publishes the same shared-memory ring the stock `rmm` does, so the
downstream `unifi_flv_bridge` / UniFi Protect path is untouched and the two are
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
> LLMs (DeepSeek), always under strict human supervision, review, and
> real-hardware testing. Verify anything you rely on.

## Status

Working and validated on real hardware: sensor capture, ISP, hardware H.264
encode, the fshare ring, the on-camera vendor-tuning import, and the picture
control surface all function. Several sub-features carry their own status —
see [Features](#features).

## Features

- **Drop-in producer** — publishes H.264 NALs (Annex-B, SPS/PPS re-emitted per
  IDR) onto the stock fshare ring at the stock offset/header size, so stock
  `imggrabber` and the FLV bridge read it unchanged.
- **Two video channels** — HIGH (2304×1296 @20 fps, `vi_dev 0`) and LOW
  (640×360 @20 fps, `vi_dev 1`), the stock `rmm` two-vipp topology; `MEDIAD_LOW=0`
  for HIGH-only.
- **Hardware H.264 encode** — VENC, profile 2 (High), VBR, GOP 40, QP [10,40]
  (stock `rmm`'s own encoder values), FastEnc off. The H.264 engine is the
  clean-room **freecodec** implementation (statically linked) and its supporting
  framework is the clean-room `libvenc_base.so` shipped in `unifi/lib/`; no vendor
  encoder blob is linked or shipped (see [Future](#future--in-progress)).
- **Bitrate** — HIGH 2.8 Mbps / LOW 0.7 Mbps by default; HIGH is adjustable from
  Protect, clamped to 4 Mbps by the control surface. `MEDIAD_HIGH_BPS`/`LOW_BPS`
  override at boot.
- **Mic audio** — direct ALSA capture → AAC-LC, 16 kHz mono, ADTS, published into
  the same ring (type `0x0100`), as stock `rmm` does. The AAC encoder is our own
  **freecodec** build over upstream FAAC.
- **ISP pipeline** — native Lindenis V833 `sun8iw19p1` libisp framework with the
  **clean-room 3A/config tier** (`freewinner`) replacing the vendor algorithm
  archives.
- **On-camera vendor tuning** — reads the camera's **own** OEM ISP config from
  `/home/app/rmm` at first init (test/3a/tunning/dynamic sections), remaps it to
  our struct, caches it on SD. **No blob or per-firmware address is compiled in.**
- **WDR + PLTM** — full vendor WDR tuning via the Melis `isp521-ipc` algorithm
  archive; local tone mapping (PLTM) runs.
- **Night vision** — IR-cut filter + IR LED from `/dev/cpld_periph`, plus a
  day/night **ISP tuning swap**. Modes: day, night, **auto at a lux threshold**
  (hysteresis + debounce), or IR-cut only; `ir_led` 0–100.
- **Shutter exposure** — Protect Auto / Frame Capture / Best Low Light.
- **Control surface** — an `AF_UNIX` socket + `mediad_ctl` CLI, a `mediad.conf`,
  and a built-in web UI for every control (see [Control surface](#control-surface)).
- **Robustness** — self-watchdog (exit after 15 s with no frames so a wedged
  pipeline can't wedge the board); `oom_score_adj=-1000`; `nbufs=3` to fit the
  60 MB box.
- **Burned-in OSD** — date/time, camera name, a bitrate/stats line and a
  placeholder logo, rendered camera-side and pushed to the encoder's overlay
  engine (Protect's `ChangeOsdSettings`). The stock `rmm`'s bitmap watermark is
  not reproduced.

## Future / in progress

The goal is a build with no proprietary blob — **reached 2026-09-23**: the
`ALGO_RTOS=1 FREECODEC_H264=1 FREECODEC_FENC=1 FREECODEC_FISP=1
FREECODEC_FCAP=1 FREECODEC_HEADERS=1` deploy build (what `package.sh` runs)
compiles no vendor source and includes no vendor header. Status by tier:

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
  replace all twelve vendor files in `work/media_daemon/Makefile`'s `SRC_MPI` +
  `SRC_VENCODER`; `FREECODEC_HEADERS=1` resolves the daemon's own headers from
  `freewinner/freewinner-git/mw_headers` (our declarations) instead of the SDK's.

A built `mediad` therefore links only our code plus the camera's own C/C++
runtime and ALSA. See [Legal](#legal).

## Requirements

- A supported Yi camera (see below) with an SD card already running **yi-protect**
  (this package overlays it).
- A build host with `git`, `curl`, `make`, `python3`, `patch`, `tar`, and the
  musl cross-toolchain (fetched for you, below).

## Supported hardware

Allwinner **sun8iw19**, ~60 MB RAM, BusyBox userland. Developed against:

- Yi **Pro 2k** (`y623`; PCB silkscreen may read `y621`)
- Yi **Dome Camera U** (Full HD) (`h52ga`)
- Yi **Dome Guard** (`r35gb`)

The **sensor** is read at boot from the live ISP
(`/sys/class/video4linux/v4l-subdev*/name`, e.g. `gc3003_mipi`, `gc2053_mipi`)
and the **model** from the platform's `model_suffix` file (written by
yi-protect). Geometry is keyed on the sensor and only the mounting orientation
(mirror/flip) on the model — the model is never inferred from the sensor, since
several models can share one sensor (`r35gb` and `h52ga` both use `gc2053_mipi`).

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
| `isp_control.{c,h}` | The control socket + web UI. |
| `rmm_tuning.{c,h}` | On-camera tuning extraction from `/home/app/rmm` (+ SD cache). |
| `stock_reg.c` | Optional stock register-table replay wrapper. |
| `tools/`, `rmm_extract.c` | Extractor CLI and layout/build tools. |

## Install (SD-card overlay)

The deliverable is a small overlay for an existing yi-protect SD card —
`mediad` plus the scripts that make it run in place of the stock `rmm`; the
bridge/client stack is untouched.

```
./package.sh                         # build mediad + assemble dist/
# copy dist/ to the card, then on the device:
/tmp/sd/unifi/install-mediad.sh      # default target /tmp/sd
```

The installer copies `unifi/bin/mediad`, `unifi/etc/mediad.conf`,
`unifi/script/mediad.sh` and the shared library in `unifi/lib/`
(`libvenc_base.so`, which `mediad` finds via its `$ORIGIN/../lib` rpath), sets
`IS_MEDIAD=yes` in `unifi.cfg`, and changes the two lines that handle the stock
encoder (`init.sh` launches `mediad.sh start`; `watchdog.sh` watches `mediad`),
keeping `.pre-mediad` backups. If yi-protect is already mediad-aware, it only
copies files. Reboot afterwards.

## Build from source

`build.sh` fetches every third-party input into `./repos/` and
`work/media_daemon/prebuilt/` (both gitignored), then the Makefile compiles the
vendor MPP source it needs and links our clean-room codecs:

```
./build.sh
make -C work/media_daemon                      # default: from-scratch, linear
make -C work/media_daemon BUILD=build-rtos-v TARGET=mediad_rtos_v ALGO_RTOS=1 FREECODEC_H264=1 FREECODEC_FENC=1 FREECODEC_FISP=1 FREECODEC_FCAP=1 FREECODEC_HEADERS=1   # clean deploy build
```

`build.sh` fetches the musl cross toolchain, the V833 SDK (sparse), FAAC, a
link-time `libasound.so`, and applies the SDK ABI patch. The SDK's closed
Allwinner sources/binaries are fetched for interoperability and are **not**
redistributed here. See [Legal](#legal).

The `ALGO_RTOS=1` deploy build links the AGPL-3.0-only clean-room ISP tier from a
sibling [`freewinner`](../freewinner) checkout (path override: `FREEWINNER=`), and
the freecodec AAC archive from a sibling `freecodec-git` checkout (override
`FREECODEC_DIR=`). The clean tier replaces the vendor archive's 3A algorithm
members *and* the register/config/base tier, so `libisp_algo_rtos.a` contributes
no objects. On first boot the clean modules locate the camera's own tuning tables
in `/home/app/rmm` and cache them to `/tmp/sd/unifi/isp_cfg/freeisp_tables.bin`;
later boots read that bundle only. `MEDIAD_NO_RMM_TUNING=1` disables the feed.

The default (non-RTOS) target still builds `libvenc_base.so` from SDK source and,
like `ALGO_RTOS=0`, uses the vendor ISP algorithm archives; it is a development
target, not the deliverable.

Nothing proprietary is committed: tuning blobs, prebuilt archives, firmware and
the device musl `libasound.so` are all gitignored and produced locally.

## Control surface

`mediad` runs an `AF_UNIX` `SOCK_STREAM` listener (default `/tmp/mediad_ctl.sock`,
override `MEDIAD_CTL_SOCK`); one text line per request (`get`/`set`/`list`/`reset`/
`ping`/…). `mediad_ctl` is a small on-device CLI for it. Keys:
`brightness`, `contrast`, `saturation`, `hue`, `sharpness`, `denoise`,
`exposure`, `aebias`, `gamma`, `tdf`, `pltm`, `wdr`/`hdr`, `flicker`/`frequency`,
`mirror`, `flip`, `bitrate`, `nightvision`, `ir_cut`, `ir_led`, `shutter`.
Defaults are the **stock** values (0–100 levels, 50 = neutral). They can also be
set from `mediad.conf` (`/tmp/sd/unifi/etc/mediad.conf`, override `MEDIAD_CONF`)
or the built-in web UI (`webui=1`, `webui_port=8099`).

Encoder quality is env-overridable: `MEDIAD_HIGH_BPS`/`MEDIAD_LOW_BPS`,
`MEDIAD_PROFILE`/`MEDIAD_RC`/`MEDIAD_MINQP`/`MEDIAD_MAXQP`/`MEDIAD_GOP`/
`MEDIAD_3DNR`/`MEDIAD_FPS`.

## Legal

- **Not affiliated with, or endorsed by, Yi Technology / Kami.** "Yi" and "Kami"
  are trademarks of their respective owners. (This is camera-side software; the
  UniFi Protect side is [yi-protect](https://github.com/hutchx86/yi-protect).)
- **No Yi/Kami firmware or binaries are distributed here**, and no Allwinner
  binaries either. The `package.sh` deploy build compiles **no** vendor
  Allwinner source and includes no vendor header: the ISP 3A/register tier, AAC,
  H.264, the VENC/ISP/capture middleware and the daemon's own interface headers
  are all clean-room (AGPL-3.0-only `freewinner`/`freecodec`). The SDK is still
  fetched at build time as a source of *concepts* and for the `-lvenc_base`
  sibling layout, but contributes no object to the shipped binary. This repo does
  **not** distribute any Allwinner source or binary. See [`NOTICE`](NOTICE).
- Intended for interoperability and personal use on hardware you own. Reverse
  engineering may be restricted in your jurisdiction; you are responsible for how
  you use it.

## Disclaimer

**This is a proof-of-concept project, not a production-ready system.** Large
parts were produced with LLMs under strict human supervision and real-hardware
testing — review and verify everything yourself. **This software is provided "as
is", without warranty of any kind.** It runs custom binaries on your camera and
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

- **[lindenis-org](https://github.com/lindenis-org)** — the Lindenis V833
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
