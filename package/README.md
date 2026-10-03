# mediad SD-card package

An overlay for an existing **yi-protect** SD card that installs `mediad` and
makes it run in place of the stock `rmm` encoder. `mediad` publishes the same
`/dev/shm/fshare_frame_buf` ring the stock `rmm` does, so the yi-protect
bridge/client stack is untouched.

Supported models: `y623` (Yi Pro 2k), `h51ga`/`h52ga` (Yi Dome Camera U),
`r35gb` (Yi Dome Guard), `y291ga` (Yi 1080p Home).

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

1. Copy `dist/` to the card as its own folder, e.g.
   `scp -r dist root@<camera>:/tmp/sd/mediad-dist` (or copy it onto the card on
   a PC). Do not copy it over the card's `yi-protect/`.
2. On the camera: `sh /tmp/sd/mediad-dist/install-mediad.sh` (the SD root
   defaults to `/tmp/sd`; pass another as the first argument).
3. Reboot (or `/tmp/sd/yi-protect/script/mediad.sh start` with the stock `rmm`
   stopped).

The installer:

1. copies `mediad`, `mediad.sh` and the shared libraries into
   `yi-protect/{bin,script,lib}`, and `mediad.conf` into `etc` only if the card
   has none;
2. sets `IS_MEDIAD=yes` in `yi-protect/etc/yi-protect.cfg` (the client then forwards
   Protect's picture controls to mediad's control socket);
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
