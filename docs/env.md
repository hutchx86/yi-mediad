# mediad environment variables

`mediad` reads these at startup. On a yi-protect card, set them in
`unifi/etc/mediad.env` (`export KEY=value` lines), which `mediad.sh` sources
before each start; per-model files (`unifi/etc/mediad.<model>.env`) are sourced
after it and win. Most are bring-up and diagnostic knobs: the defaults are the
tested configuration.

Picture controls (brightness, denoise, night vision, ...) are not environment
variables: set them over the control socket, in `mediad.conf`, or from
yi-protect's settings page (see the README).

## Paths and files

| Variable | Default | Effect |
| --- | --- | --- |
| `MEDIAD_CONF` | `/tmp/sd/unifi/etc/mediad.conf` | Control config file (`mediad.sh` sets it when the file exists). |
| `MEDIAD_CTL_SOCK` | `/tmp/mediad_ctl.sock` | Control socket path (also read by `mediad_ctl`). |
| `MEDIAD_RMM_PATH` | `/home/app/rmm` | Stock `rmm` binary the ISP tuning and 3A tables are read from. |
| `MEDIAD_ISP_CACHE` | `/tmp/sd/unifi/isp_cfg` | Directory for the cached day/night tuning blobs. |
| `MEDIAD_TABLE_BUNDLE` | `/tmp/sd/unifi/isp_cfg/freeisp_tables.bin` | Cache of the 3A tables for the clean-room ISP tier; empty string = never cache. |
| `MEDIAD_BITRATE_FILE` | `/tmp/sd/unifi/etc/mediad.bitrate` | Where the bitrate last set by Protect is persisted. |
| `MEDIAD_NO_RMM_TUNING` | unset | Set: do not read tuning or 3A tables from `rmm` (built-in defaults). |

## Sensor, model and geometry

| Variable | Default | Effect |
| --- | --- | --- |
| `MEDIAD_SENSOR` | live sensor name (V4L2 subdev) | Override the detected sensor, e.g. `gc2053_mipi`. |
| `MEDIAD_MODEL` | `unifi/etc/model_suffix` | Override the model (selects mounting orientation). |
| `MEDIAD_MIRROR`, `MEDIAD_FLIP` | per model | Mounting orientation base (0/1). |
| `MEDIAD_CAP_W`, `MEDIAD_CAP_H` | per sensor | HIGH capture size. |
| `MEDIAD_PIC_W`, `MEDIAD_PIC_H` | per sensor | HIGH encoded size. |
| `MEDIAD_CROP_X`, `MEDIAD_CROP_Y` | unset | HIGH: encode a PIC_W x PIC_H window of the capture at this offset (no scaling). |
| `MEDIAD_OUT_W`, `MEDIAD_OUT_H` | encoded size | HIGH displayed size (SPS crop). |
| `MEDIAD_OUT_X`, `MEDIAD_OUT_Y` | centred | Offset of that window (even pixels). |
| `MEDIAD_WDR` | from the camera's tuning, else per sensor | Force sensor WDR capture mode on (1) or off (0). |
| `MEDIAD_NV21` | unset | `1`: capture uncompressed NV21 instead of LBC 2.5X (about twice the frame memory; can exhaust RAM on small models). |
| `VINCROP` | unset | `x,y,w,h` for `vin_crop_shim.so` (LD_PRELOAD): VIPP crop on the capture node whose format is w x h. |

## Encoder

| Variable | Default | Effect |
| --- | --- | --- |
| `MEDIAD_HIGH_BPS`, `MEDIAD_LOW_BPS` | 2800000 / 700000, or the persisted Protect value | Bitrate in bps; wins over the persisted value. |
| `MEDIAD_FPS` | 20 | Encode frame rate, clamped 5..30; below 20, source frames are dropped. |
| `MEDIAD_PROFILE` | 2 | 0 baseline, 1 main, 2 high. |
| `MEDIAD_RC` | 1 | 0 CBR, 1 VBR, 2 AVBR. |
| `MEDIAD_MINQP`, `MEDIAD_MAXQP` | 10 / 40 | QP floor and cap. |
| `MEDIAD_GOP` | 5 s HIGH, 1 s LOW | Keyframe interval in frames, both channels. |
| `MEDIAD_GOP_HIGH`, `MEDIAD_GOP_LOW` | as above | Per channel; override `MEDIAD_GOP`. |
| `MEDIAD_3DNR` | 0 | Encoder 3D filter level 0..3 (smears motion). |
| `MEDIAD_FASTENC` | 0 | Encoder fast-encode flag. |
| `MEDIAD_LOW` | unset | `0`: HIGH channel only. |

## Audio

| Variable | Default | Effect |
| --- | --- | --- |
| `MEDIAD_NO_AUDIO` | unset | Set: no mic capture and no talkback. |
| `MEDIAD_MIC_GAIN_DB` | 12 | Software mic gain in dB (0 disables). |
| `MEDIAD_AO_CARD` | 0 | Talkback output: 0 internal codec (`default`), 1 daudio (`hw:1,0`). |

## OSD

| Variable | Default | Effect |
| --- | --- | --- |
| `MEDIAD_OSD_ENABLE`, `MEDIAD_OSD_DATE` | 1 | Initial OSD / date line state. |
| `MEDIAD_OSD_NAME` | client device name, else model | Camera name shown. |
| `MEDIAD_OSD_LUMA_HI`, `MEDIAD_OSD_LUMA_LO` | built in | Background thresholds (AE statistics units) for switching the letter colour. |
| `MEDIAD_OSD_HWINV` | unset | `1`: use the encoder's per-frame inversion instead. |
| `MEDIAD_OSD_TRACE` | unset | `1`: log colour decisions. |

## Process

| Variable | Default | Effect |
| --- | --- | --- |
| `MEDIAD_NO_MLOCK` | unset | Set: do not lock mediad's memory in RAM. |

## ISP bring-up and diagnostics

Applied once at startup; not needed in normal use.

| Variable | Effect |
| --- | --- |
| `ISP_AE_BIAS`, `ISP_AE_METERING`, `ISP_AE_MODE` | AE exposure bias, metering and mode. |
| `ISP_BRIGHTNESS`, `ISP_CONTRAST`, `ISP_SATURATION`, `ISP_SHARPNESS` | Raw ISP picture values. |
| `ISP_3DNR`, `ISP_NR`, `ISP_PLTMWDR` | Raw 3DNR, denoise and PLTM-WDR values. |
| `ISP_TUNE_OFF` | `1`: ignore the camera's tuning, use built-in defaults. |
| `ISP_TUNE_PARTS` | Tuning sections to import: `test`, `3a`, `tunning`, `dyn`, `all` (comma list). |
| `ISP_KEEP_3A` | Set: do not import the tuning's 3A section. |
| `ISP_TUNE_ENABLE`, `ISP_TUNE_DISABLE` | Comma lists of ISP modules to force on / off (`cm` = colour matrix). |
| `ISP_GAMMA_INVERT`, `ISP_GAMMA_TYPE`, `ISP_GAMMA_NUM`, `ISP_GTM_TYPE` | Gamma / tone-mapping table overrides. |
| `ISP_AE_HIST`, `ISP_AE_HISTSEL`, `ISP_AE_DEFTBL`, `ISP_AE_KI`, `ISP_AE_STATSEL`, `ISP_AE_MAXLV`, `ISP_AE_ISO2GAIN`, `ISP_AE_GAIN_RANGE` | Per-field AE parameter overrides (`ISP_AE_GAIN_RANGE`: up to four comma-separated values). |
| `MEDIAD_ISP_APPLY` | Set: re-run the full ISP config path on control changes (can wedge the pipeline). |
| `MEDIAD_NO_REPLAY` | `STOCK_REG=1` builds: do not replay the captured register table. |

With `STOCK_REG=1`, setting `ISP_SATURATION`, `ISP_SHARPNESS`, `ISP_NR`,
`ISP_3DNR` or `ISP_PLTMWDR` also copies that control's registers onto the
replayed table.

## `rmm_extract` (host/device tuning extractor)

`rmm_extract <rmm_path> <sensor> [outdir]`; the arguments default to
`RMM_PATH`, `RMM_SENSOR` and `RMM_OUTDIR` (`/tmp/sd/unifi/isp_cfg`).
