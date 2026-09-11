/*
 * HwcodecVideoEncoderFactory -- VideoEncoderFactory that prefers hardware H264
 * (via the dgj_encode Rust library) when this machine actually has a working
 * hardware encoder, and is indistinguishable from the builtin factory when it
 * does not.
 *
 * That dual behavior is deliberate: a machine without working hardware must
 * not advertise H264, because SDP would then negotiate H264 and land on
 * OpenH264 Constrained Baseline, which is worse than the VP8 we ship today
 * (see media/engine/internal_encoder_factory.cc for the builtin baseline).
 */
#ifndef LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_ENCODER_FACTORY_H_
#define LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_ENCODER_FACTORY_H_

#include <memory>
#include <string>
#include <vector>

#include "api/video_codecs/video_encoder_factory.h"

namespace libwebrtc {

class HwcodecVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  HwcodecVideoEncoderFactory();
  ~HwcodecVideoEncoderFactory() override;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
  CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
      absl::optional<std::string> scalability_mode) const override;
  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

 private:
  std::unique_ptr<webrtc::VideoEncoderFactory> builtin_;
  // Non-empty iff the one-shot probe in the constructor found working hardware
  // H264 (e.g. "h264_qsv"). Empty means "no hardware; behave exactly like the
  // builtin factory".
  std::string hw_h264_name_;
  std::string hw_h265_name_;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_ENCODER_FACTORY_H_
