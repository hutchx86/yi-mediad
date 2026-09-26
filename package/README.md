# mediad SD-card package

An overlay for an existing **yi-protect** SD card that installs `mediad` and
makes it run in place of the stock `rmm` encoder. Nothing else is needed:
`mediad` publishes the same `/dev/shm/fshare_frame_buf` ring the stock `rmm`
does, so the yi-protect bridge/client stack is untouched.

## Build the package

From the repo root:

```
./package.sh          # -> dist/
```

`dist/` is the deliverable:

```
dist/
  install-mediad.sh          # installer (runs on the device / a mounted card)
  README.md
  unifi/
    bin/mediad               # stripped, ARM (the deploy build)
    etc/mediad.conf          # default settings (optional at runtime)
    script/mediad.sh         # start/stop/restart/status
    lib/libvenc_base.so      # encoder support library ($ORIGIN/../lib)
```

## Install onto a yi-protect SD card

Copy `dist/` to the card and run the installer (default target `/tmp/sd`):

```
scp -r dist/* <camera>:/tmp/sd/          # or copy on the bench
/tmp/sd/unifi/install-mediad.sh          # or: install-mediad.sh /path/to/sd
```

The installer:

1. copies `mediad`, `mediad.conf`, `mediad.sh` and the shared library
   (`libvenc_base.so`, our clean-room encoder framework) into
   `unifi/{bin,etc,script,lib}`;
2. sets `IS_MEDIAD=yes` in `unifi/etc/unifi.cfg` (the client then forwards
   Protect's picture controls to mediad's control socket);
3. edits `unifi/script/init.sh` to launch `mediad.sh start` where it launched
   the stock `./rmm` (backs up to `init.sh.pre-mediad`);
4. edits `unifi/script/watchdog.sh` to watch `mediad` instead of `./rmm`
   (backs up to `watchdog.sh.pre-mediad`).

If either script differs from what it expects, it says so and leaves it alone —
make that one-line change by hand (the installer prints the exact line).

Then reboot (or `unifi/script/mediad.sh start`).

## Notes

- `mediad` reads `unifi/etc/mediad.conf` if present; otherwise it uses defaults
  matching stock `rmm`. All controls are also settable at runtime over
  `/tmp/mediad_ctl.sock` (`mediad_ctl`, or Protect via the client).
- The stock `rmm` is left in place; the installer only changes what is launched.
- The `package.sh` deploy build compiles no vendor source and includes no vendor
  header: ISP 3A/register, AAC, H.264, the VENC/ISP/capture middleware and the
  daemon's own interface headers are all clean-room (`freewinner`/`freecodec`,
  AGPL-3.0-only). The build fetches the SDK but no vendor object reaches the
  binary — see the repo's `LICENSE`/`NOTICE`.
