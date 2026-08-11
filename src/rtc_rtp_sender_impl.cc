#include "rtc_rtp_sender_impl.h"

#include <src/rtc_audio_track_impl.h>
#include <src/rtc_dtls_transport_impl.h>
#include <src/rtc_dtmf_sender_impl.h>
#include <src/rtc_rtp_parameters_impl.h>
#include <src/rtc_video_track_impl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace {

bool EnvEnabled(const char* name, bool default_value = true) {
  char value[16] = {};
  size_t length = 0;
  if (getenv_s(&length, value, sizeof(value), name) != 0 || length == 0) {
    return default_value;
  }
  std::string setting(value);
  std::transform(setting.begin(), setting.end(), setting.begin(), ::tolower);
  return setting != "0" && setting != "false" && setting != "off";
}

}  // namespace

namespace libwebrtc {
RTCRtpSenderImpl::RTCRtpSenderImpl(
    rtc::scoped_refptr<webrtc::RtpSenderInterface> rtp_sender)
    : rtp_sender_(rtp_sender) {

  // 获取 sender 的参数对象
  auto parms = parameters();

  // A hardware remote-desktop encoder owns its own mailbox/backpressure policy.
  // WebRTC's CPU adaptation measures callback latency rather than actual CPU
  // time, so one GPU completion tail was interpreted as CPU overload and
  // permanently reduced a 32fps source to 10fps. Disable both resolution and
  // framerate adaptation by default; the encoder already replaces stale frames
  // instead of queueing them. Set DGJ_UU_FIXED_CADENCE=0 for the old
  // MAINTAIN_RESOLUTION behaviour during A/B tests.
  parms->SetDegradationPreference(
      EnvEnabled("DGJ_UU_FIXED_CADENCE")
          ? RTCDegradationPreference::DISABLED
          : RTCDegradationPreference::MAINTAIN_RESOLUTION);

  scoped_refptr<libwebrtc::RTCRtpEncodingParameters> encoding =
      RTCRtpEncodingParameters::Create();

  // Keep native startup defaults aligned with the 1080p60 direct-UDP profile.
  // Dart setParameters is rejected for this external source on Windows, so
  // these values are the effective sender limits rather than just fallbacks.
  // The old 32fps ceiling imposed a 31ms source period even though the shared
  // texture NVENC path now submits in about 0.25ms with no queue growth.
  encoding->set_max_bitrate_bps(40 * 1000 * 1000);
  encoding->set_min_bitrate_bps(12 * 1000 * 1000);
  encoding->set_max_framerate(60);

  // The capture source runs at 64fps (60 * 1.07) to avoid phase drift against
  // a 60Hz desktop. Keeping the sender at 60 discards only the cadence margin
  // and preserves one encoded frame per display interval.
  //
  // Smoothness, separately, is not driven by this value at all. Three measured
  // runs:
  //   elevated peer  shown 28.5  dropped 14.6/window  -> user called it choppy
  //   user-mode 40s  shown 29.4  dropped 11.9/window  -> user called it smooth
  //   user-mode 150s shown 28.1  dropped 17.3/window  jitterBuffer 11.3ms
  //                  (worst of the three)             -> user called it smooth
  // So shown/dropped/assembly do not predict perceived smoothness. Chasing
  // shown toward the 30fps display ceiling buys nothing.
  //
  // Only the sending side uses this; the controlling side has no video sender.
  encoding->set_num_temporal_layers(1);             // match L1T1
  encoding->set_active(true);
  encoding->set_network_priority(RTCPriority::kHigh);
  encoding->set_scalability_mode("L1T1");

  // 将编码参数添加到向量中
  // std::vector<RTCRtpEncodingParameters> encodings;
  // encodings.push_back(*(encoding.get()));
  std::vector<scoped_refptr<RTCRtpEncodingParameters>> encodings;
  encodings.push_back(encoding);

  // 将编码参数应用到参数对象中
  parms->set_encodings(encodings);

  set_parameters(parms);

}

bool RTCRtpSenderImpl::set_track(scoped_refptr<RTCMediaTrack> track) {
  if (track == nullptr) {
    return rtp_sender_->SetTrack(nullptr);
  }
  if (std::string(webrtc::MediaStreamTrackInterface::kVideoKind) ==
      track->kind().std_string()) {
    VideoTrackImpl* impl = static_cast<VideoTrackImpl*>(track.get());
    return rtp_sender_->SetTrack(impl->rtc_track().get());
  } else if (std::string(webrtc::MediaStreamTrackInterface::kAudioKind) ==
             track->kind().std_string()) {
    AudioTrackImpl* impl = static_cast<AudioTrackImpl*>(track.get());
    return rtp_sender_->SetTrack(impl->rtc_track().get());
  }
  return false;
}

scoped_refptr<RTCMediaTrack> RTCRtpSenderImpl::track() const {
  rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track =
      rtp_sender_->track();

  if (nullptr == track.get()) {
    return scoped_refptr<RTCMediaTrack>();
  }

  if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    return scoped_refptr<RTCMediaTrack>(new RefCountedObject<VideoTrackImpl>(
        rtc::scoped_refptr<webrtc::VideoTrackInterface>(
            static_cast<webrtc::VideoTrackInterface*>(track.get()))));
  } else if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
    return scoped_refptr<RTCMediaTrack>(new RefCountedObject<AudioTrackImpl>(
        rtc::scoped_refptr<webrtc::AudioTrackInterface>(
            rtc::scoped_refptr<webrtc::AudioTrackInterface>(
                static_cast<webrtc::AudioTrackInterface*>(track.get())))));
  }
  return scoped_refptr<RTCMediaTrack>();
}

scoped_refptr<RTCDtlsTransport> RTCRtpSenderImpl::dtls_transport() const {
  if (nullptr == rtp_sender_->dtls_transport().get()) {
    return scoped_refptr<RTCDtlsTransport>();
  }
  return new RefCountedObject<RTCDtlsTransportImpl>(
      rtp_sender_->dtls_transport());
}

uint32_t RTCRtpSenderImpl::ssrc() const { return rtp_sender_->ssrc(); }

RTCMediaType RTCRtpSenderImpl::media_type() const {
  return static_cast<RTCMediaType>(rtp_sender_->media_type());
}

const string RTCRtpSenderImpl::id() const { return rtp_sender_->id(); }

const vector<string> RTCRtpSenderImpl::stream_ids() const {
  std::vector<string> vec;
  for (std::string item : rtp_sender_->stream_ids()) {
    vec.push_back(item.c_str());
  }
  return vec;
}

void RTCRtpSenderImpl::set_stream_ids(const vector<string> stream_ids) const {
  std::vector<std::string> list;
  for (auto id : stream_ids.std_vector()) {
    list.push_back(to_std_string(id));
  }
  rtp_sender_->SetStreams(list);
}

const vector<scoped_refptr<RTCRtpEncodingParameters>>
RTCRtpSenderImpl::init_send_encodings() const {
  std::vector<scoped_refptr<RTCRtpEncodingParameters>> vec;
  for (webrtc::RtpEncodingParameters item :
       rtp_sender_->init_send_encodings()) {
    vec.push_back(new RefCountedObject<RTCRtpEncodingParametersImpl>(item));
  }
  return vec;
}

scoped_refptr<RTCRtpParameters> RTCRtpSenderImpl::parameters() const {
  return new RefCountedObject<RTCRtpParametersImpl>(
      rtp_sender_->GetParameters());
}

bool RTCRtpSenderImpl::set_parameters(
    const scoped_refptr<RTCRtpParameters> parameters) {
  RTCRtpParametersImpl* impl =
      static_cast<RTCRtpParametersImpl*>(parameters.get());
  return rtp_sender_->SetParameters(impl->rtp_parameters()).ok();
}

scoped_refptr<RTCDtmfSender> RTCRtpSenderImpl::dtmf_sender() const {
  if (nullptr == rtp_sender_->GetDtmfSender().get()) {
    return scoped_refptr<RTCDtmfSender>();
  }
  return new RefCountedObject<RTCDtmfSenderImpl>(rtp_sender_->GetDtmfSender());
}

}  // namespace libwebrtc
