#ifndef LIB_WEBRTC_RTC_VIDEO_FRAME_HXX
#define LIB_WEBRTC_RTC_VIDEO_FRAME_HXX

#include "rtc_types.h"

namespace libwebrtc {

class RTCVideoFrame : public RefCountInterface {
 public:
  enum class Type { kARGB, kBGRA, kABGR, kRGBA };

  enum VideoRotation {
    kVideoRotation_0 = 0,
    kVideoRotation_90 = 90,
    kVideoRotation_180 = 180,
    kVideoRotation_270 = 270
  };

 public:
  LIB_WEBRTC_API static scoped_refptr<RTCVideoFrame> Create(
      int width, int height, const uint8_t* buffer, int length);

  LIB_WEBRTC_API static scoped_refptr<RTCVideoFrame> Create(
      int width, int height, const uint8_t* data_y, int stride_y,
      const uint8_t* data_u, int stride_u, const uint8_t* data_v, int stride_v);

  virtual scoped_refptr<RTCVideoFrame> Copy() = 0;

  // The resolution of the frame in pixels. For formats where some planes are
  // subsampled, this is the highest-resolution plane.
  virtual int width() const = 0;
  virtual int height() const = 0;

  virtual VideoRotation rotation() = 0;

  // Returns pointer to the pixel data for a given plane. The memory is owned by
  // the VideoFrameBuffer object and must not be freed by the caller.
  virtual const uint8_t* DataY() const = 0;
  virtual const uint8_t* DataU() const = 0;
  virtual const uint8_t* DataV() const = 0;

  // Returns the number of bytes between successive rows for a given plane.
  virtual int StrideY() const = 0;
  virtual int StrideU() const = 0;
  virtual int StrideV() const = 0;

  // Windows hardware decoders may keep the decoded BGRA frame in a shareable
  // D3D11 texture. Returns its DXGI shared HANDLE as an opaque pointer, or
  // nullptr for ordinary CPU-backed frames. The frame owns the texture and the
  // handle is valid only while this RTCVideoFrame remains referenced.
  virtual void* D3D11SharedHandle() const { return nullptr; }

  virtual int ConvertToARGB(Type type, uint8_t* dst_argb, int dst_stride_argb,
                            int dest_width, int dest_height) = 0;

  // Sender capture time mapped into the receiver's NTP clock by WebRTC. This
  // survives RTP, jitter buffering and decode, and lets the host measure true
  // capture-to-present latency without comparing unsynchronised wall clocks.
  // Declared last to preserve every existing vtable slot.
#if defined(_WIN32)
  virtual int64_t capture_time_ms() const { return -1; }

  // Same-process fast path for hardware-decoded frames. These borrowed
  // pointers remain valid only while this frame is referenced. Renderers must
  // fall back to D3D11SharedHandle when they cannot use the decoder device.
  virtual void* D3D11Texture() const { return nullptr; }
  virtual void* D3D11Device() const { return nullptr; }

  // What D3D11Texture() holds: 0 = BGRA, 1 = the decoder's NV12 surface. For
  // NV12, D3D11ArraySlice() selects the entry in the decode pool's texture array,
  // and the two colour flags say how to convert it. A renderer that can consume
  // NV12 avoids the decoder's conversion draw and the renderer-side copy. Appended
  // last so existing vtable slots keep their positions.
  virtual int D3D11Format() const { return 0; }
  virtual int D3D11ArraySlice() const { return 0; }
  virtual bool D3D11IsBt709() const { return false; }
  virtual bool D3D11IsFullRange() const { return false; }
#endif

 protected:
  virtual ~RTCVideoFrame() {}
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_VIDEO_FRAME_HXX
