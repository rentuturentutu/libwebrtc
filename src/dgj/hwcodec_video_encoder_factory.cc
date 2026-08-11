#include "libwebrtc/src/dgj/hwcodec_video_encoder_factory.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "absl/strings/match.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "dgj_encode_ffi.h"
#include "libwebrtc/src/dgj/hwcodec_video_encoder.h"
#include "rtc_base/logging.h"

namespace libwebrtc {

namespace {

// dgj_enc_probe fills a JSON array of
//   {"name":"h264_qsv","format":"H264","priority":..,"hwdevice":".."}
// Pull the H264 entry we want and return its name. Empty means "no hardware".
//
// Why not simply the first entry, which is what this did originally: the order is
// whatever hwcodec's `available_encoders` happened to enumerate, and on a laptop
// with both an Intel iGPU and a discrete NVIDIA that put h264_qsv first. Measured
// on this machine, QSV encodes 1080p in 11.1-11.3ms at qp 26, while UU Remote --
// same GPU, same LAN -- reports 4ms at qp 13 out of NVENC. Encoder choice was
// therefore the largest single unexplained gap, and it was being made by
// enumeration order.
//
// Preference order is explicit rather than driven by the probe's `priority`
// field: that value comes from hwcodec and its meaning is not documented here, so
// relying on it would be trading one accidental ordering for another.
//
// DGJ_H264_ENCODER overrides everything (e.g. DGJ_H264_ENCODER=h264_qsv to get
// the old behaviour back for an A/B) as long as the probe found that name.
std::string PickCodecName(char* json,
                          const char* format,
                          const char* env_name,
                          std::initializer_list<const char*> preference) {
  if (!json) {
    return "";
  }

  // Collect every H264 candidate the probe reported, in probe order.
  std::vector<std::string> candidates;
  const char* p = json;
  while (p && *p) {
    const char* name_key = strstr(p, "\"name\":\"");
    if (!name_key) {
      break;
    }
    name_key += 8;  // past `"name":"`
    const char* name_end = strchr(name_key, '"');
    if (!name_end) {
      break;
    }
    const char* format_key = strstr(name_end, "\"format\":\"");
    if (format_key) {
      const char* format_start = format_key + 10;
      const char* format_end = strchr(format_start, '"');
      const size_t format_len = strlen(format);
      if (format_end && static_cast<size_t>(format_end - format_start) ==
                            format_len &&
          strncmp(format_start, format, format_len) == 0) {
        candidates.emplace_back(name_key, name_end);
      }
    }
    p = name_end + 1;
  }
  if (candidates.empty()) {
    return "";
  }

  const auto has = [&candidates](const char* name) {
    return std::find(candidates.begin(), candidates.end(), name) !=
           candidates.end();
  };

  char env[64] = {};
  size_t env_len = 0;
  if (getenv_s(&env_len, env, sizeof(env), env_name) == 0 &&
      env_len > 0 && has(env)) {
    RTC_LOG(LS_INFO) << "[dgj-hwcodec] " << env_name << "=" << env;
    return env;
  }

  // nvenc first, then amf, then qsv. Discrete-GPU encoders are both faster and
  // better at a given bitrate than the Intel iGPU's on the machines this runs
  // on; qsv stays as the fallback because it is the one that is always there.
  for (const char* preferred : preference) {
    if (has(preferred)) {
      return preferred;
    }
  }
  // Something unrecognised (a future vendor, or mediacodec on another platform):
  // take what the probe put first rather than refusing hardware altogether.
  return candidates.front();
}

bool IsH264Format(const webrtc::SdpVideoFormat& format) {
  // Codec names negotiate case-insensitively; match the same lenient rule the
  // internal factory applies to codec names.
  return absl::EqualsIgnoreCase(format.name, "H264");
}

bool IsH265Format(const webrtc::SdpVideoFormat& format) {
  return absl::EqualsIgnoreCase(format.name, "H265");
}

bool H265Enabled() {
  char value[16] = {};
  size_t length = 0;
  if (getenv_s(&length, value, sizeof(value), "DGJ_H265") != 0 ||
      length == 0) {
    return true;
  }
  std::string setting(value);
  std::transform(setting.begin(), setting.end(), setting.begin(), ::tolower);
  return setting != "0" && setting != "false" && setting != "off";
}

}  // namespace

HwcodecVideoEncoderFactory::HwcodecVideoEncoderFactory()
    : builtin_(webrtc::CreateBuiltinVideoEncoderFactory()) {
  // One probe per process. hwcodec's `available_encoders` actually opens each
  // candidate and encodes a test frame (a present-but-broken driver must not
  // be advertised), which costs a few hundred milliseconds -- far too much to
  // repeat per session.
  // Warm the adapter-bound texture probe at factory construction. Previously
  // the first captured frame paid this cost (about 1.1s on the test machine).
  char vram_buf[4096] = {};
  const int vram_ret =
      dgj_venc_probe(1920, 1080, 6000, 60, vram_buf, sizeof(vram_buf));
  if (vram_ret == DGJ_OK) {
    RTC_LOG(LS_INFO) << "[dgj-hwcodec] vram probe: " << vram_buf;
  } else {
    RTC_LOG(LS_WARNING) << "[dgj-hwcodec] vram probe failed (" << vram_ret
                        << "); texture path will probe on first use";
  }

  char buf[4096];
  int ret = dgj_enc_probe(1920, 1080, 5000, 30, buf, sizeof(buf));
  if (ret != DGJ_OK) {
    RTC_LOG(LS_WARNING)
        << "[dgj-hwcodec] probe failed (" << ret << "); hardware H264 off";
    return;
  }
  // Log the whole probe result, not just the winner. Which encoders this machine
  // actually offers is the input to PickH264Name, and without it a surprising
  // choice cannot be told apart from a missing candidate.
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] probe: " << buf;
  hw_h264_name_ = PickCodecName(
      buf, "H264", "DGJ_H264_ENCODER",
      {"h264_nvenc", "h264_amf", "h264_qsv"});
  if (H265Enabled() && dgj_vdec_available(DGJ_CODEC_H265) == 1) {
    hw_h265_name_ = PickCodecName(
        buf, "H265", "DGJ_H265_ENCODER",
        {"hevc_nvenc", "hevc_amf", "hevc_qsv"});
  }
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] hardware H264 encoder: '"
                   << (hw_h264_name_.empty() ? "none" : hw_h264_name_)
                   << "'";
  RTC_LOG(LS_INFO) << "[dgj-hwcodec] hardware H265 encoder: '"
                   << (hw_h265_name_.empty() ? "none" : hw_h265_name_)
                   << "'";
}

HwcodecVideoEncoderFactory::~HwcodecVideoEncoderFactory() = default;

std::vector<webrtc::SdpVideoFormat>
HwcodecVideoEncoderFactory::GetSupportedFormats() const {
  std::vector<webrtc::SdpVideoFormat> formats = builtin_->GetSupportedFormats();
  if (hw_h264_name_.empty() && hw_h265_name_.empty()) {
    // No hardware: identical to today's behavior (VP8 first). Do not append a
    // bare H264 -- OpenH264 Constrained Baseline would win negotiation and
    // look worse than VP8.
    return formats;
  }

  // UU's 1080p path uses H.265. Advertise it first only when this machine can
  // both encode and decode it; otherwise negotiation remains H.264/VPx.
  std::vector<webrtc::SdpVideoFormat> hw_first;
  if (!hw_h265_name_.empty()) {
    hw_first.emplace_back("H265");
  }
  if (!hw_h264_name_.empty()) {
    hw_first.emplace_back("H264", webrtc::CodecParameterMap{
                                      // The NVENC/QSV wrapper is configured for
                                      // H.264 Main (CABAC). Advertising
                                      // constrained-baseline here makes the
                                      // answer select PT108 even though the
                                      // encoder emits Main-profile SPS/PPS.
                                      {"profile-level-id", "4d001f"},
                                      {"packetization-mode", "1"},
                                      {"level-asymmetry-allowed", "1"},
                                  });
  }
  for (const auto& format : formats) {
    if ((IsH264Format(format) && !hw_h264_name_.empty()) ||
        (IsH265Format(format) && !hw_h265_name_.empty())) {
      continue;  // Hardware already advertised at the front.
    }
    hw_first.push_back(format);
  }
  return hw_first;
}

std::unique_ptr<webrtc::VideoEncoder> HwcodecVideoEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  if (IsH264Format(format) && !hw_h264_name_.empty()) {
    // Software wrapper around the same OpenH264 encoder the builtin factory
    // would produce, so a broken hardware encoder degrades to a working
    // stream rather than a black screen.
    return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
        env, builtin_->Create(env, format),
        std::make_unique<HwcodecVideoEncoder>(hw_h264_name_,
                                              webrtc::kVideoCodecH264),
        /*prefer_temporal_support=*/false);
  }
  if (IsH265Format(format) && !hw_h265_name_.empty()) {
    // There is no builtin software H.265 encoder in this WebRTC branch, so the
    // codec is only advertised after hardware probing and is used directly.
    return std::make_unique<HwcodecVideoEncoder>(hw_h265_name_,
                                                 webrtc::kVideoCodecH265);
  }
  return builtin_->Create(env, format);
}

}  // namespace libwebrtc
