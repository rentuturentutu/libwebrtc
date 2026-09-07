#include "rtc_video_frame_impl.h"

#include <dxgi.h>

#include "api/video/i420_buffer.h"
#include "libwebrtc/src/dgj/dgj_texture_buffer.h"
#include "libyuv/convert_argb.h"
#include "libyuv/convert_from.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

namespace libwebrtc {

VideoFrameBufferImpl::VideoFrameBufferImpl(
    rtc::scoped_refptr<webrtc::VideoFrameBuffer> frame_buffer)
    : buffer_(frame_buffer) {}

VideoFrameBufferImpl::VideoFrameBufferImpl(
    rtc::scoped_refptr<webrtc::I420Buffer> frame_buffer)
    : buffer_(frame_buffer) {}

VideoFrameBufferImpl::~VideoFrameBufferImpl() {}

scoped_refptr<RTCVideoFrame> VideoFrameBufferImpl::Copy() {
  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(buffer_));
  frame->set_timestamp_us(timestamp_us_);
#if defined(_WIN32)
  frame->set_capture_time_ms(capture_time_ms_);
#endif
  frame->set_rotation(rotation_);
  return frame;
}

int VideoFrameBufferImpl::width() const { return buffer_->width(); }

int VideoFrameBufferImpl::height() const { return buffer_->height(); }

const uint8_t* VideoFrameBufferImpl::DataY() const {
  return buffer_->GetI420()->DataY();
}

const uint8_t* VideoFrameBufferImpl::DataU() const {
  return buffer_->GetI420()->DataU();
}

const uint8_t* VideoFrameBufferImpl::DataV() const {
  return buffer_->GetI420()->DataV();
}

int VideoFrameBufferImpl::StrideY() const {
  return buffer_->GetI420()->StrideY();
}

int VideoFrameBufferImpl::StrideU() const {
  return buffer_->GetI420()->StrideU();
}

int VideoFrameBufferImpl::StrideV() const {
  return buffer_->GetI420()->StrideV();
}

void* VideoFrameBufferImpl::D3D11SharedHandle() const {
  if (!buffer_ || buffer_->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    return nullptr;
  }

  // DGJ is the only native VideoFrameBuffer producer in this Windows build.
  // Its decode/capture textures are BGRA and D3D11_RESOURCE_MISC_SHARED.
  auto* native = static_cast<DgjD3D11TextureBuffer*>(buffer_.get());
  ID3D11Texture2D* texture = native->texture();
  if (texture == nullptr) {
    return nullptr;
  }
  IDXGIResource* resource = nullptr;
  if (FAILED(texture->QueryInterface(
          __uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) ||
      resource == nullptr) {
    return nullptr;
  }
  HANDLE handle = nullptr;
  const HRESULT result = resource->GetSharedHandle(&handle);
  resource->Release();
  return SUCCEEDED(result) ? handle : nullptr;
}

void* VideoFrameBufferImpl::D3D11Texture() const {
  if (!buffer_ || buffer_->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    return nullptr;
  }
  auto* native = static_cast<DgjD3D11TextureBuffer*>(buffer_.get());
  return native->texture();
}

void* VideoFrameBufferImpl::D3D11Device() const {
  if (!buffer_ || buffer_->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    return nullptr;
  }
  auto* native = static_cast<DgjD3D11TextureBuffer*>(buffer_.get());
  ID3D11Texture2D* texture = native->texture();
  ID3D11Device* device = native->device();
  if (texture == nullptr || device == nullptr) {
    return nullptr;
  }
  ID3D11Device* texture_device = nullptr;
  texture->GetDevice(&texture_device);
  const bool matches = texture_device == device;
  if (texture_device != nullptr) {
    texture_device->Release();
  }
  return matches ? device : nullptr;
}

int VideoFrameBufferImpl::ConvertToARGB(Type type, uint8_t* dst_buffer,
                                        int dst_stride, int dest_width,
                                        int dest_height) {
  rtc::scoped_refptr<webrtc::I420Buffer> i420 =
      webrtc::I420Buffer::Rotate(*buffer_.get(), rotation_);

  rtc::scoped_refptr<webrtc::I420Buffer> dest =
      webrtc::I420Buffer::Create(dest_width, dest_height);

  dest->ScaleFrom(*i420.get());
  int buf_size = dest->width() * dest->height() * (32 >> 3);
  switch (type) {
    case libwebrtc::RTCVideoFrame::Type::kARGB:
      libyuv::I420ToARGB(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    case libwebrtc::RTCVideoFrame::Type::kBGRA:
      libyuv::I420ToBGRA(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    case libwebrtc::RTCVideoFrame::Type::kABGR:
      libyuv::I420ToABGR(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    case libwebrtc::RTCVideoFrame::Type::kRGBA:
      libyuv::I420ToRGBA(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    default:
      break;
  }
  return buf_size;
}

libwebrtc::RTCVideoFrame::VideoRotation VideoFrameBufferImpl::rotation() {
  switch (rotation_) {
    case webrtc::kVideoRotation_0:
      return RTCVideoFrame::kVideoRotation_0;
    case webrtc::kVideoRotation_90:
      return RTCVideoFrame::kVideoRotation_90;
    case webrtc::kVideoRotation_180:
      return RTCVideoFrame::kVideoRotation_180;
    case webrtc::kVideoRotation_270:
      return RTCVideoFrame::kVideoRotation_270;
    default:
      break;
  }
  return RTCVideoFrame::kVideoRotation_0;
}

scoped_refptr<RTCVideoFrame> RTCVideoFrame::Create(int width, int height,
                                                   const uint8_t* buffer,
                                                   int length) {
  int stride_y = width;
  int stride_uv = (width + 1) / 2;

  int size_y = stride_y * height;
  int size_u = stride_uv * height / 2;
  // int size_v = size_u;

  RTC_DCHECK(length == (width * height * 3) / 2);

  const uint8_t* data_y = buffer;
  const uint8_t* data_u = buffer + size_y;
  const uint8_t* data_v = buffer + size_y + size_u;

  rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_uv, data_v, stride_uv);

  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(i420_buffer));
  return frame;
}

scoped_refptr<RTCVideoFrame> RTCVideoFrame::Create(
    int width, int height, const uint8_t* data_y, int stride_y,
    const uint8_t* data_u, int stride_u, const uint8_t* data_v, int stride_v) {
  rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_u, data_v, stride_v);

  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(i420_buffer));
  return frame;
}

}  // namespace libwebrtc
