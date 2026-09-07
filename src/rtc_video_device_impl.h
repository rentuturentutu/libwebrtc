#ifndef LIB_WEBRTC_VIDEO_DEVICE_IMPL_HXX
#define LIB_WEBRTC_VIDEO_DEVICE_IMPL_HXX

#include <memory>

#include "modules/video_capture/video_capture.h"
#include "rtc_base/thread.h"
#include "rtc_video_device.h"
#include "src/internal/external_capturer.h"
#if defined(USE_DGJ_HWCODEC)
#include "src/dgj/dgj_encode_ffi.h"
#endif
#include "src/internal/vcm_capturer.h"
#include "src/internal/video_capturer.h"

namespace libwebrtc {

class RTCVideoCapturerImpl : public RTCVideoCapturer {
 public:
  RTCVideoCapturerImpl(
      std::shared_ptr<webrtc::internal::VideoCapturer> video_capturer)
      : video_capturer_(video_capturer) {}
  std::shared_ptr<webrtc::internal::VideoCapturer> video_capturer() {
    return video_capturer_;
  }

  bool StartCapture() override {
    return video_capturer_ != nullptr && video_capturer_->StartCapture();
  }

  bool CaptureStarted() override {
    return video_capturer_ != nullptr && video_capturer_->CaptureStarted();
  }

  void StopCapture() override {
    if (video_capturer_ != nullptr) video_capturer_->StopCapture();
  }

 private:
  std::shared_ptr<webrtc::internal::VideoCapturer> video_capturer_;
};

// Capturer whose frames come from the host application. Derives from
// RTCVideoCapturerImpl so the static_cast in
// RTCPeerConnectionFactoryImpl::CreateVideoSource_s stays valid and the
// existing CreateVideoSource path can be reused unchanged.
class RTCExternalVideoCapturerImpl : public RTCVideoCapturerImpl {
 public:
  explicit RTCExternalVideoCapturerImpl(
      std::shared_ptr<webrtc::internal::ExternalCapturer> capturer)
      : RTCVideoCapturerImpl(capturer), external_capturer_(capturer) {}

  bool PushFrameBGRA(const uint8_t* data, int stride, int width, int height,
                     int64_t timestamp_us) override {
    return external_capturer_ != nullptr &&
           external_capturer_->PushFrameBGRA(data, stride, width, height,
                                            timestamp_us);
  }

  bool PushFrameI420(const uint8_t* data_y, int stride_y, const uint8_t* data_u,
                     int stride_u, const uint8_t* data_v, int stride_v,
                     int width, int height, int64_t timestamp_us) override {
    return external_capturer_ != nullptr &&
           external_capturer_->PushFrameI420(data_y, stride_y, data_u, stride_u,
                                             data_v, stride_v, width, height,
                                             timestamp_us);
  }

  bool PushFrameTexture(void* device, void* texture, void* shared_handle,
                        int64_t luid, int width, int height,
                        int64_t timestamp_us, void* lease,
                        TextureReleaseCallback release) override {
#if defined(USE_DGJ_HWCODEC)
    return external_capturer_ != nullptr &&
           external_capturer_->PushFrameTexture(
               device, texture, shared_handle, luid, width, height,
               timestamp_us, lease, release);
#else
    return false;
#endif
  }

  void SetMaxFramerateObserver(MaxFramerateObserver observer,
                               void* user) override {
    if (external_capturer_ != nullptr) {
      external_capturer_->SetMaxFramerateObserver(observer, user);
    }
  }

  bool PrewarmTextureEncoder(void* device, int64_t luid, int width,
                             int height, int fps) override {
#if defined(USE_DGJ_HWCODEC)
    if (device == nullptr || width <= 0 || height <= 0 || fps <= 0) {
      return false;
    }
    const int kbs = width >= 1920 && height >= 1080 ? 24000 : 12000;
    return dgj_venc_prewarm(luid, device, DGJ_CODEC_H264, width, height, kbs,
                            fps, /*gop=*/0) == DGJ_OK;
#else
    return false;
#endif
  }

  void ClearTextureEncoderPrewarm() override {
#if defined(USE_DGJ_HWCODEC)
    dgj_venc_clear_prewarm();
#endif
  }

  bool StartTestPattern() override {
    return external_capturer_ != nullptr &&
           external_capturer_->StartTestPattern();
  }

  void StopTestPattern() override {
    if (external_capturer_ != nullptr) external_capturer_->StopTestPattern();
  }

  bool IsScreencast() const override { return true; }

 private:
  std::shared_ptr<webrtc::internal::ExternalCapturer> external_capturer_;
};

class RTCVideoDeviceImpl : public RTCVideoDevice {
 public:
  RTCVideoDeviceImpl(rtc::Thread* worker_thread);

 public:
  uint32_t NumberOfDevices() override;

  int32_t GetDeviceName(uint32_t deviceNumber, char* deviceNameUTF8,
                        uint32_t deviceNameLength, char* deviceUniqueIdUTF8,
                        uint32_t deviceUniqueIdUTF8Length,
                        char* productUniqueIdUTF8 = 0,
                        uint32_t productUniqueIdUTF8Length = 0) override;

  scoped_refptr<RTCVideoCapturer> Create(const char* name, uint32_t index,
                                         size_t width, size_t height,
                                         size_t target_fps) override;

 private:
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info_;
  rtc::Thread* worker_thread_ = nullptr;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_VIDEO_DEVICE_IMPL_HXX
