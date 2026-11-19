# mediad SD-card package

`mediad` is the clean-room encoder daemon that replaces the stock Yi `rmm`
encoder under **yi-protect**. It publishes the same `/dev/shm/fshare_frame_buf`
ring the stock `rmm` does, so the yi-protect bridge/client stack is untouched.
It adds H.264 + H.265, per-model capture geometry, and the advanced picture
controls.

Supported models: `y623` (Yi Pro 2k), `h51ga`/`h52ga` (Yi Dome Camera U),
`r35gb` (Yi Dome Guard), `y291ga` (Yi 1080p Home).

## Stock firmware

`mediad` reads the camera's own ISP tuning from the stock `/home/app/rmm`.
Update the camera in Yi Home to a tested build before installing, and compare
the camera's image with the table (`md5sum /home/app/rmm`). The old h51ga
`9.0.21.12` build fails the extraction and falls back to shim defaults, which
shows as a green image; update it to `9.0.21.16`.

| Model | Stock home | Stock `rmm` md5 | State |
| --- | --- | --- | --- |
| `h51ga` | `9.0.21.16_202611031656` | `4f53a67f8ea451a83f6c721c17e2f79d` | tested |
| `h51ga` | `9.0.21.13_202109101104` | `24669ee2ac1601377c3b66e7736f486d` | tested |
| `h51ga` | `9.0.21.12_202011061017` | `bc57673c7f8c1f2507ab74bd83867529` | update to `9.0.21.16` |
| `h52ga` | stock dump 2026-09-22 | `7e071bafcb9871d60a275fd58002480c` | fleet |
| `r35gb` | stock dump 2026-09-22 | `6c5a7f8c9ace9b521866a73a6f78b9da` | fleet |
| `r35gb` (remote) | stock dump | `e3775c9ea7cd59e8e629e0ad5923ecec` | fleet |
| `y291ga` | `12.0.35.8_202411121417` | `564bb52dc8921dae607ea1e40d34358e` | fleet |
| `y623` | stock dump 2026-11-15 | not recorded | update; record md5 |
| `h30ga` | `11.1.26.7_202606301651` | not recorded | update; record md5 |

## Contents

Built from the repo root with `./build.sh && ./package.sh`:

```
dist/
  install-mediad.sh          installer
  README.md                  this file
  licenses/                  LICENSE, LICENSE-EXCEPTION, NOTICE, OFL-Terminus.txt,
                             COPYING-FAAC, SOURCE.txt (exact source revisions)
  yi-protect/bin/mediad           stripped ARM binary (the deploy build)
  yi-protect/etc/mediad.conf      default settings (installed only if absent)
  yi-protect/script/mediad.sh     start/stop/restart/status/candidate
  yi-protect/lib/libvenc_base.so  clean-room encoder support (found via $ORIGIN/../lib)
  yi-protect/lib/vin_crop_shim.so optional capture crop (LD_PRELOAD; not used by default)
```

## Install

Pick the starting point that matches the camera. Either way `mediad` publishes
the same fshare ring as the stock `rmm`, so the yi-protect stack is unchanged.

### A. Camera with no yi-protect yet

Install yi-protect first — see the yi-protect README, "Install (prebuilt)" or
"Build from source". Then add `mediad` either way:

- **Overlay after install (simplest).** Finish the yi-protect install, let the
  camera boot and be adopted, then follow section B below on the card.
- **Mediad-bundled image.** Build this package first (`./build.sh &&
  ./package.sh`) so its `dist/` exists, then run yi-protect's `build_sd.sh`; it
  packages `dist/` into the SD image. `IS_MEDIAD` defaults to `auto`, so the
  image runs `mediad` on first boot with no config edit.

> The published yi-protect release image is **rmm-only** — `mediad` is not built
> into it. Getting `mediad` on a fresh camera means either the overlay (A/B
> above) or building a bundled image yourself.

### B. Camera already running yi-protect (stock `rmm`)

1. Build if needed: `./build.sh && ./package.sh`.
2. Copy `dist/` to the card as its own folder, e.g.
   `scp -r dist root@<camera>:/tmp/sd/mediad-dist` (or copy it onto the card on
   a PC). Do not copy it over the card's `yi-protect/`.
3. On the camera: `sh /tmp/sd/mediad-dist/install-mediad.sh` (the SD root
   defaults to `/tmp/sd`; pass another as the first argument).
4. Reboot (or `/tmp/sd/yi-protect/script/mediad.sh start` with the stock `rmm`
   stopped).

The installer:

1. copies `mediad`, `mediad.sh` and the shared libraries into
   `yi-protect/{bin,script,lib}`, and `mediad.conf` into `etc` only if the card
   has none;
2. sets `IS_MEDIAD=yes` in `yi-protect/etc/yi-protect.cfg` (the default `auto`
   would also select it now that the files are present);
3. edits `yi-protect/script/init.sh` to launch `mediad.sh start` where it launched
   the stock `./rmm` (backup: `init.sh.pre-mediad`);
4. edits `yi-protect/script/watchdog.sh` to watch `mediad` instead of `./rmm`
   (backup: `watchdog.sh.pre-mediad`).

If either script differs from what it expects, it says so and leaves it alone;
make that one-line change by hand (the installer prints the exact line). If
yi-protect is already mediad-aware, it only copies files.

## Running

- `mediad.sh start` sources `yi-protect/etc/mediad.env` (optional `export KEY=value`
  knobs, see the repo's `docs/env.md`) and then `mediad.<model>.env` if present,
  and logs each start to `/tmp/sd/mediad-pass-<n>.log`.
- **No auto-rollback:** `mediad.sh` starts the installed binary directly and
  leaves it in place whether or not it produces frames (a failing build is fixed
  and redeployed, never silently reverted). `mediad.sh candidate <file>` installs
  a new binary and starts it.
- Picture settings: yi-protect's settings page (`http://<camera>/`, port
  `WEBUI_PORT` in `yi-protect.cfg`), Protect, or `yi-protect/etc/mediad.conf`, whose
  `key=value` lines are pinned against Protect's connect-time re-assert.
- The stock `rmm` stays on the camera; the installer only changes what is
  launched. Restore the `.pre-mediad` backups to go back.

## Licences

`mediad` and the clean-room freewinner/freecodec code it links are
AGPL-3.0-only (with the section 7 permission in `LICENSE-EXCEPTION`); FAAC is
LGPL-2.1-or-later; the OSD font is Terminus (SIL OFL 1.1). No vendor source,
header or object is compiled into the binary. `licenses/SOURCE.txt` names the
exact source revisions.
