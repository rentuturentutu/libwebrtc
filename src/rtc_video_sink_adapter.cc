#include "rtc_video_sink_adapter.h"

#include "rtc_base/logging.h"
#include "rtc_video_frame_impl.h"
#include "rtc_video_track.h"
#if defined(_WIN32)
#include "system_wrappers/include/ntp_time.h"
#endif

namespace libwebrtc {

VideoSinkAdapter::VideoSinkAdapter(
    rtc::scoped_refptr<webrtc::VideoTrackInterface> track)
    : rtc_track_(track), crt_sec_(new webrtc::Mutex()) {
  rtc_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": ctor " << (void*)this;
}

VideoSinkAdapter::~VideoSinkAdapter() {
  rtc_track_->RemoveSink(this);
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": dtor ";
}

// VideoSinkInterface implementation
void VideoSinkAdapter::OnFrame(const webrtc::VideoFrame& video_frame) {
  scoped_refptr<VideoFrameBufferImpl> frame_buffer =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(
              video_frame.video_frame_buffer()));

  frame_buffer->set_rotation(video_frame.rotation());
  frame_buffer->set_timestamp_us(video_frame.timestamp_us());
#if defined(_WIN32)
  int64_t capture_time_ms = video_frame.ntp_time_ms();
  if (capture_time_ms <= 0) {
    // Fall back to the standard abs-capture-time RTP extension. Its timestamp
    // uses the sender/capturer clock; local_capture_clock_offset maps that
    // clock into this receiver's NTP domain:
    //   capture clock = local clock + offset.
    for (const auto& packet_info : video_frame.packet_infos()) {
      if (!packet_info.absolute_capture_time().has_value() ||
          !packet_info.local_capture_clock_offset().has_value()) {
        continue;
      }
      const int64_t remote_capture_ms =
          webrtc::NtpTime(packet_info.absolute_capture_time()
                              ->absolute_capture_timestamp)
              .ToMs();
      capture_time_ms =
          remote_capture_ms - packet_info.local_capture_clock_offset()->ms();
      break;
    }
  }
  frame_buffer->set_capture_time_ms(capture_time_ms);
#endif

  webrtc::MutexLock cs(crt_sec_.get());
  for (auto renderer : renderers_) {
    renderer->OnFrame(frame_buffer);
  }
}

void VideoSinkAdapter::AddRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": AddRenderer " << (void*)renderer;
  webrtc::MutexLock cs(crt_sec_.get());
  renderers_.push_back(renderer);
}

void VideoSinkAdapter::RemoveRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": RemoveRenderer " << (void*)renderer;
  webrtc::MutexLock cs(crt_sec_.get());
  renderers_.erase(
      std::remove_if(
          renderers_.begin(), renderers_.end(),
          [renderer](
              const RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer_) {
            return renderer_ == renderer;
          }),
      renderers_.end());
}

void VideoSinkAdapter::AddRenderer(
    rtc::VideoSinkInterface<webrtc::VideoFrame>* renderer) {
  rtc_track_->AddOrUpdateSink(renderer, rtc::VideoSinkWants());
}
void VideoSinkAdapter::RemoveRenderer(
    rtc::VideoSinkInterface<webrtc::VideoFrame>* renderer) {
  rtc_track_->RemoveSink(renderer);
}

}  // namespace libwebrtc
