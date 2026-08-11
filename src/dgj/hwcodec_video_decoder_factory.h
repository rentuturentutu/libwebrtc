#ifndef LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_DECODER_FACTORY_H_
#define LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_DECODER_FACTORY_H_

#include <memory>
#include <vector>

#include "api/video_codecs/video_decoder_factory.h"

namespace libwebrtc {

class HwcodecVideoDecoderFactory final : public webrtc::VideoDecoderFactory {
 public:
  HwcodecVideoDecoderFactory();
  ~HwcodecVideoDecoderFactory() override;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
  CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                 bool reference_scaling) const override;
  std::unique_ptr<webrtc::VideoDecoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> builtin_;
  bool h264_available_ = false;
  bool h265_available_ = false;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_DECODER_FACTORY_H_
