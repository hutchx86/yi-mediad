# Build knobs (`work/media_daemon/Makefile`)

A bare `make -C work/media_daemon` is the deploy build, the same flags
`package.sh` uses (it only adds its own `BUILD`/`TARGET` names and a
reproducible build stamp). It compiles no vendor source, includes no vendor
header and links no vendor object: the ISP 3A/register/framework tiers, the
capture and ISP runtimes, the vencoder API, the H.264 encoder and
`libvenc_base.so` come from freewinner/freecodec, AAC from freecodec over FAAC.

Every knob can be overridden on the make command line. The non-default values
select **vendor comparison builds**, which need the Allwinner SDK fetched by
`build.sh` (and, for some, the SDK patch it applies). They exist to A/B a
clean-room tier against the vendor one; they are not the deliverable.

## Paths

| Knob | Default | Meaning |
| --- | --- | --- |
| `FW_ROOT` | `../../freewinner` if checked out, else `$(SIBLINGS)/freewinner/freewinner-git` | freewinner checkout (clean-room ISP + codec). |
| `SIBLINGS` | four levels above `work/media_daemon` | Directory holding sibling checkouts. |
| `FREEWINNER` | `$(FW_ROOT)/isp` | ISP tier (`src/`, `shim/`, `include/`). |
| `FREECODEC_DIR` | `$(FW_ROOT)/codec` | Codec tier (H.264, AAC, `libvenc_base.so`). |
| `PROJECT_ROOT` | repo root | Where `repos/` (toolchain, SDK) is looked up. |
| `BUILD`, `TARGET` | `build`, `mediad` | Object directory and binary name. |

## Clean-room tiers (default on)

| Knob | Default | `1` means | Cleared / `0` means |
| --- | --- | --- | --- |
| `ALGO_RTOS` | 1 | Clean-room ISP 3A + register tier on the RTOS-521 short-enum ABI; the Melis RTOS archive is linked with every member dropped (empty). | Link the SDK's prebuilt 3A archives (`libisp_ae.a`, ...). |
| `FREECODEC_H264` | 1 | Clean-room H.264 encoder objects. | Link the SDK's `libvenc_codec.so` + `libVE.so`. |
| `FREECODEC_VENC_BASE` | `$(FREECODEC_H264)` | Build `libvenc_base.so` from freecodec. | Build it from the SDK's libcedarc glue sources. |
| `FREECODEC_FENC` | 1 | Clean-room vencoder API (`libfenc.a`). | Compile the SDK's `vencoder.c`. |
| `FREECODEC_FCAP` | 1 | Clean-room capture runtime (`libfcap.a`: `AW_MPI_SYS_*`, `AW_MPI_VI_*`). | Compile the SDK's MPP capture layer (`mpi_sys.c`, `mpi_vi.c`, ...). |
| `FREECODEC_FISP` | `$(FREECODEC_FCAP)` | Clean-room ISP runtime (`libfisp.a`: `AW_MPI_ISP_*`); the SDK's `mpi_isp.c` is dropped and `AW_MPI_ISP_*` are stripped from a vendor `mpi_vi.o`. | Compile the SDK's `mpi_isp.c`. Needed whenever `FREECODEC_FCAP=1`. |
| `FREECODEC_HEADERS` | 1 | The daemon's own sources see freewinner's `mw_headers` instead of the SDK headers. | Use the SDK headers. |

## Vendor A/B knobs (default off)

All but `VENDOR_MEDIA_UTILS` act on the `ALGO_RTOS=1` build.

| Knob | Meaning |
| --- | --- |
| `VENDOR_3A` | Space-separated subset of `iso ae awb afs gtm pltm`: keep those vendor archive members and bind them instead of the clean modules. |
| `VENDOR_REG` | `1`: keep the vendor register/config tier (`isp_module_cfg.o`, `isp521_reg_cfg.o`, `isp_base.o`). |
| `VENDOR_MODCFG` | `1`: keep only `isp_module_cfg.o` / `isp521_reg_cfg.o`. |
| `VENDOR_BASE` | `1`: keep only `isp_base.o`. |
| `VENDOR_FW` | `1`: compile the SDK's libisp framework files instead of the clean framework. |
| `VENDOR_MEDIA_UTILS` | `1`: compile the SDK's media utils instead of the clean ones. |

## Other

| Knob | Default | Meaning |
| --- | --- | --- |
| `OPT` | `-O0` | Optimisation for the daemon's own sources (the whole daemon at `-O2` fails bring-up on camera). |
| `CLEAN_OPT` | `-O2` | Optimisation for the clean-room ISP tiers. |
| `EXTRA_CFLAGS` | empty | Appended to every compile, e.g. `-DMEDIAD_BUILD_STAMP='"..."'` (no spaces). |
| `STOCK_REG` | unset | `1`: compile and replay a register table captured from your own camera (`isp_cfg/stock_reg_tbl.c`, see `tools/make_stock_reg.py`). Never commit that file. |

## Targets

- `all` (default): `$(TARGET)`, `mediad_ctl` (control socket client) and
  `rmm_extract` (tuning extractor CLI).
- `clean`: remove `$(BUILD)`, `$(TARGET)`, `mediad_ctl` and `rmm_extract`.
