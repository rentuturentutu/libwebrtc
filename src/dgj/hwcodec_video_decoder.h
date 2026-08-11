#ifndef LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_DECODER_H_
#define LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_DECODER_H_

#include <cstdint>

#include "api/scoped_refptr.h"
#include "api/video/color_space.h"
#include "api/video_codecs/video_decoder.h"

namespace libwebrtc {

class DgjTexturePool;

// Hardware H.264/H.265 decoder backed by dgj_encode/hwcodec. Decoded frames remain
// D3D11 textures through WebRTC and only fall back to I420 when a consumer
// explicitly requests it.
class HwcodecVideoDecoder final : public webrtc::VideoDecoder {
 public:
  explicit HwcodecVideoDecoder(webrtc::VideoCodecType codec_type);
  ~HwcodecVideoDecoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

 private:
  static void OnDecoded(void* texture,
                        int width,
                        int height,
                        void* user);

  void* handle_ = nullptr;
  webrtc::DecodedImageCallback* callback_ = nullptr;
  rtc::scoped_refptr<DgjTexturePool> texture_pool_;
  uint32_t current_rtp_timestamp_ = 0;
  absl::optional<webrtc::ColorSpace> current_color_space_;
  int64_t decode_start_ms_ = 0;
  int decoded_in_call_ = 0;
  const webrtc::VideoCodecType codec_type_;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_SRC_DGJ_HWCODEC_VIDEO_DECODER_H_
