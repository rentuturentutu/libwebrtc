#include "libwebrtc/src/dgj/hwcodec_video_decoder.h"

#include <d3d11.h>

#include "api/video/video_frame.h"
#include "libwebrtc/src/dgj/dgj_encode_ffi.h"
#include "libwebrtc/src/dgj/dgj_texture_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace libwebrtc {
namespace {

void ReleaseDecodedTexture(void* opaque) {
  static_cast<ID3D11Texture2D*>(opaque)->Release();
}

}  // namespace

HwcodecVideoDecoder::HwcodecVideoDecoder(webrtc::VideoCodecType codec_type)
    : codec_type_(codec_type) {}

HwcodecVideoDecoder::~HwcodecVideoDecoder() {
  Release();
}

bool HwcodecVideoDecoder::Configure(const Settings& settings) {
  if (settings.codec_type() != codec_type_ ||
      (codec_type_ != webrtc::kVideoCodecH264 &&
       codec_type_ != webrtc::kVideoCodecH265)) {
    return false;
  }
  Release();
  const int codec = codec_type_ == webrtc::kVideoCodecH264 ? DGJ_CODEC_H264
                                                           : DGJ_CODEC_H265;
  const char* codec_name =
      codec_type_ == webrtc::kVideoCodecH264 ? "H264" : "H265";
  handle_ = dgj_vdec_create(codec);
  if (handle_ == nullptr) {
    RTC_LOG(LS_ERROR) << "[dgj-hwdecode] failed to open " << codec_name
                      << " decoder";
    return false;
  }
  RTC_LOG(LS_INFO) << "[dgj-hwdecode] " << codec_name
                   << " decoder started, backend="
                   << dgj_vdec_backend(handle_);
  return true;
}

int32_t HwcodecVideoDecoder::Decode(const webrtc::EncodedImage& input_image,
                                    int64_t /*render_time_ms*/) {
  if (handle_ == nullptr || callback_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (input_image.data() == nullptr || input_image.size() == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  current_rtp_timestamp_ = input_image.RtpTimestamp();
  if (const webrtc::ColorSpace* color = input_image.ColorSpace()) {
    current_color_space_ = *color;
  } else {
    current_color_space_.reset();
  }
  decode_start_ms_ = rtc::TimeMillis();
  decoded_in_call_ = 0;
  const int ret = dgj_vdec_decode(handle_, input_image.data(),
                                  input_image.size(),
                                  &HwcodecVideoDecoder::OnDecoded, this);
  if (ret != DGJ_OK) {
    RTC_LOG(LS_WARNING) << "[dgj-hwdecode] decode failed with " << ret
                        << ", bytes=" << input_image.size();
    // H264 is wrapped with WebRTC's software decoder. Fall back immediately
    // instead of waiting for several failed keyframes and showing black.
    return codec_type_ == webrtc::kVideoCodecH264
               ? WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE
               : WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwcodecVideoDecoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwcodecVideoDecoder::Release() {
  callback_ = nullptr;
  texture_pool_ = nullptr;
  if (handle_ != nullptr) {
    dgj_vdec_destroy(handle_);
    handle_ = nullptr;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo HwcodecVideoDecoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = codec_type_ == webrtc::kVideoCodecH264
                                 ? "dgj_hwcodec:h264_d3d11"
                                 : "dgj_hwcodec:h265_d3d11";
  info.is_hardware_accelerated = true;
  return info;
}

void HwcodecVideoDecoder::OnDecoded(void* opaque,
                                    int width,
                                    int height,
                                    void* user) {
  auto* self = static_cast<HwcodecVideoDecoder*>(user);
  auto* texture = static_cast<ID3D11Texture2D*>(opaque);
  if (self == nullptr || texture == nullptr || self->callback_ == nullptr ||
      width <= 0 || height <= 0) {
    return;
  }

  ID3D11Device* device = nullptr;
  texture->GetDevice(&device);
  if (device == nullptr) {
    return;
  }
  if (!self->texture_pool_ || self->texture_pool_->device() != device) {
    self->texture_pool_ = DgjTexturePool::Create(device, /*luid=*/0);
  }
  device->Release();
  if (!self->texture_pool_) {
    return;
  }

  // The hwcodec output ring owns the texture. Hold one COM reference until
  // WebRTC releases the VideoFrame so the ring cannot destroy it underneath
  // the renderer.
  texture->AddRef();
  rtc::scoped_refptr<DgjD3D11TextureBuffer> buffer =
      self->texture_pool_->Wrap(texture, nullptr, width, height, texture,
                                &ReleaseDecodedTexture);
  if (!buffer) {
    texture->Release();
    return;
  }

  webrtc::VideoFrame frame =
      webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(buffer)
          .set_rtp_timestamp(self->current_rtp_timestamp_)
          .set_color_space(self->current_color_space_)
          .set_rotation(webrtc::kVideoRotation_0)
          .build();
  ++self->decoded_in_call_;
  self->callback_->Decoded(
      frame, static_cast<int32_t>(rtc::TimeMillis() - self->decode_start_ms_),
      absl::nullopt);
}

}  // namespace libwebrtc
