#include "libwebrtc/src/dgj/hwcodec_video_decoder_factory.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

#include "absl/strings/match.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/video_decoder_software_fallback_wrapper.h"
#include "libwebrtc/src/dgj/dgj_encode_ffi.h"
#include "libwebrtc/src/dgj/hwcodec_video_decoder.h"
#include "rtc_base/logging.h"

namespace libwebrtc {
namespace {

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

bool H264HardwareDecodeEnabled() {
  char value[16] = {};
  size_t length = 0;
  if (getenv_s(&length, value, sizeof(value), "DGJ_H264_HWDEC") != 0 ||
      length == 0) {
    return true;
  }
  std::string setting(value);
  std::transform(setting.begin(), setting.end(), setting.begin(), ::tolower);
  return setting != "0" && setting != "false" && setting != "off";
}

bool IsH264(const webrtc::SdpVideoFormat& format) {
  return absl::EqualsIgnoreCase(format.name, "H264");
}

bool IsH265(const webrtc::SdpVideoFormat& format) {
  return absl::EqualsIgnoreCase(format.name, "H265");
}

}  // namespace

HwcodecVideoDecoderFactory::HwcodecVideoDecoderFactory()
    : builtin_(webrtc::CreateBuiltinVideoDecoderFactory()) {
  h264_available_ = H264HardwareDecodeEnabled() &&
                    dgj_vdec_available(DGJ_CODEC_H264) == 1;
  h265_available_ = H265Enabled() && dgj_vdec_available(DGJ_CODEC_H265) == 1;
  RTC_LOG(LS_INFO) << "[dgj-hwdecode] H264 "
                   << (h264_available_ ? "available" : "disabled/unavailable");
  RTC_LOG(LS_INFO) << "[dgj-hwdecode] H265 "
                   << (h265_available_ ? "available" : "disabled/unavailable");
}

HwcodecVideoDecoderFactory::~HwcodecVideoDecoderFactory() = default;

std::vector<webrtc::SdpVideoFormat>
HwcodecVideoDecoderFactory::GetSupportedFormats() const {
  std::vector<webrtc::SdpVideoFormat> formats = builtin_->GetSupportedFormats();
  if (h265_available_) {
    formats.insert(formats.begin(), webrtc::SdpVideoFormat("H265"));
  }
  return formats;
}

webrtc::VideoDecoderFactory::CodecSupport
HwcodecVideoDecoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    bool reference_scaling) const {
  if (IsH264(format) && h264_available_ && !reference_scaling) {
    return {/*is_supported=*/true, /*is_power_efficient=*/true};
  }
  if (IsH265(format)) {
    return {/*is_supported=*/h265_available_ && !reference_scaling,
            /*is_power_efficient=*/h265_available_};
  }
  return builtin_->QueryCodecSupport(format, reference_scaling);
}

std::unique_ptr<webrtc::VideoDecoder> HwcodecVideoDecoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  if (h264_available_ && IsH264(format)) {
    std::unique_ptr<webrtc::VideoDecoder> fallback =
        builtin_->Create(env, format);
    if (fallback) {
      return webrtc::CreateVideoDecoderSoftwareFallbackWrapper(
          env, std::move(fallback),
          std::make_unique<HwcodecVideoDecoder>(webrtc::kVideoCodecH264));
    }
  }
  if (h265_available_ && IsH265(format)) {
    return std::make_unique<HwcodecVideoDecoder>(webrtc::kVideoCodecH265);
  }
  return builtin_->Create(env, format);
}

}  // namespace libwebrtc
