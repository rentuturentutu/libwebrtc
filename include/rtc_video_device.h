#ifndef LIB_WEBRTC_RTC_VIDEO_DEVICE_HXX
#define LIB_WEBRTC_RTC_VIDEO_DEVICE_HXX

#include "rtc_types.h"

namespace libwebrtc {

// Reports the encoder's framerate decisions. `fps` is -1 when nothing
// constrains the rate, otherwise the maximum the encoder currently wants.
//
// A plain function pointer, not std::function: this header crosses the DLL
// boundary, libwebrtc is built with clang-cl (libc++) while consumers use MSVC
// STL, and passing a std::function between the two reads one layout as the
// other. That crashed on the first call -- see the dump analysed 2026-08-03,
// `std::__Cr::__function::__policy_func` dereferencing 0xd. Every other type on
// this interface is POD for the same reason.
typedef void (*MaxFramerateObserver)(int fps, void* user);
typedef void (*TextureReleaseCallback)(void* lease);


class RTCVideoCapturer : public RefCountInterface {
 public:
  virtual ~RTCVideoCapturer() {}

  virtual bool StartCapture() = 0;

  virtual bool CaptureStarted() = 0;

  virtual void StopCapture() = 0;

  // The methods below are only meaningful for a capturer obtained from
  // RTCPeerConnectionFactory::CreateExternalVideoCapturer, where the host
  // application produces the frames. They return false on any other capturer.

  // Packed BGRA (B,G,R,A in memory order, as produced by DXGI/GDI/scrap).
  // `stride` is in bytes; pass 0 for width * 4. `timestamp_us` may be 0 to let
  // the library stamp the frame. Safe to call from any thread.
  virtual bool PushFrameBGRA(const uint8_t* data, int stride, int width,
                             int height, int64_t timestamp_us) {
    return false;
  }

  virtual bool PushFrameI420(const uint8_t* data_y, int stride_y,
                             const uint8_t* data_u, int stride_u,
                             const uint8_t* data_v, int stride_v, int width,
                             int height, int64_t timestamp_us) {
    return false;
  }

  // A BGRA D3D11 texture that never leaves the GPU. `device` and `texture` are
  // an `ID3D11Device*` / `ID3D11Texture2D*` pair. `shared_handle` is the
  // producer-owned legacy shared handle and must not be closed. `luid`
  // identifies the adapter they live on, and the encoder refuses the frame
  // unless it matches its own.
  // On success the library owns `lease` and invokes `release` after the last
  // WebRTC reference to the texture is gone. On failure ownership stays with
  // the caller. This allows the producer's texture to be used directly without
  // another GPU copy.
  //
  // Returns false when the frame was dropped (all pool slots still in flight),
  // when this build has no hardware-encode support, or on a D3D error. Windows
  // only; always false elsewhere.
  virtual bool PushFrameTexture(void* device, void* texture,
                                void* shared_handle, int64_t luid, int width,
                                int height, int64_t timestamp_us, void* lease,
                                TextureReleaseCallback release) {
    return false;
  }

  // Drives a synthetic moving test pattern, for validating the injection path
  // without a real frame producer.
  virtual bool StartTestPattern() { return false; }

  virtual void StopTestPattern() {}

  // True when the frames represent screen content rather than camera output.
  // The encoder keeps resolution and drops frame rate under load in that case,
  // and can skip re-encoding an unchanged desktop.
  virtual bool IsScreencast() const { return false; }

  // Receives the encoder's framerate decisions so the host can throttle its
  // frame producer, instead of producing frames that the library then drops.
  // Without this the host pays for the acquire and the colour conversion of
  // every surplus frame.
  //
  // Fires on a library thread, only when the value moves meaningfully (not once
  // per frame), and never after this is called again with nullptr. `user` is
  // passed back untouched. Only meaningful for an external capturer.
  //
  // Declared last on purpose: inserting a virtual function in the middle of this
  // class shifts the vtable slots of everything below it, which breaks any
  // consumer that was not rebuilt against the same header.
  virtual void SetMaxFramerateObserver(MaxFramerateObserver observer,
                                       void* user) {}
};



class RTCVideoDevice : public RefCountInterface {
 public:
  virtual uint32_t NumberOfDevices() = 0;

  virtual int32_t GetDeviceName(uint32_t deviceNumber, char* deviceNameUTF8,
                                uint32_t deviceNameLength,
                                char* deviceUniqueIdUTF8,
                                uint32_t deviceUniqueIdUTF8Length,
                                char* productUniqueIdUTF8 = 0,
                                uint32_t productUniqueIdUTF8Length = 0) = 0;

  virtual scoped_refptr<RTCVideoCapturer> Create(const char* name,
                                                 uint32_t index, size_t width,
                                                 size_t height,
                                                 size_t target_fps) = 0;

 protected:
  virtual ~RTCVideoDevice() {}
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_VIDEO_DEVICE_HXX
