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

#include "rtc_desktop_capturer_impl.h"

#include <algorithm>

#include "api/sequence_checker.h"
#include "modules/desktop_capture/desktop_region.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv.h"
#ifdef WEBRTC_WIN
#include "modules/desktop_capture/win/window_capture_utils.h"
#endif

namespace libwebrtc {

enum { kCaptureDelay = 33, kCaptureMessageId = 1000 };

namespace {
constexpr int64_t kCaptureStatsLogIntervalMs = 5000;
}  // namespace

void RTCDesktopCapturerImpl::CaptureStats::ResetWindow() {
  delivered = 0;
  skipped = 0;
  forced = 0;
  errors = 0;
  dirty_rects = 0;
  dirty_pixels = 0;
  total_pixels = 0;
  tick_sum_us = 0;
  tick_max_us = 0;
  tick_count = 0;
  convert_sum_us = 0;
  convert_max_us = 0;
  deliver_sum_us = 0;
  deliver_max_us = 0;
  interval_sum_us = 0;
  interval_max_us = 0;
  interval_count = 0;
  late_sum_us = 0;
  late_max_us = 0;
}

void RTCDesktopCapturerImpl::MaybeLogCaptureStats() {
  const int64_t now_ms = rtc::TimeMillis();
  if (stats_.window_start_ms == 0) {
    stats_.window_start_ms = now_ms;
    return;
  }
  const int64_t window_ms = now_ms - stats_.window_start_ms;
  if (window_ms < kCaptureStatsLogIntervalMs) {
    return;
  }

  const uint32_t total = stats_.delivered + stats_.skipped + stats_.errors;
  // The tick covers acquire + convert + deliver, so the acquire cost is what is
  // left after subtracting the two measured sub-steps. Clamp: an exception
  // caught by __except can leave the sub-step sums short of the tick sum.
  const int64_t acquire_sum_us = std::max(
      int64_t{0},
      stats_.tick_sum_us - stats_.convert_sum_us - stats_.deliver_sum_us);

  RTC_LOG(LS_INFO) << "DesktopCapture stats:"
                   << " delivered=" << stats_.delivered
                   << " skipped=" << stats_.skipped
                   << " forced=" << stats_.forced
                   << " errors=" << stats_.errors << " total=" << total
                   << " skip_percent="
                   << (total ? stats_.skipped * 100 / total : 0)
                   << " avg_rects="
                   << (stats_.delivered ? stats_.dirty_rects / stats_.delivered
                                        : 0)
                   << " dirty_percent="
                   << (stats_.total_pixels
                           ? stats_.dirty_pixels * 100 / stats_.total_pixels
                           : 0)
                   << " interval_ms avg="
                   << (stats_.interval_count
                           ? stats_.interval_sum_us / stats_.interval_count /
                                 1000
                           : 0)
                   << " max=" << stats_.interval_max_us / 1000
                   << " late_us avg="
                   << (stats_.interval_count
                           ? stats_.late_sum_us / stats_.interval_count
                           : 0)
                   << " max=" << stats_.late_max_us
                   << " tick_us avg="
                   << (stats_.tick_count ? stats_.tick_sum_us / stats_.tick_count
                                         : 0)
                   << " max=" << stats_.tick_max_us << " acquire_us avg="
                   << (stats_.tick_count ? acquire_sum_us / stats_.tick_count
                                         : 0)
                   << " convert_us avg="
                   << (stats_.delivered
                           ? stats_.convert_sum_us / stats_.delivered
                           : 0)
                   << " max=" << stats_.convert_max_us << " deliver_us avg="
                   << (stats_.delivered
                           ? stats_.deliver_sum_us / stats_.delivered
                           : 0)
                   << " max=" << stats_.deliver_max_us
                   << " window_ms=" << window_ms;

  stats_.window_start_ms = now_ms;
  stats_.ResetWindow();
}

RTCDesktopCapturerImpl::RTCDesktopCapturerImpl(
    DesktopType type, webrtc::DesktopCapturer::SourceId source_id,
    rtc::Thread* signaling_thread, scoped_refptr<MediaSource> source)
    : thread_(rtc::Thread::Create()),
      source_id_(source_id),
      signaling_thread_(signaling_thread),
      source_(source) {
  RTC_DCHECK(thread_);
  type_ = type;
  thread_->Start();
  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
#ifdef WEBRTC_WIN
  options_.set_allow_directx_capturer(true);
#endif
#ifdef WEBRTC_LINUX
  if (type == kScreen) {
    options_.set_allow_pipewire(true);
  }
#endif
  thread_->BlockingCall([this, type] {
    if (type == kScreen) {
      // capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
      //     webrtc::DesktopCapturer::CreateScreenCapturer(options_), options_);
      capturer_ = webrtc::DesktopAndCursorComposer::CreateWithoutMouseCursorMonitor(webrtc::DesktopCapturer::CreateScreenCapturer(options_));
    } else {
      capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
          webrtc::DesktopCapturer::CreateWindowCapturer(options_), options_);
    }
  });
}

RTCDesktopCapturerImpl::~RTCDesktopCapturerImpl() {
  thread_->Stop();
  capturer_.reset();
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  if (!w_ || !h) {
    x_ = 0;
    y_ = 0;
  }
  return Start(fps);
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps) {
  if (capture_state_ == CS_RUNNING) {
    return capture_state_;
  }

  if (fps == 0) {
    capture_state_ = CS_FAILED;
    return capture_state_;
  }

  if (fps >= 60) {
    capture_delay_ = uint32_t(1000.0 / 60.0);
  } else {
    capture_delay_ = uint32_t(1000.0 / fps);
  }
  // Keep the scheduling period in microseconds: 1000 / fps truncates (30fps
  // becomes 33ms instead of 33.33ms, 60fps becomes 16ms instead of 16.67ms).
  capture_period_us_ = 1000000 / std::min(fps, 60u);

  if (source_id_ != -1) {
    if (!capturer_->SelectSource(source_id_)) {
      capture_state_ = CS_FAILED;
      return capture_state_;
    }
    if (type_ == kWindow) {
      if (!capturer_->FocusOnSelectedSource()) {
        capture_state_ = CS_FAILED;
        return capture_state_;
      }
    }
  }

  thread_->BlockingCall([this] { capturer_->Start(this); });

  // Drop buffers from a previous Start() or SelectSource() target. The first
  // frame of this session allocates one matching the new geometry.
  buffer_pool_.Release();
  force_next_frame_ = false;
  // Start from a clean window so the first log after a restart does not mix in
  // counters from the previous session or report an inflated capture interval.
  stats_ = CaptureStats();
  next_capture_us_ = rtc::TimeMicros();

  capture_state_ = CS_RUNNING;

  // No source constraints are reported, so FrameCadenceAdapterImpl stays on
  // PassthroughAdapterMode instead of entering zero-hertz mode. Zero-hertz held
  // every frame for one frame_delay_ (~33ms at 30fps), which was 73% of the
  // measured 45ms capture-to-send delay. Resolution is now protected by the
  // MAINTAIN_RESOLUTION degradation preference in RTCRtpSenderImpl rather than
  // by zero-hertz reporting a constant fake frame rate.

  thread_->PostTask([this] { CaptureFrame(); });
  if (observer_) {
    signaling_thread_->BlockingCall([&, this]() { observer_->OnStart(this); });
  }
  return capture_state_;
}

void RTCDesktopCapturerImpl::Stop() {
  if (observer_) {
    if (!signaling_thread_->IsCurrent()) {
      signaling_thread_->BlockingCall([&, this]() { observer_->OnStop(this); });
    } else {
      observer_->OnStop(this);
    }
  }
  capture_state_ = CS_STOPPED;
}

bool RTCDesktopCapturerImpl::IsRunning() {
  return capture_state_ == CS_RUNNING;
}

#ifdef WEBRTC_WIN
int filterException(int code, PEXCEPTION_POINTERS ex) {
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void RTCDesktopCapturerImpl::OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  if (result != result_) {
    if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnError(this); });
      }
      capture_state_ = CS_FAILED;
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
      result_ = result;
      ++stats_.errors;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnPaused(this); });
      }
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::SUCCESS) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnStart(this); });
      }
    }
  }

  if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
    ++stats_.errors;
    MaybeLogCaptureStats();
    return;
  }

  int width = frame->size().width();
  int height = frame->size().height();
#ifdef WEBRTC_WIN
  webrtc::DesktopRect rect_ = webrtc::DesktopRect::MakeWH(width, height);

  if (type_ != kScreen) {
    webrtc::GetWindowRect(reinterpret_cast<HWND>(source_id_), &rect_);
  }

  __try
#endif
  {
    width = w_ > 0 ? w_ : width;
    height = h_ > 0 ? h_ : height;

    // The pool never hands back a buffer that is still referenced downstream,
    // and it purges buffers whose geometry no longer matches, so no explicit
    // dimension check is needed here.
    rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer =
        buffer_pool_.CreateI420Buffer(width, height);
    if (!i420_buffer) {
      // Downstream is holding more frames than the pool allows. Dropping this
      // capture is better than overwriting a frame that is still being encoded.
      ++stats_.errors;
      MaybeLogCaptureStats();
      return;
    }

    // Always deliver successful captures. Skipping unchanged frames made the
    // encoder input rate swing between 2fps and 30fps, which caused visible
    // stutter at a standard WebRTC receiver. Screen-content rate control already
    // encodes unchanged macroblocks efficiently, so a stable cadence is more
    // valuable than the small bandwidth saving.
    if (force_next_frame_.exchange(false)) {
      ++stats_.forced;
    }
    ++stats_.delivered;
    for (webrtc::DesktopRegion::Iterator it(frame->updated_region());
         !it.IsAtEnd(); it.Advance()) {
      ++stats_.dirty_rects;
      stats_.dirty_pixels +=
          static_cast<int64_t>(it.rect().width()) * it.rect().height();
    }
    stats_.total_pixels += static_cast<int64_t>(width) * height;

    const int64_t convert_start_us = rtc::TimeMicros();
    libyuv::ConvertToI420(frame->data(), 0, i420_buffer->MutableDataY(),
                          i420_buffer->StrideY(), i420_buffer->MutableDataU(),
                          i420_buffer->StrideU(), i420_buffer->MutableDataV(),
                          i420_buffer->StrideV(), x_, y_,
#ifdef WEBRTC_WIN
                          rect_.width(), rect_.height(),
#else
                          width, height,
#endif
                          width, height, libyuv::kRotate0, libyuv::FOURCC_ARGB);
    const int64_t convert_us = rtc::TimeMicros() - convert_start_us;
    stats_.convert_sum_us += convert_us;
    stats_.convert_max_us = std::max(stats_.convert_max_us, convert_us);

    // Microsecond precision matters here: the refresh frame and a periodic
    // capture can land in the same millisecond, and equal timestamps trip the
    // monotonicity check in FrameCadenceAdapterImpl::OnFrameOnMainQueue().
    const int64_t deliver_start_us = rtc::TimeMicros();
    OnFrame(webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(i420_buffer)
                .set_rotation(webrtc::kVideoRotation_0)
                .set_timestamp_us(rtc::TimeMicros())
                .build());
    // Covers the synchronous part of VideoAdapter -> VideoBroadcaster ->
    // VideoStreamEncoder::OnFrame, i.e. how much the capture thread is being
    // held up by downstream work.
    const int64_t deliver_us = rtc::TimeMicros() - deliver_start_us;
    stats_.deliver_sum_us += deliver_us;
    stats_.deliver_max_us = std::max(stats_.deliver_max_us, deliver_us);

    MaybeLogCaptureStats();
  }
#ifdef WEBRTC_WIN
  __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
  }
#endif
}

void RTCDesktopCapturerImpl::CaptureFrame() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (capture_state_ == CS_RUNNING) {
    const int64_t tick_start_us = rtc::TimeMicros();
    if (stats_.last_tick_us != 0) {
      const int64_t interval_us = tick_start_us - stats_.last_tick_us;
      stats_.interval_sum_us += interval_us;
      stats_.interval_max_us = std::max(stats_.interval_max_us, interval_us);
      ++stats_.interval_count;
      // Distance from the slot this wake-up was scheduled for. A large value
      // means the task queue, not the capture, is the problem.
      const int64_t late_us = tick_start_us - next_capture_us_;
      if (late_us > 0) {
        stats_.late_sum_us += late_us;
        stats_.late_max_us = std::max(stats_.late_max_us, late_us);
      }
    }
    stats_.last_tick_us = tick_start_us;

    // OnCaptureResult() is invoked synchronously from inside CaptureFrame(), so
    // the tick spans acquire + convert + deliver. The acquire cost is derived at
    // log time by subtracting the convert and deliver sums.
    capturer_->CaptureFrame();

    const int64_t tick_us = rtc::TimeMicros() - tick_start_us;
    stats_.tick_sum_us += tick_us;
    stats_.tick_max_us = std::max(stats_.tick_max_us, tick_us);
    ++stats_.tick_count;

    // Schedule on an absolute grid: wait only for what is left until the next
    // slot instead of tacking a fixed period onto the end of this capture.
    // Measured before this change, the effective period was period + tick:
    // 34ms while idle and 39ms while the screen was changing -- the frame rate
    // sagged exactly when smoothness mattered most.
    next_capture_us_ += capture_period_us_;
    const int64_t now_us = rtc::TimeMicros();
    int64_t delay_us = next_capture_us_ - now_us;
    if (delay_us < 0) {
      // More than a full period behind (slow acquire, thread stall). Re-anchor
      // rather than firing a catch-up burst, which would only make the cadence
      // less even.
      next_capture_us_ = now_us + capture_period_us_;
      delay_us = capture_period_us_;
    }
    thread_->PostDelayedHighPrecisionTask([this]() { CaptureFrame(); },
                                          webrtc::TimeDelta::Micros(delay_us));
  }
}

void RTCDesktopCapturerImpl::RequestRefreshFrame() {
  // The state is deliberately not checked here: it is owned by the capture
  // thread, and CaptureFrameOnce() re-checks it there.
  // Preserve refresh-request observability even though every successful capture
  // is now delivered.
  force_next_frame_ = true;
  thread_->PostTask([this] { CaptureFrameOnce(); });
}

void RTCDesktopCapturerImpl::CaptureFrameOnce() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (capture_state_ == CS_RUNNING) {
    // Timed like a periodic tick, but deliberately kept out of the interval
    // statistics: a refresh is an extra trigger and would skew the polling
    // interval average.
    const int64_t tick_start_us = rtc::TimeMicros();
    capturer_->CaptureFrame();
    const int64_t tick_us = rtc::TimeMicros() - tick_start_us;
    stats_.tick_sum_us += tick_us;
    stats_.tick_max_us = std::max(stats_.tick_max_us, tick_us);
    ++stats_.tick_count;

    // The periodic grid is deliberately left alone. Pushing it out to avoid a
    // back-to-back frame would instead open a gap of up to 1.5 periods before
    // the next scheduled capture, and a 50ms hole hurts smoothness far more
    // than one extra frame does.
  }
}

}  // namespace libwebrtc
