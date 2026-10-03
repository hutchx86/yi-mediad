# Spec — our H.264 VENC channel (`mediad_venc`)

Behaviour spec only. Inputs, outputs, state, invariants, and the required
interface. It describes a channel that drives the public libcedarc vencoder
API; it is **not** a structural description of any Allwinner middleware source.

## Role

`mediad` used to own its encoder through the MPP VENC middleware
(`mpi_venc.c` + `VideoEnc_Component.c`), a component with a message queue, a
frame pool, and a registry entry. We own the same function directly: a small
per-channel object that turns captured frames into an H.264 elementary stream.

Downstream is unchanged: SPS/PPS are re-emitted before every IDR and each NAL is
published onto `/dev/shm/fshare_frame_buf` exactly as before.

## Interface (interoperability facts)

The libcedarc vencoder API is a public interface (declared in the SDK's
`vencoder.h`). We are allowed to call it; this section records which calls the
channel makes and the contract each must satisfy.

- `VideoEncCreate(FWM_VENC_CODEC_H264)` -> opaque `fwm_venc_handle_t*`, or NULL.
- `VideoEncSetParameter(h, FWM_VENC_PARAM_*, &val)` -> 0 on success.
- `VideoEncGetParameter(h, FWM_VENC_PARAM_H264_SPS_PPS, &fwm_venc_header_blob_t)` -> 0 on
  success; `fwm_venc_header_blob_t{length, data}` holds the SPS+PPS access units
  (Annex-B, `00 00 00 01` prefixed), valid to read but owned by the encoder.
- `VideoEncInit(h, &fwm_venc_base_config_t)` -> 0 on success. Config carries the input
  (capture) size, input stride, output (coded) size, and the input pixel format.
- `AllocInputBuffer(h, &fwm_venc_input_pool_t{count,luma_size,chroma_size})` -> 0
  on success. `luma_size`/`chroma_size` are plane byte sizes **in the encoder's own
  addressing units** (the API expresses them in the same units the rest of the
  API uses; see Open questions).
- `GetOneAllocInputBuffer(h, &fwm_venc_input_picture_t)` -> 0 and fills a buffer whose
  virtual Y/C addresses we may write.
- `ReturnOneAllocInputBuffer(h, &fwm_venc_input_picture_t)` -> returns it to the pool.
- `AddOneInputBuffer(h, &fwm_venc_input_picture_t)` -> queues a frame for encoding.
- `VideoEncodeOneFrame(h)` -> 0 on success (a frame was consumed and encoded).
- `AlreadyUsedInputBuffer(h, &fwm_venc_input_picture_t)` -> 0 once the encoder is done
  reading that input buffer.
- `ValidBitstreamFrameNum(h)` -> count of complete bitstream frames ready.
- `GetOneBitstreamFrame(h, &fwm_venc_output_frame_t)` -> 0 on success; `data0/size0`
  and `data1/size1` are two byte ranges making up one frame.
- `FreeOneBitStreamFrame(h, &fwm_venc_output_frame_t)` -> releases it.
- `VideoEncDestroy(h)`.

`fwm_venc_base_config_t` needs the encoder's memory/VE ops: they are filled by
`VideoEncCreate`/`VideoEncInit` themselves (the framework obtains them from the
cedarc glue), so the caller supplies geometry, format, and flags only.

## Configuration values (ours)

These are `mediad`'s own quality parameters and were already in `main.c` before
this channel existed; the spec fixes them as the channel's defaults, all
env-overridable in `main.c`:

| Parameter | Default | Meaning |
|---|---|---|
| fps (source = destination) | 20 | capture and encode rate |
| bitrate | HIGH 2.8 Mbps / LOW 0.7 Mbps | VBR target |
| GOP / max key interval | 40 | IDR period, frames (the codec pins 40) |
| profile | 2 (High) | 0 baseline / 1 main / 2 high |
| level | 32 (3.2) | matches coded size ≤ 2304×1296 |
| QP range | min 10, max 40 | rate-control floor/ceiling |
| RC mode | VBR (FWM_VENC_RC_VBR) | 0 CBR / 1 VBR / 2 AVBR |
| encoder 3DNR | 0 (off) | 0-3; rmm's level 3 smears motion |
| coding mode | frame | one frame per `VideoEncodeOneFrame` |
| GOP mode | normal P | one reference picture, periodic IDR |
| input format | Allwinner LBC 2.5X (`FWM_VENC_PIXEL_LBC`) | matches the VI capture format |

## Behaviour

### Open
1. Create the H.264 encoder device.
2. Program the configuration above through the parameter API.
3. Initialise with the capture size, an input stride, the coded size, and the
   input format.
4. Snapshot SPS/PPS once so they can be re-emitted.

### Encode one frame (given a captured luma/chroma virtual address + stride)
1. Present the frame to the encoder (queue it).
2. Run one encode step.
3. Reclaim the frame buffer once the encoder has finished with it.
4. If a complete bitstream frame is ready, take it, hand the two byte ranges to
   the caller, and let the caller release it after publishing.

### Force a keyframe
Request that the next encoded frame be an IDR (used on input gaps and after
stalls so consumers resync).

### Runtime bitrate change
Re-program the VBR target while running.

### Close
Destroy the encoder instance (which frees the picture ring and bitstream
buffers it allocated).

## Invariants

- Only this channel's own thread calls **encode**; open/close happen before the
  thread starts and after it is joined.
- A frame handed to `encode` is always reclaimed on every path (success,
  rejection, or error) so the picture ring never leaks across a stall.
- SPS/PPS are available before the first IDR is published.
- The coded size never exceeds the capture size.

## Resolved questions (against the SDK, not the vendor middleware)

1. **`fwm_venc_input_pool_t` units.** Not used: the channel feeds the encoder
   the captured frame's own physical addresses via `AddOneInputBuffer`
   (zero-copy), so no encoder-side input pool or `AllocInputBuffer` is needed.
2. **Input presentation.** The encoder consumes the captured buffer's physical
   address (`fwm_venc_input_picture_t.luma_phys/chroma_phys`); the VI frame already carries
   it, so no mapping step is required.

## Provenance

This spec was written from: the public vencoder API surface, `mediad`'s own
pre-existing encoder parameters, and the downstream ring contract. No Allwinner
middleware source, decompilation, or symbol-level transcription was used. The
implementation must be written from this spec alone.
