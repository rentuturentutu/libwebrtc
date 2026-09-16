#ifndef LIBWEBRTC_INTERNAL_CAPTURE_I420_BUFFER_H_
#define LIBWEBRTC_INTERNAL_CAPTURE_I420_BUFFER_H_

#include "api/video/video_frame_buffer.h"
#include "rtc_base/ref_counted_object.h"
#include "src/internal/capture_frame_id.h"

namespace libwebrtc {
inline rtc::scoped_refptr<webrtc::I420BufferInterface> WithCaptureFrameId(
    rtc::scoped_refptr<webrtc::I420BufferInterface> buffer, uint64_t id);

// Delegate pixels without copying. Scaling creates another tagged buffer so
// diagnostics survive adaptation as well as the encoder's asynchronous queue.
class CaptureI420Buffer : public webrtc::I420BufferInterface {
 public:
  CaptureI420Buffer(rtc::scoped_refptr<webrtc::I420BufferInterface> buffer,
                    uint64_t id) : buffer_(std::move(buffer)), tag_(this, id) {}
  int width() const override { return buffer_->width(); }
  int height() const override { return buffer_->height(); }
  const uint8_t* DataY() const override { return buffer_->DataY(); }
  const uint8_t* DataU() const override { return buffer_->DataU(); }
  const uint8_t* DataV() const override { return buffer_->DataV(); }
  int StrideY() const override { return buffer_->StrideY(); }
  int StrideU() const override { return buffer_->StrideU(); }
  int StrideV() const override { return buffer_->StrideV(); }
  rtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(
      int x, int y, int w, int h, int out_w, int out_h) override {
    return WithCaptureFrameId(
        buffer_->CropAndScale(x, y, w, h, out_w, out_h)->ToI420(),
        CaptureFrameIdTag::Get(this));
  }
 protected:
  ~CaptureI420Buffer() override = default;
 private:
  rtc::scoped_refptr<webrtc::I420BufferInterface> buffer_;
  CaptureFrameIdTag tag_;
};

inline rtc::scoped_refptr<webrtc::I420BufferInterface> WithCaptureFrameId(
    rtc::scoped_refptr<webrtc::I420BufferInterface> buffer, uint64_t id) {
  if (!buffer || !id) return buffer;
  return rtc::scoped_refptr<webrtc::I420BufferInterface>(
      new rtc::RefCountedObject<CaptureI420Buffer>(std::move(buffer), id));
}
} // namespace libwebrtc
#endif
