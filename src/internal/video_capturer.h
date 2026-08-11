/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#ifndef INTERNAL_VIDEO_CAPTURER_H_
#define INTERNAL_VIDEO_CAPTURER_H_

#include <stddef.h>

#include <memory>

#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "api/video_track_source_constraints.h"
#include "media/base/video_adapter.h"
#include "media/base/video_broadcaster.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"

namespace webrtc {
namespace internal {

class VideoCapturer : public rtc::VideoSourceInterface<VideoFrame> {
 public:
  VideoCapturer();
  virtual ~VideoCapturer();

  virtual bool StartCapture() { return false; }

  virtual bool CaptureStarted() { return false; }

  virtual void StopCapture() {}

  void AddOrUpdateSink(rtc::VideoSinkInterface<VideoFrame>* sink,
                       const rtc::VideoSinkWants& wants) override;
  void RemoveSink(rtc::VideoSinkInterface<VideoFrame>* sink) override;

  // Forwards source constraints to every attached sink. Reporting
  // {min_fps = 0, max_fps > 0} is what lets the encoder-side frame cadence
  // adapter enter zero-hertz mode for screen content, so identical frames are
  // not re-encoded while the desktop is idle. See
  // FrameCadenceAdapterImpl::IsZeroHertzScreenshareEnabled().
  void ProcessConstraints(const VideoTrackSourceConstraints& constraints);

 protected:
  void OnFrame(const VideoFrame& frame);
  rtc::VideoSinkWants GetSinkWants();

  // Called whenever the sinks' aggregated max framerate changes. The base class
  // only feeds it to `video_adapter_`, which drops surplus frames inside
  // OnFrame -- by then a grabber has already paid for the acquire and the colour
  // conversion. A subclass that owns the grabber can override this to throttle
  // it at the source instead.
  //
  // `fps` is rtc::VideoSinkWants::max_framerate_fps, so it is
  // std::numeric_limits<int>::max() when nothing constrains the rate.
  virtual void OnMaxFramerateChanged(int fps) {}

 private:
  void UpdateVideoAdapter();

  rtc::VideoBroadcaster broadcaster_;
  cricket::VideoAdapter video_adapter_;
};
}  // namespace internal
}  // namespace webrtc

#endif  // INTERNAL_VIDEO_CAPTURER_H_
