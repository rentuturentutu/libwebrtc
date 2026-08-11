// Declarations for the dgj_encode Rust static library
// (remote_desktop/rust/dgj_encode). Keep in sync with its src/lib.rs.
#ifndef DGJ_ENCODE_FFI_H
#define DGJ_ENCODE_FFI_H

#include <stddef.h>
#include <stdint.h>

extern "C" {

#define DGJ_OK 0
#define DGJ_ERR_INVALID_ARG -1
#define DGJ_ERR_BUFFER_TOO_SMALL -2
#define DGJ_ERR_OPEN_FAILED -3
#define DGJ_ERR_ENCODE_FAILED -4
// The I420 -> NV12 conversion's three refusal reasons, kept distinct so a field
// failure names the failing precondition.
#define DGJ_ERR_BAD_LAYOUT -5
#define DGJ_ERR_DST_STRIDE -6
#define DGJ_ERR_DST_BOUNDS -7

// Probe which hardware encoders this machine can actually use. Fills `json_out`
// with a JSON array of {"name","format","priority","hwdevice"} entries; names
// are e.g. "h264_qsv". Returns DGJ_OK or DGJ_ERR_BUFFER_TOO_SMALL (buffer left
// untouched). Expensive (actually opens encoders); call once and cache.
int dgj_enc_probe(int width,
                  int height,
                  int kbs,
                  int fps,
                  char* json_out,
                  size_t cap);

// Callback for one encoded frame. `data` is Annex-B and only valid for the
// duration of the call. One dgj_enc_encode call can produce 0..n frames.
typedef void (*DgjEncodedCb)(const uint8_t* data,
                             size_t len,
                             int64_t pts_ms,
                             int key,
                             void* user);

// Open a hardware encoder. `name` must be one of the names returned by
// dgj_enc_probe (e.g. "h264_qsv"); returns null on failure.
// `gop` is the keyframe interval in frames; finite values self-heal missed
// keyframe requests -- callers must NOT pass INT32_MAX.
void* dgj_enc_create(const char* name,
                     int32_t width,
                     int32_t height,
                     int32_t kbs,
                     int32_t fps,
                     int32_t gop);

// Encode one I420 frame. `ms` is the presentation timestamp in milliseconds.
// `force_key` applies to this frame without reopening the encoder.
int dgj_enc_encode(void* handle,
                   const uint8_t* y,
                   int32_t stride_y,
                   const uint8_t* u,
                   int32_t stride_u,
                   const uint8_t* v,
                   int32_t stride_v,
                   int64_t ms,
                   int force_key,
                   DgjEncodedCb cb,
                   void* user);

// Poll output from already submitted asynchronous frames without flushing or
// closing the encoder. EAGAIN is reported as DGJ_OK with no callback.
int dgj_enc_drain(void* handle, DgjEncodedCb cb, void* user);

// Change the target bitrate in kbps. Cheap; WebRTC calls SetRates() often.
int dgj_enc_set_bitrate(void* handle, int32_t kbs);

// Report the NV12 layout FFmpeg chose at open time. Diagnostics only: a
// DGJ_ERR_DST_* return can only be interpreted next to these numbers.
int dgj_enc_layout(void* handle,
                   int32_t* length,
                   int32_t* ls_y,
                   int32_t* ls_uv,
                   int32_t* off_uv);

// Cumulative split of what one encode costs: microseconds in the I420->NV12
// copy vs microseconds inside the encoder, plus the frame count. WebRTC's
// `encode` metric is the sum of the two, so this is the only way to tell a slow
// chroma interleave apart from a slow GPU round trip. Caller takes deltas.
int dgj_enc_timing(void* handle,
                   int64_t* conv_us,
                   int64_t* enc_us,
                   int64_t* frames);

void dgj_enc_destroy(void* handle);

// ---------------------------------------------------------------------------
// VRAM path (dgj_encode's src/vram_encode.rs, built only with --features vram).
// Same encoder family, but the input is a D3D11 BGRA texture instead of I420 in
// system memory, which removes the upload measured at 2.8ms (1080p) to 8.4ms
// (2256x1504).
// ---------------------------------------------------------------------------

// No hardware encoder on the requested adapter. Distinct from
// DGJ_ERR_OPEN_FAILED so the caller can tell "wrong GPU" (stop trying, use the
// I420 path) from "open failed" (worth one retry).
#define DGJ_ERR_NO_ENCODER -8
// The bounded asynchronous hardware queue is full. Drop this frame and keep
// the encoder; this is backpressure, not a device failure.
#define DGJ_ERR_WOULD_BLOCK -9
#define DGJ_ERR_DECODE_FAILED -10

#define DGJ_CODEC_H264 0
#define DGJ_CODEC_H265 1

// Probe VRAM encoders. Entries are {"driver","vendor","luid","format"}; the
// `luid` is the point of it -- only an adapter listed here can encode a texture
// that lives on it. Result is cached inside the library. Expensive; call once.
int dgj_venc_probe(int width,
                   int height,
                   int kbs,
                   int fps,
                   char* json_out,
                   size_t cap);

// Open an encoder for `luid`, using `device` (the capturer's ID3D11Device) so no
// cross-device sharing is needed. `driver` may be NULL for "whatever the probe
// found". Returns NULL on failure; dgj_venc_last_reason() says which kind.
void* dgj_venc_create(int64_t luid,
                      void* device,
                      const char* driver,
                      int fmt,
                      int width,
                      int height,
                      int kbs,
                      int fps,
                      int gop);

int dgj_venc_last_reason();

// Active VRAM backend for diagnostics: NV=0, AMF=1, MFX=2, FFMPEG=3.
// Returns -1 for a null handle.
int dgj_venc_backend(void* handle);

// Native NVIDIA input path: 2=dedicated pre-registered ARGB ring,
// 1=GPU-converted NV12 ring, 0=direct external ARGB. Other backends and null
// handles return -1.
int dgj_venc_input_mode(void* handle);

// Runtime values selected by the native encoder after codec defaults and
// environment overrides: input mode, VBV frames, extra async output depth and
// the resulting encoder buffer count.
int dgj_venc_config(void* handle,
                    int32_t* input_mode,
                    int32_t* vbv_frames,
                    int32_t* async_depth,
                    int32_t* buffer_count);

// Encode one BGRA texture. Must live on the adapter this encoder was created
// for. NVIDIA forces an IDR in place; other backends retain a rebuild fallback.
int dgj_venc_encode(void* handle,
                    void* texture,
                    void* shared_handle,
                    int64_t ms,
                    int force_key,
                    DgjEncodedCb cb,
                    void* user);

// Retrieve already submitted output without flushing or closing the session.
// Used for the last frame of a low-depth NVENC pipeline when input goes idle.
int dgj_venc_drain(void* handle, DgjEncodedCb cb, void* user);

int dgj_venc_set_bitrate(void* handle, int kbs);

// Exists here but not on the RAM path: a 30<->60fps switch no longer needs a
// rebuild.
int dgj_venc_set_framerate(void* handle, int fps);

// Microseconds inside the encoder plus the frame count. No conversion term --
// that is the whole point of this path. Caller takes deltas.
int dgj_venc_timing(void* handle, int64_t* enc_us, int64_t* frames);

// Native NVIDIA stages, cumulative since open. `submit` includes resource
// map + nvEncEncodePicture; `completion` waits for and locks the bitstream;
// `ready` is time spent waiting for the WebRTC worker to collect that packet.
int dgj_venc_stage_timing(void* handle,
                          int64_t* submit_us,
                          int64_t* completion_us,
                          int64_t* ready_us,
                          int64_t* frames,
                          int64_t* submit_max_us,
                          int64_t* completion_max_us,
                          int64_t* ready_max_us);

// Detailed NVENC stages since the previous snapshot. Unlike the aggregate
// timing API above, this snapshot is destructive: totals, maxima and frame
// count reset after a successful call.
typedef struct DgjNvencStageDetail {
  int64_t frames;
  int64_t submit_gate_wait_us;
  int64_t submit_gate_wait_max_us;
  int64_t copy_us;
  int64_t copy_max_us;
  int64_t register_us;
  int64_t register_max_us;
  int64_t map_us;
  int64_t map_max_us;
  int64_t encode_picture_us;
  int64_t encode_picture_max_us;
  int64_t event_wait_us;
  int64_t event_wait_max_us;
  int64_t bitstream_lock_us;
  int64_t bitstream_lock_max_us;
  int64_t completion_gate_wait_us;
  int64_t completion_gate_wait_max_us;
  int64_t bitstream_unlock_us;
  int64_t bitstream_unlock_max_us;
  int64_t unmap_us;
  int64_t unmap_max_us;
  int64_t d3d_lock_us;
  int64_t d3d_lock_max_us;
  int64_t shared_resolve_us;
  int64_t shared_resolve_max_us;
} DgjNvencStageDetail;

int dgj_venc_stage_detail_timing(void* handle,
                                 DgjNvencStageDetail* detail);

void dgj_venc_destroy(void* handle);

// ---------------------------------------------------------------------------
// D3D11 hardware decode. The callback texture is owned by the decoder and is
// valid only during the callback unless the receiver AddRefs it.
// ---------------------------------------------------------------------------

typedef void (*DgjDecodedCb)(void* texture,
                             int width,
                             int height,
                             void* user);

int dgj_vdec_available(int codec);
void* dgj_vdec_create(int codec);
int dgj_vdec_backend(void* handle);
int dgj_vdec_decode(void* handle,
                    const uint8_t* data,
                    size_t len,
                    DgjDecodedCb cb,
                    void* user);
void dgj_vdec_destroy(void* handle);
}

#endif  // DGJ_ENCODE_FFI_H
