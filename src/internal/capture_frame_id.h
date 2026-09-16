#ifndef LIBWEBRTC_INTERNAL_CAPTURE_FRAME_ID_H_
#define LIBWEBRTC_INTERNAL_CAPTURE_FRAME_ID_H_

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace libwebrtc {
// Sender-local diagnostics. Entries live exactly as long as their owning
// buffer; the table owns no frame/texture references and cannot exhaust a pool.
class CaptureFrameIdTag {
 public:
  CaptureFrameIdTag(const void* buffer, uint64_t id) : buffer_(buffer) {
    if (id) {
      auto& registry = Registry();
      std::lock_guard<std::mutex> lock(registry.mutex);
      registry.ids[buffer_] = id;
    }
  }
  ~CaptureFrameIdTag() {
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.ids.erase(buffer_);
  }
  CaptureFrameIdTag(const CaptureFrameIdTag&) = delete;
  CaptureFrameIdTag& operator=(const CaptureFrameIdTag&) = delete;
  static uint64_t Get(const void* buffer) {
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.ids.find(buffer);
    return it == registry.ids.end() ? 0 : it->second;
  }
 private:
  struct State {
    std::mutex mutex;
    std::unordered_map<const void*, uint64_t> ids;
  };
  static State& Registry() {
    // Buffer destruction may run during process teardown.
    static State* const state = new State;
    return *state;
  }
  const void* const buffer_;
};
} // namespace libwebrtc
#endif
