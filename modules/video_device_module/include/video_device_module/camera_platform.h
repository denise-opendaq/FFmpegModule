/*
 * Copyright 2022-2026 openDAQ d.o.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <video_device_module/common.h>

#include <string>
#include <vector>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

// Name of the libavdevice input format used to capture from a camera on the
// current platform (e.g. "v4l2", "avfoundation", "dshow").
const char* cameraInputFormatName();

struct CameraDeviceEntry
{
    // Identifier to pass as the device path to CameraDriver::open() / CameraDeviceImpl::CreateDeviceInfo().
    std::string path;
    // Human-readable device name.
    std::string name;
};

// Enumerates cameras available on the current platform. Implemented per-OS:
// libavdevice's source listing on Linux/Windows (v4l2/dshow), native AVFoundation on macOS
// (libavdevice's avfoundation backend does not implement programmatic device listing).
std::vector<CameraDeviceEntry> listCameraDevices();

// Assigns each entry a connection-string-safe id derived from its friendly name, unique within
// the list (a numeric suffix is appended for repeats — e.g. two identical camera models both
// named "Integrated Webcam" enumerate as "Integrated_Webcam" and "Integrated_Webcam_2"). Local
// devices are addressed by this id rather than their raw OS device path, since paths on some
// platforms (e.g. Windows dshow's "@device_pnp_\\?\usb#...") contain '?'/'#'/'&' characters that
// collide with openDAQ's own connection-string option syntax.
std::vector<std::string> assignUniqueDeviceIds(const std::vector<CameraDeviceEntry>& entries);

// True if `path` is a network stream URL (e.g. "rtsp://...", "http://...") rather than a
// local capture device identifier. Network streams are opened directly through libavformat
// (protocol auto-probed), bypassing the platform's local capture backend entirely.
bool isNetworkCameraPath(const std::string& path);

END_NAMESPACE_VIDEO_DEVICE_MODULE
