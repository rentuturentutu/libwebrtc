#ifndef LIB_WEBRTC_HXX
#define LIB_WEBRTC_HXX

#include "rtc_peerconnection_factory.h"
#include "rtc_types.h"

namespace libwebrtc {

/**
 * @class LibWebRTC
 * @brief Provides static methods for initializing, creating and terminating
 * the WebRTC PeerConnectionFactory and threads.
 *
 * This class provides static methods for initializing, creating and terminating
 * the WebRTC PeerConnectionFactory and threads. These methods are thread-safe
 * and can be called from any thread. This class is not meant to be
 * instantiated.
 *
 */
class LibWebRTC {
 public:
  /**
   * @brief Initializes the WebRTC PeerConnectionFactory and threads.
   *
   * Initializes the WebRTC PeerConnectionFactory and threads. This method is
   * thread-safe and can be called from any thread. It initializes SSL and
   * creates three threads: worker_thread, signaling_thread and network_thread.
   *
   * Also enables file logging by default into "logDir/DeskRemote" relative to
   * the process working directory. Call EnableFileLog() before this method to
   * choose a different location.
   *
   * When no field-trial string has been installed before initialization, the
   * SDK enables the sender-side RTP playout-delay override with min_ms=0 and
   * max_ms=0. This requests that a standard WebRTC receiver render a frame as
   * soon as it is decodable. Call InitFieldTrials() before Initialize() to
   * provide an application-owned field-trial string instead; the SDK does not
   * merge its default into an application string.

   *
   * @return true if initialization is successful, false otherwise.
   */
  LIB_WEBRTC_API static bool Initialize();

  /**
   * @brief Creates a new WebRTC PeerConnectionFactory.
   *
   * Creates a new WebRTC PeerConnectionFactory. This method is thread-safe and
   * can be called from any thread. It creates a new instance of the
   * RTCPeerConnectionFactoryImpl class and initializes it.
   *
   * @return A scoped_refptr object that points to the newly created
   * RTCPeerConnectionFactory.
   */
  LIB_WEBRTC_API static scoped_refptr<RTCPeerConnectionFactory>
  CreateRTCPeerConnectionFactory();

  /**
   * @brief Terminates the WebRTC PeerConnectionFactory and threads.
   *
   * Terminates the WebRTC PeerConnectionFactory and threads. This method is
   * thread-safe and can be called from any thread. It cleans up SSL and stops
   * and destroys the three threads: worker_thread, signaling_thread and
   * network_thread.
   *
   */
  LIB_WEBRTC_API static void Terminate();

  /**
   * @brief Enable RTC_LOG sink to a rotating file in `dir`.
   * Severity: 0=VERBOSE, 1=INFO, 2=WARNING, 3=ERROR.
   * `dir` is created if missing, including intermediate levels, and may use
   * either '/' or '\' separators. Relative paths resolve against the process
   * working directory. Log files are named "webrtc_log_0" .. "webrtc_log_N",
   * with index 0 being the newest; existing ones are deleted on open.
   * Pass 0 for `max_log_size_bytes` or `num_files` to use the defaults of
   * 10 MB and 5 files.
   * Initialize() already enables this for "logDir/DeskRemote", so call this
   * before Initialize() to override the location; the first caller wins.
   * Returns true on success.
   */
  LIB_WEBRTC_API static bool EnableFileLog(const char* dir,
                                           int severity,
                                           int max_log_size_bytes,
                                           int num_files);

  /**
   * @brief Inject WebRTC field trials.
   *
   * Must be called before Initialize() or before any RTCPeerConnection is
   * created. The supplied string replaces the complete field-trial
   * configuration; it is not merged with the SDK defaults. The caller must
   * keep the string storage alive for as long as WebRTC may read it.

   */
  LIB_WEBRTC_API static void InitFieldTrials(const char* trials);
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_HXX
