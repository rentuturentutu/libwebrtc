#include <cstdlib>
#include <iostream>
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "src/internal/capture_i420_buffer.h"

static void Check(bool ok) { if (!ok) std::abort(); }
int main() {
  auto first = libwebrtc::WithCaptureFrameId(webrtc::I420Buffer::Create(8, 8), 1);
  const uint64_t id = (uint64_t{1} << 63) + 65537;
  auto second = libwebrtc::WithCaptureFrameId(webrtc::I420Buffer::Create(8, 8), id);
  Check(libwebrtc::CaptureFrameIdTag::Get(first.get()) == 1);
  Check(libwebrtc::CaptureFrameIdTag::Get(second.get()) == id);
  auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(second).build();
  auto copy = frame;
  auto scaled = copy.video_frame_buffer()->CropAndScale(0, 0, 8, 8, 4, 4);
  Check(libwebrtc::CaptureFrameIdTag::Get(scaled.get()) == id);
  const void* scaled_address = scaled.get();
  scaled = nullptr;
  Check(libwebrtc::CaptureFrameIdTag::Get(scaled_address) == 0);
  auto unset = webrtc::I420Buffer::Create(4, 4);
  Check(libwebrtc::CaptureFrameIdTag::Get(unset.get()) == 0);
  std::cout << "PASS full 64-bit id, simultaneous low16 collision, copy, scaling, lifetime cleanup\n";
}
