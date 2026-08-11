#include "libwebrtc/src/dgj/hwcodec_video_encoder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "api/video/i420_buffer.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "dgj_encode_ffi.h"
#include "src/dgj/dgj_texture_buffer.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace libwebrtc {

namespace {

// Match UU's recovered minimum while still suppressing a PLI storm. RAM and
// NVIDIA texture encoders now force an IDR without rebuilding the session.
constexpr int64_t kMinForceKeyIntervalMs = 600;

constexpr int64_t kRamDrainPollMs = 1;
constexpr int64_t kRamDrainBudgetUs = 20000;

constexpr int64_t kVramDrainPollUs = 1000;
// nvEncReconfigureEncoder serializes with pending output on this driver. Even
// rate-limited updates still correlated with 100-400ms completion stalls in a
// clean-LAN trace, so the texture encoder holds one quality tier for its whole
// session. Its private encoder can reconfigure without exposing that
// serialization to the media path; ours currently cannot.
constexpr int64_t kVramRateHoldLogMs = 5000;
constexpr int kVramBitrateChangePercent = 20;
// Keep a fixed 1080p floor while GCC probes. That keeps
// dark borders and moving text from being quantized away while GCC is still
// probing the route. Keep the floor configurable for constrained links and
// A/B runs.
int EnvInt(const char* name, int default_value, int minimum, int maximum) {
  char value[32] = {};
  size_t length = 0;
  if (getenv_s(&length, value, sizeof(value), name) != 0 || length == 0) {
    return default_value;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0') {
    return default_value;
  }
  return static_cast<int>(std::clamp(parsed, static_cast<long>(minimum),
                                     static_cast<long>(maximum)));
}

int Vram1080pBitrateKbps(webrtc::VideoCodecType codec_type) {
  // Round 13 kept the transport queue empty at 20 Mbps, but H.264 still
  // reached QP 24-26 on window motion. H.264 needs more bits than UU's H.265
  // path for comparable detail, so spend the measured direct-UDP headroom on
  // a 24 Mbps floor. Keep the unverified H.265 path at its existing 20 Mbps.
  const int default_kbps =
      codec_type == webrtc::kVideoCodecH264 ? 24000 : 20000;
  return EnvInt("DGJ_VRAM_MIN_KBPS", default_kbps, 6000, 38000);
}

int StableVramBitrateKbps(int width,
                          int height,
                          int requested_kbps,
                          webrtc::VideoCodecType codec_type) {
  if (width >= 1920 && height >= 1080) {
    return std::max(requested_kbps, Vram1080pBitrateKbps(codec_type));
  }
  return requested_kbps;
}

int64_t VramDrainDelayUs(int fps) {
  // Native NVENC completion runs on its own thread. This poll only transfers
  // ready packets back onto WebRTC's encoder worker, so it is nonblocking.
  (void)fps;
  return kVramDrainPollUs;
}

// About 10s at 30fps. Often enough to see a trend, rare enough not to flood the
// log or perturb what it is measuring.
constexpr int kTimingLogFrames = 300;

// QSV depth is capped at four. A little extra headroom makes this robust to a
// codec returning several packets on one drain without retaining old screens
// indefinitely if a driver drops a packet.
constexpr size_t kMaxEncodedContexts = 8;

// Between two RAM-encoder rebuilds. Same 86ms cost as a forced keyframe (it is
// the same operation) plus an IDR plus a CBR rate-controller reset, and SetRates
// can fire every frame. 2s was tried first and produced 14 rebuilds in 40s --
// the bitrate then oscillated 1341..4468kbps because the controller was reset
// before it could converge. 8s is long enough for it to settle between changes.
constexpr int64_t kMinRamRebuildIntervalMs = 8000;

// How far the live bitrate must drift from what the encoder was opened with
// before paying for a rebuild. 40% because GCC walks continuously and its normal
// band was measured at 5.1..6.0Mbps, so a tighter cut just rebuilds on the walk.
constexpr int kRamRebuildKbsPercent = 40;


// Clamp to a sane minimum; 0 kbps makes the rate controller misbehave.
int32_t ToFfiBitrate(int kbps) {
  return kbps < 1 ? 1 : kbps;
}

// gop = fps*3: periodic IDR as a safety net for keyframe requests that the 1s
// throttle rejects. Floor of 30 keeps a very low fps from pushing the IDR
// interval out to minutes.
int32_t GopFor(int fps) {
  return std::max(fps * 3, 30);
}

}  // namespace


HwcodecVideoEncoder::HwcodecVideoEncoder(std::string codec_name,
                                         webrtc::VideoCodecType codec_type)
    : codec_name_(std::move(codec_name)), codec_type_(codec_type) {}

HwcodecVideoEncoder::~HwcodecVideoEncoder() {
  Release();
}

int HwcodecVideoEncoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                                    const VideoEncoder::Settings& settings) {
  if (!codec_settings || codec_settings->codecType != codec_type_ ||
      (codec_type_ != webrtc::kVideoCodecH264 &&
       codec_type_ != webrtc::kVideoCodecH265)) {
    RTC_LOG(LS_ERROR) << "[dgj-hwcodec] InitEncode codec mismatch";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (codec_settings->width < 1 || codec_settings->height < 1 ||
      codec_settings->maxFramerate < 1) {
    RTC_LOG(LS_ERROR) << "[dgj-hwcodec] Invalid dimensions/fps: "
                      << codec_settings->width << "x"
                      << codec_settings->height << "@"
                      << codec_settings->maxFramerate;
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  int32_t release_ret = Release();
  if (release_ret != WEBRTC_VIDEO_CODEC_OK) {
    return release_ret;
  }

  width_ = codec_settings->width;
  height_ = codec_settings->height;
  fps_ = codec_settings->maxFramerate;
  const int requested_start_kbps =
      ToFfiBitrate(codec_settings->startBitrate);
  const int quality_floor_kbps = width_ >= 1920 && height_ >= 1080
                                     ? Vram1080pBitrateKbps(codec_type_)
                                     : requested_start_kbps;
  kbs_ = StableVramBitrateKbps(width_, height_, requested_start_kbps,
                                codec_type_);
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] fixed rate policy codec="
                   << (codec_type_ == webrtc::kVideoCodecH264 ? "H264" : "H265")
                   << " requested=" << requested_start_kbps
                   << " floor=" << quality_floor_kbps << " configured=" << kbs_
                   << " kbps runtime_reconfigure=disabled";

  // gop = fps*3: periodic IDR as a safety net for keyframe requests that the
  // 1s throttle rejects (see hwcodec_video_encoder.h). The receiver PLI path
  // still produces an IDR immediately via the rebuild in dgj_enc_encode.
  handle_ = dgj_enc_create(codec_name_.c_str(), width_, height_, kbs_, fps_,
                           GopFor(fps_));
  if (!handle_) {
    RTC_LOG(LS_WARNING)
        << "[dgj-hwcodec] dgj_enc_create(" << codec_name_ << ") failed; "
        << "software fallback will take over";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  worker_error_.store(WEBRTC_VIDEO_CODEC_OK, std::memory_order_release);
  initialized_.store(true, std::memory_order_release);
  StartWorker();

  RTC_LOG(LS_INFO) << "[dgj-hwcodec] encoder started: " << codec_name_ << " "
                   << width_ << "x" << height_ << "@" << fps_ << "fps, "
                   << kbs_ << " kbps";
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwcodecVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_.store(callback, std::memory_order_release);
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwcodecVideoEncoder::Release() {
  callback_.store(nullptr, std::memory_order_release);
  StopWorker();
  encoded_contexts_.clear();
  if (handle_) {
    dgj_enc_destroy(handle_);
    handle_ = nullptr;
  }
  // `vram_unavailable_` is deliberately NOT cleared: it records a property of the
  // adapter, not of this encoder instance.
  ReleaseVramEncoder();
  return WEBRTC_VIDEO_CODEC_OK;
}

void HwcodecVideoEncoder::StartWorker() {
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_stop_ = false;
    pending_frame_.reset();
    pending_rates_ = false;
    ram_drain_until_us_ = 0;
    vram_drain_at_us_ = 0;
    async_submitted_ = 0;
    async_processed_ = 0;
    async_replaced_ = 0;
    async_pending_age_max_us_ = 0;
    vram_busy_drops_ = 0;
  }
  worker_ = std::thread(&HwcodecVideoEncoder::WorkerLoop, this);
}

void HwcodecVideoEncoder::StopWorker() {
  initialized_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_stop_ = true;
    pending_frame_.reset();
    pending_rates_ = false;
  }
  worker_cv_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void HwcodecVideoEncoder::WorkerLoop() {
  for (;;) {
    std::optional<PendingFrame> frame;
    bool apply_rates = false;
    bool drain_ram = false;
    bool drain_vram = false;
    int new_fps = -1;
    int new_kbs = 0;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      const auto has_work = [this] {
        return worker_stop_ || pending_frame_.has_value() || pending_rates_;
      };
      if (!has_work() && vhandle_ != nullptr && !encoded_contexts_.empty() &&
          vram_drain_at_us_ > 0) {
        const int64_t remaining_us = vram_drain_at_us_ - rtc::TimeMicros();
        if (remaining_us > 0) {
          worker_cv_.wait_for(lock, std::chrono::microseconds(remaining_us),
                              has_work);
        }
        drain_vram = !has_work();
      } else if (!has_work() && handle_ != nullptr &&
                 !encoded_contexts_.empty() &&
                 rtc::TimeMicros() < ram_drain_until_us_) {
        worker_cv_.wait_for(lock, std::chrono::milliseconds(kRamDrainPollMs),
                            has_work);
        drain_ram = !has_work();
      } else {
        worker_cv_.wait(lock, has_work);
      }
      if (worker_stop_) {
        return;
      }
      if (pending_rates_) {
        apply_rates = true;
        new_fps = pending_fps_;
        new_kbs = pending_kbs_;
        pending_rates_ = false;
      }
      if (pending_frame_) {
        frame = std::move(pending_frame_);
        pending_frame_.reset();
      }
    }

    if (apply_rates) {
      ApplyRatesOnWorker(new_fps, new_kbs);
    }
    if (drain_ram) {
      const int ret =
          dgj_enc_drain(handle_, &HwcodecVideoEncoder::OnEncoded, this);
      if (ret != DGJ_OK) {
        RTC_LOG(LS_ERROR) << "[dgj-hwcodec] dgj_enc_drain failed with " << ret;
        RecordWorkerResult(WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
      }
      continue;
    }
    if (drain_vram) {
      vram_drain_at_us_ = 0;
      const int ret =
          dgj_venc_drain(vhandle_, &HwcodecVideoEncoder::OnEncoded, this);
      if (ret == DGJ_ERR_WOULD_BLOCK) {
        vram_drain_at_us_ = rtc::TimeMicros() + kVramDrainPollUs;
      } else if (ret != DGJ_OK) {
        RTC_LOG(LS_ERROR) << "[dgj-hwcodec] dgj_venc_drain failed with "
                          << ret;
        RecordWorkerResult(WEBRTC_VIDEO_CODEC_ERROR);
      }
      continue;
    }
    if (!frame) {
      continue;
    }

    const int64_t pending_age_us = rtc::TimeMicros() - frame->enqueued_us;
    const int32_t result = EncodeOnWorker(frame->frame, frame->force_key);
    RecordWorkerResult(result);

    bool log_window = false;
    int submitted = 0;
    int processed = 0;
    int replaced = 0;
    int busy_drops = 0;
    int64_t pending_age_max_us = 0;
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      ++async_processed_;
      async_pending_age_max_us_ =
          std::max(async_pending_age_max_us_, pending_age_us);
      if (async_processed_ >= kTraceWindowFrames) {
        log_window = true;
        submitted = async_submitted_;
        processed = async_processed_;
        replaced = async_replaced_;
        busy_drops = vram_busy_drops_;
        pending_age_max_us = async_pending_age_max_us_;
        async_submitted_ = 0;
        async_processed_ = 0;
        async_replaced_ = 0;
        vram_busy_drops_ = 0;
        async_pending_age_max_us_ = 0;
      }
    }
    if (log_window) {
      RTC_LOG(LS_INFO) << "[dgj-async] submitted=" << submitted
                       << " processed=" << processed
                       << " replaced_pending=" << replaced
                       << " vram_busy_drops=" << busy_drops
                       << " pending_age_max_ms="
                       << (pending_age_max_us / 1000.0);
    }
  }
}

void HwcodecVideoEncoder::RecordWorkerResult(int32_t result) {
  if (result == WEBRTC_VIDEO_CODEC_OK) {
    return;
  }
  int32_t expected = WEBRTC_VIDEO_CODEC_OK;
  worker_error_.compare_exchange_strong(expected, result,
                                        std::memory_order_acq_rel);
}

int32_t HwcodecVideoEncoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  const int32_t worker_error =
      worker_error_.exchange(WEBRTC_VIDEO_CODEC_OK, std::memory_order_acq_rel);
  if (worker_error != WEBRTC_VIDEO_CODEC_OK) {
    return worker_error;
  }
  if (!initialized_.load(std::memory_order_acquire)) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!callback_.load(std::memory_order_acquire)) {
    RTC_LOG(LS_WARNING)
        << "[dgj-hwcodec] Encode() called without a registered callback";
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  bool force_key = false;
  if (frame_types && !frame_types->empty() &&
      (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
    int64_t now = rtc::TimeMillis();
    if (now - last_key_ms_ >= kMinForceKeyIntervalMs) {
      force_key = true;
      last_key_ms_ = now;
    }
  }

  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (worker_stop_ || !initialized_.load(std::memory_order_relaxed)) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    ++async_submitted_;
    if (pending_frame_) {
      // A keyframe request must survive replacement: losing it could leave the
      // receiver unable to recover until the next PLI or periodic GOP.
      force_key = force_key || pending_frame_->force_key;
      ++async_replaced_;
    }
    pending_frame_.emplace(PendingFrame{frame, force_key, rtc::TimeMicros()});
  }
  worker_cv_.notify_one();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwcodecVideoEncoder::EncodeOnWorker(
    const webrtc::VideoFrame& frame,
    bool force_key) {
  if (!handle_ && !vhandle_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!callback_.load(std::memory_order_acquire)) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  // How long this frame took to get from the capturer to this worker.
  const int64_t capture_us = frame.timestamp_us();
  const int64_t queue_us =
      capture_us > 0 ? rtc::TimeMicros() - capture_us : 0;
  const int64_t encode_start_us = rtc::TimeMicros();
  // Recorded so a slow frame can say whether it was a keyframe request. The IDR
  // is now requested in place, so it no longer resets QSV/NVENC rate control.

  // Texture path. Taken only when the frame really is one of our D3D11 buffers
  // and its adapter has a hardware encoder; anything else falls through to the
  // I420 path below. Both must stay live -- the software fallback wrapper and the
  // quality scaler hand us I420 frames regardless of what the capturer produces.
  if (frame.video_frame_buffer()->type() ==
      webrtc::VideoFrameBuffer::Type::kNative) {
    auto* tex_buffer =
        static_cast<DgjD3D11TextureBuffer*>(frame.video_frame_buffer().get());
    if (EnsureVramEncoder(tex_buffer->device(), tex_buffer->luid(),
                          tex_buffer->width(), tex_buffer->height())) {
      // The RAM encoder is now dead weight, and not harmlessly so: it holds a
      // session on a *different* adapter's hardware encoder (QSV on the Intel
      // iGPU here, while the texture path lands on NVENC), and SetRates below
      // returns early once vhandle_ exists, so its bitrate is never updated
      // again either. Measured with both open: 50.8ms per vram encode against
      // 10.9ms for the RAM path alone.
      ReleaseRamEncoderForTexturePath();
      RememberFrame(frame, frame.render_time_ms(), queue_us, encode_start_us,
                    force_key);
      const int vret =
          dgj_venc_encode(vhandle_, tex_buffer->texture(),
                          tex_buffer->shared_handle(),
                          frame.render_time_ms(), force_key ? 1 : 0,
                          &HwcodecVideoEncoder::OnEncoded, this);
      if (vret == DGJ_ERR_WOULD_BLOCK) {
        TakeFrame(frame.render_time_ms());
        ++vram_busy_drops_;
        vram_drain_at_us_ = rtc::TimeMicros() + kVramDrainPollUs;
        return WEBRTC_VIDEO_CODEC_OK;
      }
      if (vret != DGJ_OK) {
        TakeFrame(frame.render_time_ms());
        RTC_LOG(LS_ERROR) << "[dgj-hwcodec] dgj_venc_encode failed with " << vret
                          << " on " << tex_buffer->width() << "x"
                          << tex_buffer->height() << " luid "
                          << tex_buffer->luid()
                          << "; dropping the texture path and continuing on "
                             "I420 for this session";
        // Not FALLBACK_SOFTWARE: the RAM path is still perfectly good, and
        // handing the whole session to OpenH264 over a texture problem would be
        // a much bigger regression than one dropped frame.
        //
        // The RAM encoder was released when the texture path took over, so it
        // has to be brought back before falling through to it. A failure here
        // does mean software fallback, since then neither encoder exists.
        ReleaseVramEncoder();
        vram_unavailable_ = true;
        if (handle_ == nullptr && !RebuildRamEncoder(fps_, kbs_)) {
          RTC_LOG(LS_ERROR) << "[dgj-hwcodec] could not reopen the RAM encoder "
                               "after a texture failure; falling back to "
                               "software";
          return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        }
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
      vram_drain_at_us_ =
          rtc::TimeMicros() + VramDrainDelayUs(fps_);
      if (++vram_frames_since_log_ >= kTimingLogFrames) {
        vram_frames_since_log_ = 0;
        int64_t enc_us = 0, frames = 0;
        if (dgj_venc_timing(vhandle_, &enc_us, &frames) == DGJ_OK) {
          const int64_t d_enc = enc_us - last_venc_us_;
          const int64_t d_frames = frames - last_venc_frames_;
          last_venc_us_ = enc_us;
          last_venc_frames_ = frames;
          if (d_frames > 0) {
            RTC_LOG(LS_INFO) << "[dgj-hwcodec] vram encode over last "
                             << d_frames << " frames: "
                             << (d_enc / 1000.0 / d_frames)
                             << "ms (no i420->nv12 step)";
          }
        }
        int64_t submit_us = 0, completion_us = 0, ready_us = 0;
        int64_t stage_frames = 0, submit_max_us = 0;
        int64_t completion_max_us = 0, ready_max_us = 0;
        if (dgj_venc_stage_timing(
                vhandle_, &submit_us, &completion_us, &ready_us,
                &stage_frames, &submit_max_us, &completion_max_us,
                &ready_max_us) == DGJ_OK) {
          const int64_t d_frames = stage_frames - last_venc_stage_frames_;
          if (d_frames > 0) {
            RTC_LOG(LS_INFO)
                << "[dgj-nvenc-stage] over " << d_frames
                << " frames: submit avg "
                << ((submit_us - last_venc_submit_us_) / 1000.0 / d_frames)
                << "ms max_window " << (submit_max_us / 1000.0)
                << "ms | completion avg "
                << ((completion_us - last_venc_completion_us_) / 1000.0 /
                    d_frames)
                << "ms max_window " << (completion_max_us / 1000.0)
                << "ms | ready->callback avg "
                << ((ready_us - last_venc_ready_us_) / 1000.0 / d_frames)
                << "ms max_window " << (ready_max_us / 1000.0) << "ms";
          }
          last_venc_submit_us_ = submit_us;
          last_venc_completion_us_ = completion_us;
          last_venc_ready_us_ = ready_us;
          last_venc_stage_frames_ = stage_frames;
        }
        DgjNvencStageDetail detail = {};
        if (dgj_venc_stage_detail_timing(vhandle_, &detail) == DGJ_OK &&
            detail.frames > 0) {
          const double divisor = 1000.0 * detail.frames;
          RTC_LOG(LS_INFO)
              << "[dgj-nvenc-submit-detail] over " << detail.frames
              << " frames: driver_gate avg/max "
              << (detail.submit_gate_wait_us / divisor) << "/"
              << (detail.submit_gate_wait_max_us / 1000.0)
              << "ms | resolve " << (detail.shared_resolve_us / divisor) << "/"
              << (detail.shared_resolve_max_us / 1000.0)
              << "ms | d3d_lock " << (detail.d3d_lock_us / divisor) << "/"
              << (detail.d3d_lock_max_us / 1000.0)
              << "ms | copy " << (detail.copy_us / divisor) << "/"
              << (detail.copy_max_us / 1000.0)
              << "ms | register " << (detail.register_us / divisor) << "/"
              << (detail.register_max_us / 1000.0)
              << "ms | map " << (detail.map_us / divisor) << "/"
              << (detail.map_max_us / 1000.0)
              << "ms | encode_picture "
              << (detail.encode_picture_us / divisor) << "/"
              << (detail.encode_picture_max_us / 1000.0) << "ms";
          RTC_LOG(LS_INFO)
              << "[dgj-nvenc-completion-detail] over " << detail.frames
              << " frames: event avg/max "
              << (detail.event_wait_us / divisor) << "/"
              << (detail.event_wait_max_us / 1000.0)
              << "ms | lock " << (detail.bitstream_lock_us / divisor) << "/"
              << (detail.bitstream_lock_max_us / 1000.0)
              << "ms | driver_gate "
              << (detail.completion_gate_wait_us / divisor) << "/"
              << (detail.completion_gate_wait_max_us / 1000.0)
              << "ms | unlock " << (detail.bitstream_unlock_us / divisor)
              << "/" << (detail.bitstream_unlock_max_us / 1000.0)
              << "ms | unmap " << (detail.unmap_us / divisor) << "/"
              << (detail.unmap_max_us / 1000.0) << "ms";
        }
      }
      return WEBRTC_VIDEO_CODEC_OK;
    }
  }

  rtc::scoped_refptr<webrtc::I420BufferInterface> buffer =
      frame.video_frame_buffer()->ToI420();
  if (!buffer) {
    RTC_LOG(LS_ERROR) << "[dgj-hwcodec] frame is not I420-convertible; "
                         "dropping and requesting fallback";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  // The RAM encoder is released while the texture path is live, so a frame that
  // arrives as I420 after that (the quality scaler and the software fallback
  // wrapper both produce them) has to reopen it.
  if (handle_ == nullptr) {
    width_ = buffer->width();
    height_ = buffer->height();
    if (!RebuildRamEncoder(fps_, kbs_)) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
  }
  if (buffer->width() != width_ || buffer->height() != height_) {
    RTC_LOG(LS_WARNING)
        << "[dgj-hwcodec] resolution changed to " << buffer->width() << "x"
        << buffer->height() << " - rebuilding encoder";
    // Was Release() + dgj_enc_create, which also cleared callback_ -- after
    // which OnEncoded returned early on every frame and the stream went silent
    // with no error anywhere. RebuildRamEncoder only cycles handle_.
    width_ = buffer->width();
    height_ = buffer->height();
    if (!RebuildRamEncoder(fps_, kbs_)) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
  }



  RememberFrame(frame, frame.render_time_ms(), queue_us, encode_start_us,
                force_key);
  int ret = dgj_enc_encode(handle_, buffer->DataY(), buffer->StrideY(),
                           buffer->DataU(), buffer->StrideU(),
                           buffer->DataV(), buffer->StrideV(),
                           frame.render_time_ms(), force_key ? 1 : 0,
                           &HwcodecVideoEncoder::OnEncoded, this);
  if (ret != DGJ_OK) {
    TakeFrame(frame.render_time_ms());
    // Log the numbers the failure depends on, not just the code: the
    // conversion's preconditions relate the frame's strides to the NV12 layout
    // FFmpeg chose at open time, and neither is visible from the code alone.
    int32_t length = -1, ls_y = -1, ls_uv = -1, off_uv = -1;
    dgj_enc_layout(handle_, &length, &ls_y, &ls_uv, &off_uv);
    RTC_LOG(LS_ERROR) << "[dgj-hwcodec] dgj_enc_encode failed with " << ret
                      << "; frame " << buffer->width() << "x"
                      << buffer->height() << " strides y=" << buffer->StrideY()
                      << " u=" << buffer->StrideU()
                      << " v=" << buffer->StrideV() << "; encoder "
                      << width_ << "x" << height_ << " nv12 length=" << length
                      << " ls_y=" << ls_y << " ls_uv=" << ls_uv
                      << " off_uv=" << off_uv
                      << "; requesting software fallback";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  if (!encoded_contexts_.empty()) {
    ram_drain_until_us_ = rtc::TimeMicros() + kRamDrainBudgetUs;
  }

  // Split `encode` into its two halves once every kTimingLogFrames frames.
  // WebRTC only reports the sum, and the two have completely different fixes:
  // a slow conversion means replacing the scalar chroma interleave, a slow
  // encoder means the GPU round trip needs async depth.
  if (++frames_since_timing_log_ >= kTimingLogFrames) {
    frames_since_timing_log_ = 0;
    int64_t conv_us = 0, enc_us = 0, frames = 0;
    if (dgj_enc_timing(handle_, &conv_us, &enc_us, &frames) == DGJ_OK) {
      const int64_t d_conv = conv_us - last_conv_us_;
      const int64_t d_enc = enc_us - last_enc_us_;
      const int64_t d_frames = frames - last_timing_frames_;
      last_conv_us_ = conv_us;
      last_enc_us_ = enc_us;
      last_timing_frames_ = frames;
      if (d_frames > 0) {
        RTC_LOG(LS_INFO) << "[dgj-hwcodec] per-frame over last " << d_frames
                         << " frames: i420->nv12 "
                         << (d_conv / 1000.0 / d_frames) << "ms + encode "
                         << (d_enc / 1000.0 / d_frames) << "ms = "
                         << ((d_conv + d_enc) / 1000.0 / d_frames) << "ms";
      }
    }
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

void HwcodecVideoEncoder::ReleaseVramEncoder() {
  if (vhandle_ != nullptr) {
    dgj_venc_destroy(vhandle_);
    vhandle_ = nullptr;
    encoded_contexts_.clear();
  }
  vram_luid_ = 0;
  vram_width_ = 0;
  vram_height_ = 0;
  last_venc_us_ = 0;
  last_venc_frames_ = 0;
  last_venc_submit_us_ = 0;
  last_venc_completion_us_ = 0;
  last_venc_ready_us_ = 0;
  last_venc_stage_frames_ = 0;
  vram_frames_since_log_ = 0;
  vram_drain_at_us_ = 0;
}

void HwcodecVideoEncoder::ReleaseRamEncoderForTexturePath() {
  if (handle_ == nullptr) {
    return;
  }
  dgj_enc_destroy(handle_);
  handle_ = nullptr;
  encoded_contexts_.clear();
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] closed the " << codec_name_
                   << " RAM encoder; the texture path owns this session now";
}

bool HwcodecVideoEncoder::EnsureVramEncoder(void* device,
                                           int64_t luid,
                                           int width,
                                           int height) {
  if (vram_unavailable_ || device == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  if (vhandle_ != nullptr && luid == vram_luid_ && width == vram_width_ &&
      height == vram_height_) {
    return true;
  }
  // Adapter or geometry changed (or first frame): the encoder is bound to all
  // three, so it has to be reopened.
  ReleaseVramEncoder();

  const int ffi_codec = codec_type_ == webrtc::kVideoCodecH265
                            ? DGJ_CODEC_H265
                            : DGJ_CODEC_H264;
  vhandle_ = dgj_venc_create(luid, device, /*driver=*/nullptr, ffi_codec,
                             width, height, kbs_ > 0 ? kbs_ : 5000, fps_,
                             /*gop=*/0);
  if (vhandle_ == nullptr) {
    const int reason = dgj_venc_last_reason();
    // NO_ENCODER is permanent for this adapter, so latch it. Anything else may
    // be transient (driver busy right after a mode change), so leave the door
    // open and just take the I420 path for this frame.
    if (reason == DGJ_ERR_NO_ENCODER) {
      vram_unavailable_ = true;
    }
    RTC_LOG(LS_WARNING) << "[dgj-hwcodec] dgj_venc_create failed for luid "
                        << luid << " at " << width << "x" << height
                        << ", reason " << reason << "; using the I420 path"
                        << (vram_unavailable_ ? " from now on" : " this frame");
    return false;
  }
  vram_luid_ = luid;
  vram_width_ = width;
  vram_height_ = height;
  const int backend = dgj_venc_backend(vhandle_);
  int input_mode = dgj_venc_input_mode(vhandle_);
  int vbv_frames = -1;
  int async_depth = -1;
  int buffer_count = -1;
  if (backend == 0) {
    int configured_input_mode = input_mode;
    if (dgj_venc_config(vhandle_, &configured_input_mode, &vbv_frames,
                        &async_depth, &buffer_count) == DGJ_OK) {
      input_mode = configured_input_mode;
    }
  }
  const char* backend_name = backend == 0   ? "NV"
                             : backend == 1 ? "AMF"
                             : backend == 2 ? "MFX"
                              : backend == 3 ? "FFMPEG"
                                             : "unknown";
  std::string runtime_details;
  if (backend == 0) {
    runtime_details =
        std::string(" async_completion=1 input=") +
        (input_mode == 1
             ? "nv12_video_processor"
             : (input_mode == 2 ? "argb_dedicated_copy"
                                : "argb_direct_external")) +
        (input_mode == 1
             ? " nv12_slots=3 gpu_passes=2"
             : (input_mode == 2 ? " nvenc_input_ring=1 gpu_passes=2"
                                : " gpu_passes=1")) +
        " vbv_frames=" + std::to_string(vbv_frames) +
        " async_depth=" + std::to_string(async_depth) +
        " buffer_count=" + std::to_string(buffer_count);
  }
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] vram encoder open on luid " << luid
                   << " at " << width << "x" << height << " kbs " << kbs_
                   << " fps " << fps_ << " backend " << backend_name
                   << runtime_details
                   << " rate_mode=fixed gop=infinite";
  return true;
}

bool HwcodecVideoEncoder::RebuildRamEncoder(int fps, int kbs) {
  const int new_fps = fps < 1 ? 1 : fps;
  const int32_t new_kbs = ToFfiBitrate(kbs);
  // Timed on its own: an earlier round put this at 86ms on h264_qsv at 1080p,
  // and the per-frame trace is now seeing 200-490ms outliers. Whether a rebuild
  // alone accounts for those, or only part of them, decides where to look next.
  const int64_t rebuild_start_ms = rtc::TimeMillis();
  if (handle_ != nullptr) {
    dgj_enc_destroy(handle_);
    handle_ = nullptr;
  }
  encoded_contexts_.clear();
  handle_ = dgj_enc_create(codec_name_.c_str(), width_, height_, new_kbs,
                           new_fps, GopFor(new_fps));
  if (handle_ == nullptr) {
    RTC_LOG(LS_ERROR) << "[dgj-hwcodec] rebuild failed: " << codec_name_ << " "
                      << width_ << "x" << height_ << "@" << new_fps << "fps, "
                      << new_kbs << " kbps; requesting software fallback";
    return false;
  }
  last_rebuild_cost_ms_ = rtc::TimeMillis() - rebuild_start_ms;
  rebuild_cost_max_ms_ = std::max(rebuild_cost_max_ms_, last_rebuild_cost_ms_);
  ++rebuilds_in_window_;
  last_any_rebuild_ms_ = rtc::TimeMillis();
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] rebuilt: " << width_ << "x" << height_
                   << " " << fps_ << "->" << new_fps << "fps, " << kbs_ << "->"
                   << new_kbs << " kbps in " << last_rebuild_cost_ms_ << "ms";
  fps_ = new_fps;
  kbs_ = new_kbs;
  last_ram_rebuild_ms_ = rtc::TimeMillis();
  return true;
}

void HwcodecVideoEncoder::RememberFrame(const webrtc::VideoFrame& frame,
                                        int64_t pts_ms,
                                        int64_t queue_us,
                                        int64_t encode_start_us,
                                        bool force_key) {
  if (encoded_contexts_.size() >= kMaxEncodedContexts) {
    RTC_LOG(LS_WARNING) << "[dgj-hwcodec] dropping stale encoded context pts="
                        << encoded_contexts_.front().pts_ms;
    encoded_contexts_.pop_front();
  }
  encoded_contexts_.push_back(
      EncodedFrameContext{pts_ms, frame, queue_us, encode_start_us, force_key});
}

std::optional<HwcodecVideoEncoder::EncodedFrameContext>
HwcodecVideoEncoder::TakeFrame(int64_t pts_ms) {
  const auto it = std::find_if(
      encoded_contexts_.begin(), encoded_contexts_.end(),
      [pts_ms](const EncodedFrameContext& context) {
        return context.pts_ms == pts_ms;
      });
  if (it == encoded_contexts_.end()) {
    return std::nullopt;
  }
  EncodedFrameContext context = std::move(*it);
  encoded_contexts_.erase(it);
  return context;
}

void HwcodecVideoEncoder::TraceFrameEncoded(int64_t queue_us,
                                            int64_t enc_us,
                                            bool force_key) {
  const int64_t total_us = queue_us + enc_us;
  ++trace_frames_;
  trace_queue_sum_us_ += queue_us;
  trace_enc_sum_us_ += enc_us;
  trace_queue_max_us_ = std::max(trace_queue_max_us_, queue_us);
  trace_enc_max_us_ = std::max(trace_enc_max_us_, enc_us);
  trace_total_max_us_ = std::max(trace_total_max_us_, total_us);
  if (total_us >= kTraceOutlierUs) {
    ++trace_over_25ms_;
    if (total_us >= 2 * kTraceOutlierUs) {
      ++trace_over_50ms_;
    }
    if (force_key) {
      ++trace_force_key_slow_;
    }
    // Log the first few of each window individually: which stage a slow frame
    // sat in is the whole question, and an average cannot answer it.
    if (trace_outliers_logged_ < 5) {
      ++trace_outliers_logged_;
      const int64_t rebuild_age_ms =
          last_any_rebuild_ms_ > 0 ? rtc::TimeMillis() - last_any_rebuild_ms_
                                   : -1;
      RTC_LOG(LS_INFO) << "[dgj-trace] slow frame: queue "
                       << (queue_us / 1000.0) << "ms + encode "
                       << (enc_us / 1000.0) << "ms = " << (total_us / 1000.0)
                       << "ms | force_key=" << (force_key ? 1 : 0)
                       << " rebuild_age=" << rebuild_age_ms << "ms"
                       << " last_rebuild_cost=" << last_rebuild_cost_ms_ << "ms";
    }
  }

  if (trace_frames_ < kTraceWindowFrames) {
    return;
  }
  RTC_LOG(LS_INFO) << "[dgj-trace] over " << trace_frames_
                   << " frames: queue avg " << (trace_queue_sum_us_ / 1000.0 /
                                                trace_frames_)
                   << "ms max " << (trace_queue_max_us_ / 1000.0)
                   << "ms | encode avg "
                   << (trace_enc_sum_us_ / 1000.0 / trace_frames_) << "ms max "
                   << (trace_enc_max_us_ / 1000.0) << "ms | total max "
                   << (trace_total_max_us_ / 1000.0) << "ms | >25ms "
                   << trace_over_25ms_ << " >50ms " << trace_over_50ms_
                   << " | of those force_key " << trace_force_key_slow_
                   << " | rebuilds " << rebuilds_in_window_ << " worst "
                   << rebuild_cost_max_ms_ << "ms";
  trace_frames_ = 0;
  trace_queue_sum_us_ = 0;
  trace_queue_max_us_ = 0;
  trace_enc_sum_us_ = 0;
  trace_enc_max_us_ = 0;
  trace_total_max_us_ = 0;
  trace_over_25ms_ = 0;
  trace_over_50ms_ = 0;
  trace_outliers_logged_ = 0;
  trace_force_key_slow_ = 0;
  rebuilds_in_window_ = 0;
  rebuild_cost_max_ms_ = 0;
}

void HwcodecVideoEncoder::SetRates(const RateControlParameters& parameters) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  const uint32_t sum_bps = parameters.bitrate.get_sum_bps();
  if (sum_bps == 0) {
    // Encoding paused; leave the hardware encoder alone rather than feeding it
    // a zero bitrate.
    return;
  }
  const int new_kbs = static_cast<int>(sum_bps / 1000);
  const int new_fps = parameters.framerate_fps >= 1.0
                          ? static_cast<int>(parameters.framerate_fps + 0.5)
                          : -1;
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (worker_stop_) {
      return;
    }
    pending_fps_ = new_fps;
    pending_kbs_ = new_kbs;
    pending_rates_ = true;
  }
  worker_cv_.notify_one();
}

void HwcodecVideoEncoder::ApplyRatesOnWorker(int new_fps, int new_kbs) {
  if (!handle_ && !vhandle_) {
    return;
  }
  if (new_fps < 1) {
    new_fps = fps_;
  }

  // Keep the configured 1080p desktop quality tier on both paths. WebRTC's
  // initial allocation is often below the tier (for example 6285 kbps), and
  // applying it before the native texture encoder is opened causes the first
  // NVENC session to start at that lower rate. That immediately raises QP and
  // destroys dark edges and moving text. The same clamp used by InitEncode
  // must also be applied to asynchronous SetRates updates.
  new_kbs =
      StableVramBitrateKbps(width_, height_, new_kbs, codec_type_);

  // `new_fps` is WebRTC's observed/content rate, not the configured capture
  // tier. Following it made an idle desktop repeatedly reconfigure 64->2->64.
  // Keep the configured fps. The NVENC quality tier is also fixed for the
  // session: on this driver, in-place rate changes serialize with outstanding
  // output and were the dominant source of visible 100-400ms frame gaps.
  if (vhandle_ != nullptr) {
    const int64_t now_ms = rtc::TimeMillis();
    const bool bitrate_stale =
        kbs_ > 0 &&
        std::abs(new_kbs - kbs_) * 100 / kbs_ >= kVramBitrateChangePercent;
    if (bitrate_stale &&
        now_ms - last_vram_rate_update_ms_ >= kVramRateHoldLogMs) {
      RTC_LOG(LS_INFO) << "[dgj-hwcodec] vram rate hold fixed=" << kbs_
                       << " requested=" << new_kbs
                       << " kbps (runtime reconfigure disabled)";
      last_vram_rate_update_ms_ = now_ms;
    }
    return;
  }

  if (handle_ == nullptr) {
    return;
  }

  // RAM path. The bitrate cannot be changed in place: dgj_enc_set_bitrate only
  // writes AVCodecContext::bit_rate, and libmfx does not re-read it without
  // MFXVideoENCODE_Reset. It returns DGJ_OK, so the failure is invisible from
  // here. Measured symptom: opened at 3500kbps while WebRTC's target had risen to
  // 5772kbps, the encoder kept emitting 3500kbps worth of frames.
  //
  // The frame rate is deliberately NOT followed, even though it is the other half
  // of the CBR per-frame budget (bit_rate / fps). Two rounds of trying:
  //   - threshold 5 frames absolute: 14 rebuilds in 40s
  //   - threshold 33% on an EMA, then 15%: still oscillated 64->27->34->42
  // The reason is not calibration. framerate_fps measures delivered frames, and an
  // idle desktop legitimately delivers 2fps while a window drag delivers 55fps
  // (measured). That signal cannot distinguish "the user picked a different tier"
  // from "nothing is moving", so following it reconfigures the encoder for idle
  // and then chases the recovery. Each rebuild costs 86ms, an IDR, and a CBR
  // controller reset, which is worse than a divisor that is ~25% too high.
  //
  // Doing this properly means telling the encoder the *configured* rate through
  // its own path (Dart -> native -> here) rather than inferring it. Until that
  // exists, fps stays at whatever InitEncode was given.
  const bool kbs_stale =
      kbs_ > 0 &&
      std::abs(new_kbs - kbs_) * 100 / kbs_ >= kRamRebuildKbsPercent;
  if (!kbs_stale) {
    return;
  }
  if (rtc::TimeMillis() - last_ram_rebuild_ms_ < kMinRamRebuildIntervalMs) {
    return;
  }
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] rebuild trigger: kbs " << kbs_ << "->"
                   << new_kbs << " raw_fps=" << new_fps;
  // A failed rebuild leaves handle_ null; Encode() then returns
  // WEBRTC_VIDEO_CODEC_UNINITIALIZED and the fallback wrapper takes over, which
  // is the same contract as a failed InitEncode.
  if (!RebuildRamEncoder(fps_, new_kbs)) {
    RecordWorkerResult(WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  }
}


webrtc::VideoEncoder::EncoderInfo HwcodecVideoEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  // True so the video stream encoder hands our kNative texture buffers through
  // untouched. It does not commit us to the texture path: Encode() checks the
  // buffer type and the adapter LUID per frame and takes the I420 path otherwise.
  info.supports_native_handle = true;
  // codec_name_ is the RAM fallback candidate. Native textures use the
  // adapter-bound backend, so reporting h264_qsv for an NVENC session is false.
  info.implementation_name = "dgj_hwcodec:auto";
  info.is_hardware_accelerated = true;
  // Screenshare under MAINTAIN_RESOLUTION: the video stream encoder already
  // keeps the resolution fixed; a quality scaler would fight the 30fps budget.
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.supports_simulcast = false;
  info.requested_resolution_alignment = 2;
  info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kI420};
  info.is_qp_trusted = true;
  return info;
}

void HwcodecVideoEncoder::OnEncoded(const uint8_t* data,
                                    size_t len,
                                    int64_t pts_ms,
                                    int key,
                                    void* user) {
  HwcodecVideoEncoder* self = static_cast<HwcodecVideoEncoder*>(user);
  webrtc::EncodedImageCallback* callback =
      self->callback_.load(std::memory_order_acquire);
  std::optional<EncodedFrameContext> context = self->TakeFrame(pts_ms);
  if (!callback) {
    return;  // Racing Release(); drop the frame.
  }
  if (!context) {
    RTC_LOG(LS_WARNING) << "[dgj-hwcodec] encoded packet has no source frame, "
                           "pts="
                        << pts_ms;
    return;
  }

  webrtc::EncodedImage image;
  image.SetEncodedData(webrtc::EncodedImageBuffer::Create(data, len));
  image.SetRtpTimestamp(context->frame.rtp_timestamp());
  image.SetColorSpace(context->frame.color_space());
  image.SetSimulcastIndex(0);
  image._encodedWidth = self->width_;
  image._encodedHeight = self->height_;
  image.ntp_time_ms_ = context->frame.ntp_time_ms();
  image.capture_time_ms_ = context->frame.render_time_ms();
  image.rotation_ = context->frame.rotation();
  image._frameType = key ? webrtc::VideoFrameType::kVideoFrameKey
                         : webrtc::VideoFrameType::kVideoFrameDelta;

  if (self->codec_type_ == webrtc::kVideoCodecH264) {
    self->bitstream_parser_.ParseBitstream(image);
    image.qp_ = self->bitstream_parser_.GetLastSliceQp().value_or(-1);
  } else {
    // WebRTC has no H.265 slice-QP parser in this branch. Keep the value
    // explicitly unknown rather than feeding H.264 syntax to it.
    image.qp_ = -1;
  }

  // Close the measurement attached to the packet's actual source frame.
  self->TraceFrameEncoded(context->queue_us,
                          rtc::TimeMicros() - context->encode_start_us,
                          context->force_key);

  webrtc::CodecSpecificInfo csi;
  csi.codecType = self->codec_type_;
  if (self->codec_type_ == webrtc::kVideoCodecH264) {
    csi.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;
    csi.codecSpecific.H264.idr_frame = key != 0;
    csi.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
    csi.codecSpecific.H264.base_layer_sync = false;
  }

  callback->OnEncodedImage(image, &csi);
}

}  // namespace libwebrtc
