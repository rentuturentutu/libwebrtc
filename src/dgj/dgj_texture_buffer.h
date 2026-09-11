/*
 * DgjTexturePool / DgjD3D11TextureBuffer -- a kNative VideoFrameBuffer carrying a
 * BGRA ID3D11Texture2D, so a captured frame can reach the hardware encoder
 * without a round trip through system memory.
 *
 * dgj_capture owns the texture ring and hands each frame over with an opaque
 * lease. This buffer returns the lease only after WebRTC drops its last frame
 * reference, so the capture texture can be wrapped directly without a second
 * CopyResource.
 *
 * Threading: the D3D11 immediate context is not thread safe. The normal native
 * path does not touch it here; the ToI420 fallback serializes its staging copy
 * and map through the pool's lock.
 */
#ifndef LIBWEBRTC_SRC_DGJ_DGJ_TEXTURE_BUFFER_H_
#define LIBWEBRTC_SRC_DGJ_DGJ_TEXTURE_BUFFER_H_

#include <d3d11.h>

#include <cstdint>
#include <mutex>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "rtc_base/ref_count.h"
#include "rtc_base/ref_counted_object.h"

namespace libwebrtc {

class DgjTexturePool;

// A single captured frame, still on the GPU. `type()` is kNative, so an encoder
// that advertises `supports_native_handle` receives it untouched; anything else
// in the pipeline falls back to ToI420().
class DgjD3D11TextureBuffer : public webrtc::VideoFrameBuffer {
 public:
  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  // Downloads the texture and converts it. Not cheap -- that is the whole point
  // of this class -- but it must work, because the software fallback wrapper, the
  // quality scaler and the JPEG thumbnail path all reach for it.
  rtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;

  // For the encoder: the raw texture and the adapter it lives on. The encoder
  // MUST compare `luid()` against its own before using `texture()`.
  ID3D11Texture2D* texture() const { return texture_; }
  void* shared_handle() const { return shared_handle_; }
  int64_t luid() const { return luid_; }
  // The device the texture belongs to; the encoder needs it to open a session
  // that can consume the texture without a cross-device copy.
  ID3D11Device* device() const;

  // What `texture()` actually holds. 0 = BGRA (the historical contract), 1 = the
  // decoder's NV12 surface, in which case `array_slice()` selects the plane pair
  // inside the decode pool's texture array. `bt709()`/`full_range()` come from the
  // bitstream colour space when present; they default to BT.601 studio, which is
  // what the BGRA shader path has always assumed, so colours do not shift when the
  // NV12 path is enabled.
  int d3d11_format() const { return d3d11_format_; }
  int array_slice() const { return array_slice_; }
  bool bt709() const { return bt709_; }
  bool full_range() const { return full_range_; }
  void SetD3D11Format(int format, int array_slice, bool bt709,
                      bool full_range) {
    d3d11_format_ = format;
    array_slice_ = array_slice;
    bt709_ = bt709;
    full_range_ = full_range;
  }


  // Public only because rtc::RefCountedObject constructs it. Use
  // DgjTexturePool::Wrap.
  DgjD3D11TextureBuffer(rtc::scoped_refptr<DgjTexturePool> pool,
                        ID3D11Texture2D* texture,
                        void* shared_handle,
                        void* lease,
                        void (*release)(void*),
                        int64_t luid,
                        int width,
                        int height);

 protected:
  ~DgjD3D11TextureBuffer() override;

 private:
  const rtc::scoped_refptr<DgjTexturePool> pool_;
  // Kept alive by `lease_`, owned by dgj_capture.
  ID3D11Texture2D* const texture_;
  // Producer-owned and valid for the lifetime of lease_. Never close it here.
  void* const shared_handle_;
  void* const lease_;
  void (*const release_)(void*);
  const int64_t luid_;
  const int width_;
  const int height_;
  int d3d11_format_ = 0;
  int array_slice_ = 0;
  bool bt709_ = false;
  bool full_range_ = false;
};

class DgjTexturePool : public rtc::RefCountInterface {
 public:
  // `device` is the capturer's ID3D11Device (from DgjTexFrame::device); copying
  // cannot cross devices, so it must be that one and not a new one.
  static rtc::scoped_refptr<DgjTexturePool> Create(void* device, int64_t luid);

  // Wraps a producer-leased texture directly. On success this object owns the
  // lease; on failure ownership remains with the caller.
  rtc::scoped_refptr<DgjD3D11TextureBuffer> Wrap(void* src,
                                                 void* shared_handle,
                                                 int width,
                                                 int height,
                                                 void* lease,
                                                 void (*release)(void*));

  int64_t luid() const { return luid_; }
  ID3D11Device* device() const { return device_; }

  // Public only because rtc::RefCountedObject constructs it. Use Create().
  DgjTexturePool(ID3D11Device* device,
                 ID3D11DeviceContext* context,
                 int64_t luid);

 protected:
  ~DgjTexturePool() override;

 private:
  friend class DgjD3D11TextureBuffer;

  // `array_slice` picks the entry inside a decode pool's texture array; it is 0
  // for the ordinary single-texture BGRA case. NV12 sources are converted with
  // NV12ToI420, BGRA ones with ARGBToI420.
  bool DownloadToI420(ID3D11Texture2D* texture,
                      int array_slice,
                      int width,
                      int height,
                      uint8_t* dst_y,
                      int stride_y,
                      uint8_t* dst_u,
                      int stride_u,
                      uint8_t* dst_v,
                      int stride_v);

  ID3D11Device* const device_;
  ID3D11DeviceContext* const context_;
  const int64_t luid_;

  mutable std::mutex lock_;
  int width_ = 0;
  int height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  // Reused download target for ToI420, allocated on first use.
  ID3D11Texture2D* staging_ = nullptr;
  int64_t wrapped_ = 0;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_SRC_DGJ_DGJ_TEXTURE_BUFFER_H_
