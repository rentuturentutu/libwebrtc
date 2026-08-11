/*
 * Copyright 2022 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
#define LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX

#include <atomic>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "common_video/include/video_frame_buffer_pool.h"
#include "include/rtc_desktop_capturer.h"
#include "include/rtc_types.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/thread.h"
#include "src/internal/vcm_capturer.h"
#include "src/internal/video_capturer.h"

namespace libwebrtc {

class RTCDesktopCapturerImpl : public RTCDesktopCapturer,
                               public webrtc::DesktopCapturer::Callback,
                               public webrtc::internal::VideoCapturer {
 public:
  RTCDesktopCapturerImpl(DesktopType type,
                         webrtc::DesktopCapturer::SourceId source_id,
                         rtc::Thread* signaling_thread,
                         scoped_refptr<MediaSource> source);
  ~RTCDesktopCapturerImpl();

  void RegisterDesktopCapturerObserver(
      DesktopCapturerObserver* observer) override {
    observer_ = observer;
  }

  void DeRegisterDesktopCapturerObserver() override { observer_ = nullptr; }
  CaptureState Start(uint32_t fps) override;

  CaptureState Start(uint32_t fps, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h) override;

  void Stop() override;

  bool IsRunning() override;

  // rtc::VideoSourceInterface override. Invoked by the encoder when it needs a
  // fresh frame, e.g. after a key frame request or once frames were dropped.
  // webrtc::VideoTrackSource does not forward this to source(), so
  // ScreenCapturerTrackSource plumbs it here explicitly.
  void RequestRefreshFrame() override;

  scoped_refptr<MediaSource> source() override { return source_; }

 protected:
  virtual void OnCaptureResult(
      webrtc::DesktopCapturer::Result result,
      std::unique_ptr<webrtc::DesktopFrame> frame) override;

 private:
  void CaptureFrame();
  // Captures exactly one frame without touching the periodic capture schedule.
  // Reusing CaptureFrame() here would post a second recurring task.
  void CaptureFrameOnce();
  void MaybeLogCaptureStats();

  // Capture-thread latency and dirty-region diagnostics. CaptureFrame(),
  // CaptureFrameOnce() and OnCaptureResult() all run on `thread_`, so these
  // fields need no synchronization.
  struct CaptureStats {
    int64_t window_start_ms = 0;
    int64_t last_tick_us = 0;
    uint32_t delivered = 0;
    uint32_t skipped = 0;
    uint32_t forced = 0;
    uint32_t errors = 0;
    uint32_t dirty_rects = 0;
    int64_t dirty_pixels = 0;
    int64_t total_pixels = 0;
    int64_t tick_sum_us = 0;
    int64_t tick_max_us = 0;
    uint32_t tick_count = 0;
    int64_t convert_sum_us = 0;
    int64_t convert_max_us = 0;
    int64_t deliver_sum_us = 0;
    int64_t deliver_max_us = 0;
    int64_t interval_sum_us = 0;
    int64_t interval_max_us = 0;
    uint32_t interval_count = 0;
    // How late each wake-up was relative to its scheduled slot. Separates
    // "the scheduler is imprecise" from "the capture itself is slow".
    int64_t late_sum_us = 0;
    int64_t late_max_us = 0;

    // Clears the accumulators. `window_start_ms` and `last_tick_us` survive so
    // the next window measures intervals continuously.
    void ResetWindow();
  };
  CaptureStats stats_;

  webrtc::DesktopCaptureOptions options_;
  std::unique_ptr<webrtc::DesktopCapturer> capturer_;
  std::unique_ptr<rtc::Thread> thread_;
  // Hands out a buffer that is not still referenced downstream. OnFrame() only
  // runs the synchronous part of the pipeline; the encoder keeps the frame after
  // it returns, so writing every capture into one reused buffer would corrupt
  // the frame being encoded.
  webrtc::VideoFrameBufferPool buffer_pool_;
  std::atomic<CaptureState> capture_state_{CS_STOPPED};
  DesktopType type_;
  webrtc::DesktopCapturer::SourceId source_id_;
  DesktopCapturerObserver* observer_ = nullptr;
  uint32_t capture_delay_ = 1000;  // 1s
  // Capture period and the absolute timestamp of the next scheduled capture.
  // Scheduling on an absolute grid keeps the cadence from drifting by however
  // long each capture takes; `capture_delay_` is left alone because it is
  // pre-existing state that other code may read.
  int64_t capture_period_us_ = 0;
  int64_t next_capture_us_ = 0;
  // Set by RequestRefreshFrame() so the next captured frame bypasses the dirty
  // region early-out. Without this a refresh request would be swallowed while
  // the screen is static, which is exactly when it is needed. Written from the
  // worker thread, consumed on the capture thread.
  std::atomic<bool> force_next_frame_{false};
  webrtc::DesktopCapturer::Result result_ =
      webrtc::DesktopCapturer::Result::SUCCESS;
  rtc::Thread* signaling_thread_ = nullptr;
  scoped_refptr<MediaSource> source_;
  uint32_t x_ = 0;
  uint32_t y_ = 0;
  uint32_t w_ = 0;
  uint32_t h_ = 0;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
