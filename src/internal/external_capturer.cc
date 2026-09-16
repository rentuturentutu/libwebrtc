#include "src/internal/external_capturer.h"
#include "src/internal/capture_i420_buffer.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>


#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv.h"

namespace webrtc {
namespace internal {

std::shared_ptr<ExternalCapturer> ExternalCapturer::Create(size_t width,
                                                           size_t height,
                                                           size_t target_fps) {
  if (width == 0 || height == 0 || target_fps == 0) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": invalid geometry " << width << "x"
                      << height << "@" << target_fps;
    return nullptr;
  }
  return std::make_shared<ExternalCapturer>(width, height, target_fps);
}

ExternalCapturer::ExternalCapturer(size_t width, size_t height,
                                   size_t target_fps)
    : width_(width), height_(height), target_fps_(target_fps) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": ctor " << width << "x" << height << "@"
                   << target_fps;
}

ExternalCapturer::~ExternalCapturer() {
  StopTestPattern();
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": dtor";
}

bool ExternalCapturer::StartCapture() {
  capturing_ = true;
  return true;
}

bool ExternalCapturer::CaptureStarted() {
  return capturing_;
}

void ExternalCapturer::StopCapture() {
  capturing_ = false;
  StopTestPattern();
}

void ExternalCapturer::DeliverI420(rtc::scoped_refptr<I420Buffer> buffer,
                                   int64_t timestamp_us, uint64_t frame_id) {
  OnFrame(VideoFrame::Builder()
              .set_video_frame_buffer(libwebrtc::WithCaptureFrameId(buffer, frame_id))
              .set_rotation(kVideoRotation_0)
              .set_timestamp_us(timestamp_us > 0 ? timestamp_us
                                                 : rtc::TimeMicros())
              .set_id(static_cast<uint16_t>(frame_id))
              .build());
}

bool ExternalCapturer::PushFrameBGRA(const uint8_t* data, int stride, int width,
                                     int height, int64_t timestamp_us) {
  return PushFrameBGRAWithId(data, stride, width, height, timestamp_us, 0);
}

bool ExternalCapturer::PushFrameBGRAWithId(const uint8_t* data, int stride,
                                           int width, int height,
                                           int64_t timestamp_us,
                                           uint64_t frame_id) {
  if (!capturing_ || data == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const int src_stride = stride > 0 ? stride : width * 4;
  if (src_stride < width * 4) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": stride " << src_stride
                      << " too small for width " << width;
    return false;
  }

  // This conversion was the one full-frame CPU operation in the send path that
  // had never been measured: the encoder's own timing log covers i420->nv12 and
  // the encode, but not this. Without the number there is no way to say what the
  // texture path would actually save, since it skips this step entirely.
  const int64_t t0 = rtc::TimeMicros();
  rtc::scoped_refptr<I420Buffer> buffer = I420Buffer::Create(width, height);
  // libyuv's "ARGB" means B,G,R,A in memory order, which is what DXGI /
  // GDI / scrap hand out as BGRA.
  const int result = libyuv::ARGBToI420(
      data, src_stride, buffer->MutableDataY(), buffer->StrideY(),
      buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(),
      buffer->StrideV(), width, height);
  const int64_t elapsed_us = rtc::TimeMicros() - t0;
  if (result != 0) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": ARGBToI420 failed " << result;
    return false;
  }

  // Includes the I420Buffer::Create allocation on purpose: it is per frame and
  // it is part of what this step costs.
  bgra_conv_us_ += elapsed_us;
  bgra_conv_us_max_ = std::max(bgra_conv_us_max_, elapsed_us);
  // 300 frames is ~5s at 60fps, matching the encoder's and the capturer's other
  // periodic logs so the windows line up.
  if (++bgra_conv_frames_ >= 300) {
    RTC_LOG(LS_INFO) << "[dgj-capturer] bgra->i420 over last "
                     << bgra_conv_frames_ << " frames: avg "
                     << (bgra_conv_us_ / 1000.0 / bgra_conv_frames_)
                     << "ms max " << (bgra_conv_us_max_ / 1000.0) << "ms at "
                     << width << "x" << height;
    bgra_conv_frames_ = 0;
    bgra_conv_us_ = 0;
    bgra_conv_us_max_ = 0;
  }

  DeliverI420(buffer, timestamp_us, frame_id);
  return true;
}

bool ExternalCapturer::PushFrameI420(const uint8_t* data_y, int stride_y,
                                     const uint8_t* data_u, int stride_u,
                                     const uint8_t* data_v, int stride_v,
                                     int width, int height,
                                     int64_t timestamp_us) {
  if (!capturing_ || data_y == nullptr || data_u == nullptr ||
      data_v == nullptr || width <= 0 || height <= 0) {
    return false;
  }

  rtc::scoped_refptr<I420Buffer> buffer = I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_u, data_v, stride_v);
  if (buffer == nullptr) {
    return false;
  }

  DeliverI420(buffer, timestamp_us, 0);
  return true;
}

#if defined(USE_DGJ_HWCODEC)
bool ExternalCapturer::PushFrameTexture(void* device, void* texture,
                                       void* shared_handle, int64_t luid,
                                       int width, int height,
                                       int64_t timestamp_us, void* lease,
                                       void (*release)(void*)) {
  return PushFrameTextureWithId(device, texture, shared_handle, luid, width,
                                height, timestamp_us, lease, release, 0);
}

bool ExternalCapturer::PushFrameTextureWithId(
    void* device, void* texture, void* shared_handle, int64_t luid, int width,
    int height, int64_t timestamp_us, void* lease, void (*release)(void*),
    uint64_t frame_id) {
  if (!capturing_ || device == nullptr || texture == nullptr || width <= 0 ||
      height <= 0 || lease == nullptr || release == nullptr) {
    return false;
  }

  // The pool is bound to one device. A device change means the capture session
  // was rebuilt on another adapter (monitor moved, GPU switched), so start over
  // rather than copy across devices, which CopyResource cannot do.
  if (tex_pool_ && tex_pool_->luid() != luid) {
    RTC_LOG(LS_INFO) << __FUNCTION__ << ": adapter changed "
                     << tex_pool_->luid() << " -> " << luid
                     << ", rebuilding texture pool";
    tex_pool_ = nullptr;
  }
  if (!tex_pool_) {
    tex_pool_ = libwebrtc::DgjTexturePool::Create(device, luid);
    if (!tex_pool_) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << ": DgjTexturePool::Create failed";
      return false;
    }
  }

  rtc::scoped_refptr<libwebrtc::DgjD3D11TextureBuffer> buffer =
      tex_pool_->Wrap(texture, shared_handle, width, height, lease, release);
  if (!buffer) {
    return false;
  }

  buffer->SetCaptureFrameId(frame_id);
  OnFrame(VideoFrame::Builder()
              .set_video_frame_buffer(buffer)
              .set_rotation(kVideoRotation_0)
              .set_timestamp_us(timestamp_us > 0 ? timestamp_us
                                                 : rtc::TimeMicros())
              .set_id(static_cast<uint16_t>(frame_id & 0xffffu))
              .build());

  // Every 300 frames, matching the other periodic logs.
  if (++tex_pushed_ % 300 == 0) {
    RTC_LOG(LS_INFO) << "[dgj-direct-texture] pushed=" << tex_pushed_;
  }
  return true;
}
#endif  // USE_DGJ_HWCODEC

bool ExternalCapturer::StartTestPattern() {
  if (test_pattern_running_) {
    return true;
  }
  capturing_ = true;
  test_pattern_running_ = true;
  test_pattern_thread_ =
      std::make_unique<std::thread>([this] { TestPatternLoop(); });
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": test pattern started";
  return true;
}

void ExternalCapturer::StopTestPattern() {
  if (!test_pattern_running_) {
    return;
  }
  test_pattern_running_ = false;
  if (test_pattern_thread_ && test_pattern_thread_->joinable()) {
    test_pattern_thread_->join();
  }
  test_pattern_thread_.reset();
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": test pattern stopped";
}

void ExternalCapturer::SetMaxFramerateObserver(void (*observer)(int, void*),
                                               void* user) {
  std::lock_guard<std::mutex> guard(fps_observer_lock_);
  fps_observer_ = observer;
  fps_observer_user_ = user;
  // Reset so the next decision is always delivered, even if it equals whatever
  // a previous observer last saw.
  last_reported_fps_ = 0;
}

void ExternalCapturer::OnMaxFramerateChanged(int fps) {
  // Nothing constrains the rate: leave the grabber at whatever the host asked
  // for. Reported as -1 so the host can tell "no limit" from a real number.
  const bool unlimited = fps >= std::numeric_limits<int>::max();
  int reported = unlimited ? -1 : fps;

  if (!unlimited) {
    // The encoder can ask for absurdly low rates while it is settling; below
    // this a remote desktop is unusable, and the adapter can still drop the
    // difference.
    constexpr int kMinFps = 10;
    reported = std::max(reported, kMinFps);
  }

  {
    std::lock_guard<std::mutex> guard(fps_observer_lock_);
    if (!fps_observer_) {
      return;
    }
    // Hysteresis: UpdateVideoAdapter runs on every AddOrUpdateSink, and the
    // encoder nudges its restrictions far more often than the grabber needs to
    // know. Only cross the FFI boundary when the decision moved meaningfully.
    constexpr int kMinDeltaFps = 5;
    if (last_reported_fps_ != 0 &&
        std::abs(reported - last_reported_fps_) < kMinDeltaFps) {
      return;
    }
    last_reported_fps_ = reported;
    RTC_LOG(LS_INFO) << "[dgj-capturer] max framerate -> "
                     << (reported < 0 ? "unlimited" : std::to_string(reported));
    // Called under the lock on purpose: it guarantees the observer is not torn
    // down mid-call. The host side is a short atomic store.
    fps_observer_(reported, fps_observer_user_);
  }
}

void ExternalCapturer::TestPatternLoop() {
  const int width = static_cast<int>(width_);
  const int height = static_cast<int>(height_);
  const int stride = width * 4;
  const int64_t frame_interval_us = 1000000 / static_cast<int64_t>(target_fps_);

  std::vector<uint8_t> bgra(static_cast<size_t>(stride) * height);
  int64_t frame_index = 0;

  // Eight static colour bars plus a square that walks left to right, so both
  // "is there a picture" and "is it actually updating" are visible at a glance.
  const uint8_t bars[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0},
                              {0, 255, 0},     {255, 0, 255}, {0, 0, 255},
                              {255, 0, 0},     {0, 0, 0}};

  while (test_pattern_running_) {
    const int64_t start_us = rtc::TimeMicros();

    const int bar_width = std::max(1, width / 8);
    for (int y = 0; y < height; ++y) {
      uint8_t* row = bgra.data() + static_cast<size_t>(y) * stride;
      for (int x = 0; x < width; ++x) {
        const int bar = std::min(7, x / bar_width);
        row[x * 4 + 0] = bars[bar][0];
        row[x * 4 + 1] = bars[bar][1];
        row[x * 4 + 2] = bars[bar][2];
        row[x * 4 + 3] = 255;
      }
    }

    const int box = std::max(16, height / 8);
    const int span = std::max(1, width - box);
    const int box_x = static_cast<int>((frame_index * 8) % span);
    const int box_y = (height - box) / 2;
    for (int y = box_y; y < box_y + box && y < height; ++y) {
      uint8_t* row = bgra.data() + static_cast<size_t>(y) * stride;
      for (int x = box_x; x < box_x + box && x < width; ++x) {
        row[x * 4 + 0] = 0;
        row[x * 4 + 1] = 128;
        row[x * 4 + 2] = 255;
        row[x * 4 + 3] = 255;
      }
    }

    PushFrameBGRA(bgra.data(), stride, width, height, rtc::TimeMicros());
    ++frame_index;

    const int64_t elapsed_us = rtc::TimeMicros() - start_us;
    const int64_t sleep_us = frame_interval_us - elapsed_us;
    if (sleep_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
  }
}

}  // namespace internal
}  // namespace webrtc
