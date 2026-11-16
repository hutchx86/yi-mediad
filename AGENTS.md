# AGENTS.md

Guidance for coding agents working in this repository. Read `README.md` first for what
the project is; this file covers how to change it.

## Build and verify

    ./build.sh && ./package.sh   # mediad plus the dist/ overlay
    make -C media_daemon test-hevc

The codec and ISP host tests live in the freewinner checkout:
`make -C <freewinner>/isp check` and `make -C <freewinner>/codec check`.

Run the real command before claiming a change works, and report the exact command and
result. On-camera behaviour is the final check.

## Layout

- `media_daemon/`: the daemon (VI capture, VENC channel, OSD, audio, ISP control,
  fshare ring).
- `package/`: config, scripts and the install flow that `package.sh` assembles.
- `vin_crop_shim/`: the VI window shim.
- `docs/`: environment knobs and the channel/encoder specs.
- `dist/`: generated; do not edit.

## Code

- Smallest correct change. Match the surrounding style; leave unrelated lines alone.
- Comments explain why, not what. No commented-out code or banner art.
- Handle errors at boundaries and fail loudly. Never swallow an error.
- Add a regression test for every fix.

## Boundaries

- The encoder, VE driver, ISP and 3A code come from the clean-room `freewinner`
  checkout. Do not copy vendor code, decompilation, disassembly or byte dumps here.
- Per-channel and per-model facts are table or environment data, not code branches on
  the model name.
- Never commit firmware images, flash dumps, credentials or device certificates.
- Do not push, force-push or rewrite published history unless the maintainer asks.

## AI assistance

AI-assisted changes are allowed. The author is responsible for reviewing and testing
them. Disclose material AI assistance; do not add generated-by trailers.
