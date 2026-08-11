#include "libwebrtc.h"

#include <cerrno>
#include <memory>
#include <string>

#if defined(WEBRTC_WIN)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "api/scoped_refptr.h"
#include "rtc_base/logging.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/field_trial.h"
#include "rtc_peerconnection_factory_impl.h"

namespace libwebrtc {

// Initialize static variable g_is_initialized to false.
static bool g_is_initialized = false;
static std::unique_ptr<rtc::FileRotatingLogSink> g_file_log_sink;

namespace {

// Default log directory, relative to the process working directory.
#if defined(WEBRTC_WIN)
constexpr char kDefaultLogDir[] = "logDir\\DeskRemote";
#else
constexpr char kDefaultLogDir[] = "logDir/DeskRemote";
#endif

// Tell a standard WebRTC receiver to render as soon as a frame is decodable
// instead of building a jitter buffer. The controlling side is not this SDK,
// so the playout-delay RTP header extension is the only available control.
// min_ms and max_ms must both be present for the sender override to apply.
//
// WebRTC-DgjForceLowLatency does the same thing on the receive side without
// depending on signalling: VCMTiming pins its playout delay range to [0, 0],
// which is what actually enables the low-latency render path. The extension
// alone proved insufficient -- measured jitterBufferDelay still ramped to
// ~100ms on a 5ms LAN because VCMTiming's default range is [0, 10s], well
// above the 500ms low-latency threshold.
constexpr char kDefaultFieldTrials[] =
    "WebRTC-ForceSendPlayoutDelay/min_ms:0,max_ms:0/"
    "WebRTC-DgjForceLowLatency/Enabled/"
    // Let a frame's packets leave the pacer quickly instead of being smeared
    // across most of a frame interval.
    //
    // video_send_stream_impl.cc:106 defaults kStrictPacingMultiplier to 1.1, and
    // for screenshare content the ALR path uses 1.0. At 1.1x the target bitrate
    // a ~25KB frame at 6Mbps needs ~30ms to drain -- almost a whole frame
    // interval -- which is exactly the measured arrival jitter: assembly
    // 0-18ms and receive interFrame spread +-11..27ms. That jitter is what
    // starves the controlling side's 2-vsync slots: the average source period
    // was already a safe 31ms, yet pull gap still drifted to 39ms in some
    // windows, and the windows with the smallest interFrame spread (+-11ms)
    // were exactly the ones with the best display rate.
    //
    // 10.0 -> 3.0, now that this value is actually honoured.
    //
    // Both earlier settings (3.0, then 10.0) were dead code:
    // GetConfiguredPacingFactor returned the ALR screenshare default of 1
    // before ever reading this trial, and the [dgj] log line printed
    // "configured = 1" both times. video_send_stream_impl.cc now checks an
    // explicitly configured factor first, so from this build on the number
    // below is real.
    //
    // What factor 1 cost us: the pacer emitted at exactly the target bitrate
    // with zero burst headroom, so packets sat waiting for budget. Measured
    // totalPacketSendDelay/packetsSent on the sending side was 37.8ms average
    // with a 70.0ms peak -- the largest single term in the ~99ms end-to-end
    // budget, and bigger than everything else we spent rounds optimizing.
    //
    // Why not stay at 10: factor 10 is effectively unpaced, the maximum
    // burst-loss risk on a constrained WAN. 3.0 predicts sendDelay around 12ms,
    // and the queue time limit tightened from 250ms to 30ms in
    // video_send_stream_impl.cc is the closed-loop backstop on the same
    // quantity. Raise this only if sendDelay stays high while lost/nack are
    // still zero.
    "WebRTC-Video-Pacing/factor:3.0/";


// Creates a single directory level. Returns true if the directory exists
// afterwards, including the case where it already existed.
bool CreateDirectoryLevel(const std::string& path) {
  if (path.empty()) {
    return true;
  }
#if defined(WEBRTC_WIN)
  if (_mkdir(path.c_str()) == 0) {
    return true;
  }
#else
  if (::mkdir(path.c_str(), 0755) == 0) {
    return true;
  }
#endif
  return errno == EEXIST;
}

// Creates every level of `path`. Accepts both '/' and '\' separators so callers
// can pass either style. rtc_base offers no reusable helper for this: the
// filesystem functions in file_rotating_stream.cc live in an anonymous
// namespace, and <filesystem> is not used in this code base.
bool CreateDirectoryRecursive(const std::string& input) {
  // Drop trailing separators: _mkdir()/mkdir() on a path ending in one is not
  // portable, and the caller may well pass "some/dir/".
  std::string path = input;
  while (path.size() > 1 &&
         (path.back() == '/' || path.back() == '\\') &&
         path[path.size() - 2] != ':') {
    path.pop_back();
  }
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '/' && path[i] != '\\') {
      continue;
    }
    // Skip the root of an absolute path and the "C:\" style drive root; neither
    // can be created.
    if (i == 0 || path[i - 1] == ':') {
      continue;
    }
    if (!CreateDirectoryLevel(path.substr(0, i))) {
      return false;
    }
  }
  return CreateDirectoryLevel(path);
}

}  // namespace

// Initializes SSL, if not initialized.
bool LibWebRTC::Initialize() {
  if (!g_is_initialized) {
    rtc::InitializeSSL();

    // Enable file logging by default so field issues stay diagnosable without
    // the application opting in. The path is relative, i.e. resolved against
    // the process working directory rather than the executable directory.
    // Applications needing a different location can call EnableFileLog() before
    // Initialize(); the first caller wins. The return value is deliberately
    // ignored -- missing logs must not prevent the SDK from starting.
    EnableFileLog(kDefaultLogDir, /*severity=*/1, /*max_log_size_bytes=*/0,
                  /*num_files=*/0);

    // Set the field trials after the log sink is installed so that the string
    // InitFieldTrialsFromString() logs ends up in the file, and before any
    // PeerConnectionFactory exists so that RTPSenderVideo picks it up.
    // InitFieldTrialsFromString() replaces the complete string, so preserve an
    // application-provided configuration instead of silently dropping it.
    if (webrtc::field_trial::GetFieldTrialString() == nullptr) {
      webrtc::field_trial::InitFieldTrialsFromString(kDefaultFieldTrials);
    }
    g_is_initialized = true;
  }
  return g_is_initialized;
}


// Stops and cleans up the threads and SSL.
void LibWebRTC::Terminate() {
  rtc::ThreadManager::Instance()->SetCurrentThread(NULL);
  rtc::CleanupSSL();

  if (g_file_log_sink) {
    rtc::LogMessage::RemoveLogToStream(g_file_log_sink.get());
    g_file_log_sink.reset();
  }

  // Resets the static variable g_is_initialized to false.
  g_is_initialized = false;
}

bool LibWebRTC::EnableFileLog(const char* dir,
                              int severity,
                              int max_log_size_bytes,
                              int num_files) {
  if (g_file_log_sink) {
    return true;
  }
  if (!dir || !*dir) {
    return false;
  }
  // FileRotatingStream requires the directory to already exist, so create it
  // here instead of making every caller do it.
  if (!CreateDirectoryRecursive(dir)) {
    return false;
  }
  if (max_log_size_bytes <= 0) max_log_size_bytes = 10 * 1024 * 1024;
  if (num_files <= 0) num_files = 5;
  rtc::LoggingSeverity sev = rtc::LS_INFO;
  switch (severity) {
    case 0: sev = rtc::LS_VERBOSE; break;
    case 1: sev = rtc::LS_INFO; break;
    case 2: sev = rtc::LS_WARNING; break;
    case 3: sev = rtc::LS_ERROR; break;
    default: sev = rtc::LS_INFO; break;
  }
  auto sink = std::make_unique<rtc::FileRotatingLogSink>(
      dir, "webrtc_log", static_cast<size_t>(max_log_size_bytes),
      static_cast<size_t>(num_files));
  if (!sink->Init()) {
    return false;
  }
  rtc::LogMessage::AddLogToStream(sink.get(), sev);
  rtc::LogMessage::LogTimestamps(true);
  rtc::LogMessage::LogThreads(true);
  g_file_log_sink = std::move(sink);
  return true;
}

void LibWebRTC::InitFieldTrials(const char* trials) {
  if (trials && *trials) {
    webrtc::field_trial::InitFieldTrialsFromString(trials);
  }
}

// Creates and returns an instance of RTCPeerConnectionFactory.
scoped_refptr<RTCPeerConnectionFactory>
LibWebRTC::CreateRTCPeerConnectionFactory() {
  scoped_refptr<RTCPeerConnectionFactory> rtc_peerconnection_factory =
      scoped_refptr<RTCPeerConnectionFactory>(
          new RefCountedObject<RTCPeerConnectionFactoryImpl>());
  rtc_peerconnection_factory->Initialize();
  return rtc_peerconnection_factory;
}

}  // namespace libwebrtc
