#ifndef INTERNAL_EXTERNAL_CAPTURER_H_
#define INTERNAL_EXTERNAL_CAPTURER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "pc/video_track_source.h"
#include "src/internal/video_capturer.h"
#if defined(USE_DGJ_HWCODEC)
#include "src/dgj/dgj_texture_buffer.h"
#endif

namespace webrtc {
namespace internal {

// A VideoCapturer whose frames are pushed in from outside the library
// (for example a Rust/C++ desktop grabber living in the host application).
//
// Frames are converted to I420 and handed to VideoCapturer::OnFrame, which
// takes care of the VideoAdapter scaling and broadcasting to every sink.
class ExternalCapturer : public VideoCapturer {
 public:
  static std::shared_ptr<ExternalCapturer> Create(size_t width, size_t height,
                                                  size_t target_fps);

  ExternalCapturer(size_t width, size_t height, size_t target_fps);
  ~ExternalCapturer() override;

  bool StartCapture() override;
  bool CaptureStarted() override;
  void StopCapture() override;

  // Pushes a packed BGRA/ARGB-in-memory frame. `stride` is in bytes; pass 0 to
  // assume width * 4. Returns false when the capturer is stopped or the
  // arguments are inconsistent. Safe to call from any thread.
  bool PushFrameBGRA(const uint8_t* data, int stride, int width, int height,
                     int64_t timestamp_us);

  // Pushes a planar I420 frame. Returns false on invalid arguments.
  bool PushFrameI420(const uint8_t* data_y, int stride_y, const uint8_t* data_u,
                     int stride_u, const uint8_t* data_v, int stride_v,
                     int width, int height, int64_t timestamp_us);

#if defined(USE_DGJ_HWCODEC)
  // Pushes a BGRA D3D11 texture without touching system memory. `device` and
  // `texture` come straight from dgj_capture's DgjTexFrame. On success the
  // supplied lease is released after the resulting frame's last reference.
  //
  // Returns false when the frame had to be dropped (every pool slot still in
  // flight) or on a D3D error. A false return is not fatal: the caller should
  // count it and carry on.
  bool PushFrameTexture(void* device, void* texture, void* shared_handle,
                        int64_t luid, int width, int height,
                        int64_t timestamp_us, void* lease,
                        void (*release)(void*));
#endif


  // Drives a synthetic moving test pattern on an internal thread. Used to
  // validate the injection path end to end without a real grabber.
  bool StartTestPattern();
  void StopTestPattern();

  // Registers a sink for the encoder's framerate decisions, so the host can
  // throttle its grabber instead of letting frames be produced and then dropped.
  //
  // A plain function pointer, not std::function: this crosses the DLL boundary
  // via RTCVideoCapturer, where libc++ and MSVC STL layouts do not match. See
  // the typedef in include/rtc_video_device.h.
  //
  // The callback fires on whichever thread updated the sink wants (the encoder
  // queue in practice), and only when the value actually changes past the
  // hysteresis below -- not once per frame. Pass nullptr to detach; the capturer
  // never calls it after that returns.
  void SetMaxFramerateObserver(void (*observer)(int, void*), void* user);

 protected:
  void OnMaxFramerateChanged(int fps) override;

 private:
  void TestPatternLoop();
  void DeliverI420(rtc::scoped_refptr<I420Buffer> buffer, int64_t timestamp_us);

  const size_t width_;
  const size_t height_;
  const size_t target_fps_;

  std::atomic<bool> capturing_{false};
  std::atomic<bool> test_pattern_running_{false};
  std::unique_ptr<std::thread> test_pattern_thread_;
  // BGRA->I420 cost, logged every 300 frames. Only touched from the capture
  // thread, which is the only caller of PushFrameBGRA in production; the test
  // pattern thread is the other caller and never runs at the same time.
  int64_t bgra_conv_us_ = 0;
  int64_t bgra_conv_us_max_ = 0;
  int bgra_conv_frames_ = 0;
  // Framerate throttling handed back to the host's grabber. Guarded because
  // OnMaxFramerateChanged runs on the encoder queue while the observer is
  // installed and removed from the signalling side.
  std::mutex fps_observer_lock_;
  void (*fps_observer_)(int, void*) = nullptr;
  void* fps_observer_user_ = nullptr;
  // Last value handed to the observer, so an unchanged (or barely changed)
  // decision does not cross the FFI boundary again.
  int last_reported_fps_ = 0;
#if defined(USE_DGJ_HWCODEC)
  // Created on the first texture frame, because the device and LUID only become
  // known then. Never used by the BGRA/I420 paths.
  rtc::scoped_refptr<libwebrtc::DgjTexturePool> tex_pool_;
  int64_t tex_pushed_ = 0;
#endif
};

// Track source that reports screen content. The encoder side uses this to keep
// resolution and sacrifice frame rate under load (text stays readable), and to
// enable the zero-hertz cadence adapter so an idle desktop is not re-encoded.
class ExternalTrackSource : public webrtc::VideoTrackSource {
 public:
  explicit ExternalTrackSource(std::shared_ptr<VideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(capturer) {}

  bool is_screencast() const override { return true; }

  // Forwards the encoder's constraints into the capturer, which replays them to
  // its sinks. This is what lets the frame cadence adapter enter zero-hertz mode
  // for screen content, matching what the desktop capturer track source does.
  void ProcessConstraints(
      const webrtc::VideoTrackSourceConstraints& constraints) override {
    if (capturer_) {
      capturer_->ProcessConstraints(constraints);
    }
  }

 private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::shared_ptr<VideoCapturer> capturer_;
};


}  // namespace internal
}  // namespace webrtc

#endif  // INTERNAL_EXTERNAL_CAPTURER_H_
