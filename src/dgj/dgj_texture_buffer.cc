#include "src/dgj/dgj_texture_buffer.h"

#include <limits>

#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"

namespace libwebrtc {
namespace {

// RTC_LOG cannot take std::hex (its LogStreamer only accepts value types, and
// passing the manipulator trips -Wmicrosoft-cast under /WX), so HRESULTs are
// formatted here.
std::string HexHr(HRESULT hr) {
  char buf[16] = {};
  snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
  return std::string(buf);
}

}  // namespace


// ---------------------------------------------------------------------------
// DgjD3D11TextureBuffer
// ---------------------------------------------------------------------------

DgjD3D11TextureBuffer::DgjD3D11TextureBuffer(
    rtc::scoped_refptr<DgjTexturePool> pool,
    ID3D11Texture2D* texture,
    void* shared_handle,
    void* lease,
    void (*release)(void*),
    int64_t luid,
    int width,
    int height)
    : pool_(std::move(pool)),
      texture_(texture),
      shared_handle_(shared_handle),
      lease_(lease),
      release_(release),
      luid_(luid),
      width_(width),
      height_(height) {}

DgjD3D11TextureBuffer::~DgjD3D11TextureBuffer() {
  release_(lease_);
}

ID3D11Device* DgjD3D11TextureBuffer::device() const {
  return pool_->device();
}

rtc::scoped_refptr<webrtc::I420BufferInterface>
DgjD3D11TextureBuffer::ToI420() {
  rtc::scoped_refptr<webrtc::I420Buffer> out =
      webrtc::I420Buffer::Create(width_, height_);
  if (!pool_->DownloadToI420(texture_, array_slice_, width_, height_,
                             out->MutableDataY(), out->StrideY(),
                             out->MutableDataU(), out->StrideU(),
                             out->MutableDataV(), out->StrideV())) {
    // A black frame is a poor result, but returning null here would crash
    // callers that treat ToI420 as infallible (I420Buffer::Create never fails,
    // so much of WebRTC assumes this cannot).
    webrtc::I420Buffer::SetBlack(out.get());
  }
  return WithCaptureFrameId(out, CaptureFrameIdTag::Get(this));
}

// ---------------------------------------------------------------------------
// DgjTexturePool
// ---------------------------------------------------------------------------

rtc::scoped_refptr<DgjTexturePool> DgjTexturePool::Create(void* device,
                                                          int64_t luid) {
  if (device == nullptr) {
    RTC_LOG(LS_ERROR) << "[dgj-texpool] null device";
    return nullptr;
  }
  ID3D11Device* d3d = static_cast<ID3D11Device*>(device);
  ID3D11DeviceContext* context = nullptr;
  d3d->GetImmediateContext(&context);
  if (context == nullptr) {
    RTC_LOG(LS_ERROR) << "[dgj-texpool] GetImmediateContext returned null";
    return nullptr;
  }
  d3d->AddRef();
  return rtc::scoped_refptr<DgjTexturePool>(
      new rtc::RefCountedObject<DgjTexturePool>(d3d, context, luid));
}

DgjTexturePool::DgjTexturePool(ID3D11Device* device,
                              ID3D11DeviceContext* context,
                              int64_t luid)
    : device_(device), context_(context), luid_(luid) {
  RTC_LOG(LS_INFO) << "[dgj-texpool] created for luid " << luid;
}

DgjTexturePool::~DgjTexturePool() {
  if (staging_ != nullptr) {
    staging_->Release();
  }
  if (context_ != nullptr) {
    context_->Release();
  }
  if (device_ != nullptr) {
    device_->Release();
  }
  RTC_LOG(LS_INFO) << "[dgj-direct-texture] destroyed, wrapped=" << wrapped_;
}

rtc::scoped_refptr<DgjD3D11TextureBuffer> DgjTexturePool::Wrap(void* src,
                                                               void* shared_handle,
                                                               int width,
                                                               int height,
                                                               void* lease,
                                                               void (*release)(void*)) {
  if (src == nullptr || width <= 0 || height <= 0 || lease == nullptr ||
      release == nullptr) {
    return nullptr;
  }
  ID3D11Texture2D* source = static_cast<ID3D11Texture2D*>(src);
  D3D11_TEXTURE2D_DESC src_desc = {};
  source->GetDesc(&src_desc);

  std::lock_guard<std::mutex> guard(lock_);

  if (src_desc.Width != static_cast<UINT>(width_) ||
      src_desc.Height != static_cast<UINT>(height_) ||
      src_desc.Format != format_) {
    if (staging_ != nullptr) {
      staging_->Release();
      staging_ = nullptr;
    }
  }
  width_ = static_cast<int>(src_desc.Width);
  height_ = static_cast<int>(src_desc.Height);
  format_ = src_desc.Format;

  ++wrapped_;
  return rtc::scoped_refptr<DgjD3D11TextureBuffer>(
      new rtc::RefCountedObject<DgjD3D11TextureBuffer>(
          rtc::scoped_refptr<DgjTexturePool>(this), source, shared_handle,
          lease, release, luid_, width, height));
}

bool DgjTexturePool::DownloadToI420(ID3D11Texture2D* texture,
                                    int array_slice,
                                    int width,
                                    int height,
                                    uint8_t* dst_y,
                                    int stride_y,
                                    uint8_t* dst_u,
                                    int stride_u,
                                    uint8_t* dst_v,
                                    int stride_v) {
  if (texture == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(lock_);

  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  if (desc.Width != static_cast<UINT>(width) ||
      desc.Height != static_cast<UINT>(height)) {
    RTC_LOG(LS_ERROR) << "[dgj-texpool] texture geometry mismatch actual="
                      << desc.Width << "x" << desc.Height << " frame=" << width
                      << "x" << height;
    return false;
  }
  if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
      desc.Format != DXGI_FORMAT_NV12) {
    RTC_LOG(LS_ERROR) << "[dgj-texpool] unsupported texture format "
                      << static_cast<int>(desc.Format);
    return false;
  }
  // Captured before `desc` is reshaped into the staging description below.
  const DXGI_FORMAT src_format = desc.Format;
  const UINT src_array_size = desc.ArraySize;
  const UINT src_mip_levels = desc.MipLevels;
  const UINT src_height = desc.Height;
  if (staging_ != nullptr) {
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_->GetDesc(&staging_desc);
    if (staging_desc.Width != desc.Width ||
        staging_desc.Height != desc.Height ||
        staging_desc.Format != desc.Format) {
      staging_->Release();
      staging_ = nullptr;
    }
  }
  if (staging_ == nullptr) {
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
    if (FAILED(hr) || staging_ == nullptr) {
      RTC_LOG(LS_ERROR) << "[dgj-texpool] staging CreateTexture2D failed "
                        << HexHr(hr);
      staging_ = nullptr;
      return false;
    }
  }

  // A hardware decode pool hands out one slice of a texture array, so the whole
  // resource cannot be copied: sizes would not match and the wrong slice would
  // be read.
  if (src_array_size > 1 || array_slice > 0) {
    if (array_slice < 0 || static_cast<UINT>(array_slice) >= src_array_size) {
      RTC_LOG(LS_ERROR) << "[dgj-texpool] array slice " << array_slice
                        << " out of range " << src_array_size;
      return false;
    }
    context_->CopySubresourceRegion(
        staging_, 0, 0, 0, 0, texture,
        D3D11CalcSubresource(0, static_cast<UINT>(array_slice), src_mip_levels),
        nullptr);
  } else {
    context_->CopyResource(staging_, texture);
  }
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "[dgj-texpool] Map failed " << HexHr(hr);
    return false;
  }
  const uint64_t bytes_per_row =
      src_format == DXGI_FORMAT_NV12 ? static_cast<uint64_t>(width)
                                     : static_cast<uint64_t>(width) * 4u;
  if (mapped.pData == nullptr || mapped.RowPitch < bytes_per_row ||
      mapped.RowPitch > static_cast<UINT>((std::numeric_limits<int>::max)())) {
    RTC_LOG(LS_ERROR) << "[dgj-texpool] invalid mapped layout format="
                      << static_cast<int>(src_format)
                      << " row_pitch=" << mapped.RowPitch
                      << " minimum=" << bytes_per_row
                      << " data=" << mapped.pData;
    context_->Unmap(staging_, 0);
    return false;
  }
  // libyuv's "ARGB" is B,G,R,A in memory order, which is what
  // DXGI_FORMAT_B8G8R8A8_UNORM holds. An NV12 staging map is the Y plane
  // followed by the interleaved UV plane, both at RowPitch, with UV starting one
  // full Y plane in.
  int result = 0;
  if (src_format == DXGI_FORMAT_NV12) {
    const auto* y = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t* uv = y + static_cast<size_t>(mapped.RowPitch) * src_height;
    result = libyuv::NV12ToI420(y, static_cast<int>(mapped.RowPitch), uv,
                                static_cast<int>(mapped.RowPitch), dst_y,
                                stride_y, dst_u, stride_u, dst_v, stride_v,
                                width, height);
  } else {
    result = libyuv::ARGBToI420(
        static_cast<const uint8_t*>(mapped.pData),
        static_cast<int>(mapped.RowPitch), dst_y, stride_y, dst_u, stride_u,
        dst_v, stride_v, width, height);
  }
  context_->Unmap(staging_, 0);
  if (result != 0) {
    RTC_LOG(LS_ERROR) << "[dgj-texpool] to-I420 conversion failed " << result
                      << " format=" << static_cast<int>(src_format);
    return false;
  }
  return true;
}

}  // namespace libwebrtc
