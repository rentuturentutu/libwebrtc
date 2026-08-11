/*
 * HwcodecVideoEncoder -- WebRTC VideoEncoder backed by the dgj_encode Rust
 * library (FFmpeg h264_qsv / nvenc / amf hardware H264).
 *
 * Only drives hardware H264. VP8/VP9/AV1 traffic never reaches this class --
 * HwcodecVideoEncoderFactory routes those straight to the builtin factory.
 *
 * Error contract (see video_encoder_software_fallback_wrapper.cc):
 *   - InitEncode returning anything != OK makes the fallback wrapper hand the
 *     whole session to OpenH264.
 *   - Encode returning WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE switches the
 *     wrapper to OpenH264 from the next frame on.
 */
#ifndef LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_ENCODER_H_
#define LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_ENCODER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "api/video/video_frame.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"

namespace libwebrtc {

class HwcodecVideoEncoder : public webrtc::VideoEncoder {
 public:
  // `codec_name` is one of the names dgj_enc_probe returned ("h264_qsv").
  HwcodecVideoEncoder(std::string codec_name,
                      webrtc::VideoCodecType codec_type);
  ~HwcodecVideoEncoder() override;

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const VideoEncoder::Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  struct PendingFrame {
    webrtc::VideoFrame frame;
    bool force_key;
    int64_t enqueued_us;
  };

  struct EncodedFrameContext {
    int64_t pts_ms;
    webrtc::VideoFrame frame;
    int64_t queue_us;
    int64_t encode_start_us;
    bool force_key;
  };

  static void OnEncoded(const uint8_t* data,
                        size_t len,
                        int64_t pts_ms,
                        int key,
                        void* user);

  void StartWorker();
  void StopWorker();
  void WorkerLoop();
  int32_t EncodeOnWorker(const webrtc::VideoFrame& frame, bool force_key);
  void ApplyRatesOnWorker(int new_fps, int new_kbs);
  void RecordWorkerResult(int32_t result);
  void RememberFrame(const webrtc::VideoFrame& frame,
                     int64_t pts_ms,
                     int64_t queue_us,
                     int64_t encode_start_us,
                     bool force_key);
  std::optional<EncodedFrameContext> TakeFrame(int64_t pts_ms);

  // Opens (or reopens) the VRAM encoder for `luid`/`device`. False means this
  // frame must take the I420 path.
  bool EnsureVramEncoder(void* device, int64_t luid, int width, int height);
  void ReleaseVramEncoder();

  // Reopens the RAM encoder at `fps`/`kbs` using the current width_/height_.
  //
  // Deliberately NOT Release() + dgj_enc_create: Release() clears callback_,
  // after which OnEncoded drops every frame and the stream dies silently.
  // This only cycles handle_.
  //
  // Why a rebuild is the only option: dgj_enc_set_bitrate just writes
  // AVCodecContext::bit_rate, which libmfx does not re-read without a reset, and
  // the RAM path has no frame-rate setter at all (dgj_encode_ffi.h:141). Both
  // values are baked in at open time, and both feed the CBR per-frame budget
  // (bit_rate / fps), so a stale one shows up directly as picture quality.
  bool RebuildRamEncoder(int fps, int kbs);

  // Closes the RAM encoder once the texture path is carrying the session.
  //
  // Not Release(): that clears callback_ and would kill the stream. Not
  // ReleaseVramEncoder's counterpart either -- this one is expected to be undone
  // by RebuildRamEncoder if an I420 frame shows up later (quality scaler,
  // software fallback wrapper) or if the texture path fails.
  //
  // Why it matters: the two encoders sit on different adapters (QSV on the Intel
  // iGPU, NVENC on the discrete one that the capture texture lives on), and
  // holding both sessions open measured 50.8ms per vram encode against 10.9ms
  // for the RAM path by itself. SetRates also returns early while vhandle_
  // exists, so the RAM encoder's bitrate would go stale regardless.
  void ReleaseRamEncoderForTexturePath();


  std::string codec_name_;
  const webrtc::VideoCodecType codec_type_;
  void* handle_ = nullptr;  // dgj_enc_create result
  // QSV may return a packet one or more submissions after its source frame.
  // Keep the small number of in-flight frames keyed by packet PTS so RTP and
  // capture metadata cannot be copied from the wrong input.
  std::deque<EncodedFrameContext> encoded_contexts_;
  std::atomic<webrtc::EncodedImageCallback*> callback_{nullptr};
  webrtc::H264BitstreamParser bitstream_parser_;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 30;
  int kbs_ = 0;
  int64_t last_key_ms_ = -1000000;  // keyframe-request throttle, see Encode()
  // Rebuild throttle for the RAM encoder. A rebuild costs 86ms on h264_qsv at
  // 1080p, and SetRates can fire every frame, so it needs its own limiter
  // independent of the keyframe one.
  int64_t last_ram_rebuild_ms_ = -1000000;

  // hwcodec's RAM encoder blocks inside Encoder::encode() while QSV waits for
  // the GPU. Keep that wait off WebRTC's encoder task queue and retain at most
  // one not-yet-started frame. If QSV stalls, newer frames replace the pending
  // one instead of becoming a latency-producing FIFO.
  std::atomic<bool> initialized_{false};
  std::atomic<int32_t> worker_error_{0};
  std::thread worker_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  bool worker_stop_ = false;
  std::optional<PendingFrame> pending_frame_;
  bool pending_rates_ = false;
  int pending_fps_ = -1;
  int pending_kbs_ = 0;
  // After an async RAM submit returns EAGAIN, poll briefly so the last frame in
  // a low-motion burst does not wait for the next capture to drain its packet.
  int64_t ram_drain_until_us_ = 0;
  // NVENC keeps one surface in flight so D3D11 and encode can overlap. If no
  // next capture arrives, drain that tail after more than one expected frame
  // interval; an active stream gets the parallel pipeline, an idle one cannot
  // retain its last visual update indefinitely.
  int64_t vram_drain_at_us_ = 0;

  // Protected by worker_mutex_. Reported every kTraceWindowFrames processed
  // frames so a test can tell whether a QSV stall was absorbed by replacement
  // or merely moved to another thread.
  int async_submitted_ = 0;
  int async_processed_ = 0;
  int async_replaced_ = 0;
  int64_t async_pending_age_max_us_ = 0;
  int vram_busy_drops_ = 0;



  // Timing split diagnostics, logged every kTimingLogFrames frames.
  int frames_since_timing_log_ = 0;
  int64_t last_conv_us_ = 0;
  int64_t last_enc_us_ = 0;
  int64_t last_timing_frames_ = 0;

  // VRAM (texture) path. Used when the incoming frame is a kNative
  // DgjD3D11TextureBuffer whose adapter has a hardware encoder. Both paths stay
  // live: the software fallback wrapper and the JPEG thumbnail path still feed
  // I420 frames, and a frame can arrive from either source at any time.
  void* vhandle_ = nullptr;  // dgj_venc_create result
  int64_t vram_luid_ = 0;
  // Tracked separately from width_/height_, which describe the RAM encoder: the
  // two can legitimately differ while both are open.
  int vram_width_ = 0;
  int vram_height_ = 0;
  // Latched once dgj_venc_create reports DGJ_ERR_NO_ENCODER for this adapter, so
  // a hopeless open is not retried 60 times a second.
  bool vram_unavailable_ = false;
  int vram_frames_since_log_ = 0;
  int64_t last_venc_us_ = 0;
  int64_t last_venc_frames_ = 0;
  int64_t last_venc_submit_us_ = 0;
  int64_t last_venc_completion_us_ = 0;
  int64_t last_venc_ready_us_ = 0;
  int64_t last_venc_stage_frames_ = 0;
  int64_t last_vram_rate_update_ms_ = -1000000;

  // --- Per-frame latency tracing on the send side ---
  //
  // Why per-frame and not more averages: stopwatch photographs put the
  // end-to-end delay at 29-88ms across ten samples on an otherwise idle LAN.
  // The averages we already log only account for ~28ms and are all stable, so
  // the thing that actually hurts is the ~50ms of *spread*, and a mean cannot
  // locate a spread. These fields time each frame individually and report the
  // distribution plus every outlier.
  //
  // The clock is `VideoFrame::timestamp_us()`, which for our pipeline is the
  // capture instant: dgj_capture stamps DgjFrame::ts_us with now_us() at
  // delivery (rust/dgj_capture/src/lib.rs) and ExternalCapturer::PushFrameBGRA
  // passes it through to set_timestamp_us. So `now - frame.timestamp_us()` at
  // the top of Encode() is a real capture-to-encoder-queue figure and needs no
  // extra plumbing.
  //
  // Two stages, because they have completely different suspects:
  //   queue  capture -> worker entry. Covers conversion, VideoStreamEncoder's
  //          task queue, and the single pending-frame slot above.
  //   enc    worker entry -> OnEncoded. The encoder itself, including the
  //          hwcodec upload and its synchronous GPU wait.
  void TraceFrameEncoded(int64_t queue_us,
                         int64_t enc_us,
                         bool force_key);

  static constexpr int kTraceWindowFrames = 300;
  // Frames slower than this (queue + encode) get a line of their own. 25ms is
  // about 1.5 frame intervals at 64fps: late enough to matter, rare enough not
  // to flood.
  static constexpr int64_t kTraceOutlierUs = 25000;
  int trace_frames_ = 0;
  int64_t trace_queue_sum_us_ = 0;
  int64_t trace_queue_max_us_ = 0;
  int64_t trace_enc_sum_us_ = 0;
  int64_t trace_enc_max_us_ = 0;
  int64_t trace_total_max_us_ = 0;
  int trace_over_25ms_ = 0;
  int trace_over_50ms_ = 0;
  int trace_outliers_logged_ = 0;

  // Attribution for the outliers. The three things a 400ms frame could be:
  //
  //   force_key   a PLI arrived, so this frame was encoded as an IDR.
  //   rebuild_age how long since the last RAM bitrate rebuild. SetRates uses a
  //               40% drift threshold and an 8s throttle, so a value near that
  //               bound identifies the trigger.
  //   nothing     both of the above cold, in which case the stall is inside the
  //               encoder itself -- hwcodec pins async_depth=1 with LOW_DELAY, so
  //               every Encode blocks until the GPU is done, and an iGPU shared
  //               with browser compositing and video decode can make us wait.
  //
  // The distinction matters because a rate rebuild is ours to fix, while a GPU
  // scheduling stall requires the texture/NVENC path or driver-level changes.
  int trace_force_key_slow_ = 0;
  // Wall-clock ms of the last RAM rebuild, from either trigger.
  int64_t last_any_rebuild_ms_ = 0;
  // Cost of the last rebuild on its own, so it can be compared with the frame
  // total rather than inferred from it.
  int64_t last_rebuild_cost_ms_ = 0;
  int64_t rebuild_cost_max_ms_ = 0;
  int rebuilds_in_window_ = 0;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_ENCODER_H_
