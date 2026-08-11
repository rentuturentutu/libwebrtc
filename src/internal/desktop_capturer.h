#ifndef LIB_WEBRTC_DESKTOP_CAPTURER_IMPL_HXX
#define LIB_WEBRTC_DESKTOP_CAPTURER_IMPL_HXX

#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#ifdef WEBRTC_WIN
#include "modules/desktop_capture/win/window_capture_utils.h"
#endif
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "include/base/refcount.h"
#include "media/base/video_adapter.h"
#include "media/base/video_broadcaster.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/thread.h"
#include "rtc_desktop_device.h"
#include "rtc_types.h"
#include "src/internal/vcm_capturer.h"
#include "src/internal/video_capturer.h"
#include "src/rtc_desktop_capturer_impl.h"
#include "third_party/libyuv/include/libyuv.h"

namespace libwebrtc {

class ScreenCapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<ScreenCapturerTrackSource> Create(
      scoped_refptr<RTCDesktopCapturer> capturer) {
    if (capturer) {
      return rtc::make_ref_counted<ScreenCapturerTrackSource>(capturer);
    }
    return nullptr;
  }

 public:
  explicit ScreenCapturerTrackSource(scoped_refptr<RTCDesktopCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}
  virtual ~ScreenCapturerTrackSource() { capturer_->Stop(); }

 private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return static_cast<RTCDesktopCapturerImpl*>(capturer_.get());
  }

  // 关键：标记为屏幕内容源。默认 VideoTrackSource::is_screencast() 返回 false，
  // 导致编码器把桌面当普通摄像头视频处理（静止画面也满码率）。返回 true 后
  // libwebrtc 启用 screen-content 码控：静止时码率大幅下降、只给变化区域花码率，
  // 是降低公网码率/抖动的根治开关。
  bool is_screencast() const override { return true; }

  // 屏幕内容无需去噪（去噪是给摄像头噪点用的，对屏幕反而增加开销并模糊文字）。
  absl::optional<bool> needs_denoising() const override { return false; }

  // webrtc::VideoTrackSource 只把 AddOrUpdateSink/RemoveSink 转发给 source()，
  // 下面两个接口都没有转发，落到基类空实现，因此必须在这里显式接线。

  // 编码器侧 zero-hertz 适配器在丢帧后会请求刷新帧；不接这条链路的话画面会停
  // 在旧内容不自动补帧。
  void RequestRefreshFrame() override {
    static_cast<RTCDesktopCapturerImpl*>(capturer_.get())
        ->RequestRefreshFrame();
  }

  // 把采集器上报的 {min_fps=0, max_fps>0} 送到所有 sink，这是 zero-hertz 的激活
  // 条件。采集器内部也会直接调，这里 override 是为了让应用层通过 track source
  // 公共接口调用时同样生效。
  void ProcessConstraints(
      const webrtc::VideoTrackSourceConstraints& constraints) override {
    static_cast<RTCDesktopCapturerImpl*>(capturer_.get())
        ->ProcessConstraints(constraints);
  }

  scoped_refptr<RTCDesktopCapturer> capturer_;
};

}  // namespace libwebrtc
#endif