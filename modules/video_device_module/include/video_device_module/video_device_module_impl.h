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
#include <video_device_module/camera_platform.h>
#include <opendaq/module_impl.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

class VideoDeviceModule final : public Module
{
public:
    explicit VideoDeviceModule(const ContextPtr& context);

    ListPtr<IDeviceInfo> onGetAvailableDevices() override;
    DictPtr<IString, IDeviceType> onGetAvailableDeviceTypes() override;
    DevicePtr onCreateDevice(const StringPtr& connectionString,
                             const ComponentPtr& parent,
                             const PropertyObjectPtr& config) override;
    DictPtr<IString, IFunctionBlockType> onGetAvailableFunctionBlockTypes() override;
    FunctionBlockPtr onCreateFunctionBlock(const StringPtr& id,
                                           const ComponentPtr& parent,
                                           const StringPtr& localId,
                                           const PropertyObjectPtr& config) override;

private:
    // Refreshes localDevices from a fresh platform enumeration, reassigning ids. Also returns
    // the scan as an ordered id/entry list, since the platform enumeration order (e.g. matching
    // USB port order) is worth preserving for display, which localDevices itself (an
    // unordered_map) cannot do.
    std::vector<std::pair<std::string, CameraDeviceEntry>> refreshLocalDeviceCache();
    // Resolves a connection-string id (see CameraDeviceEntry / assignUniqueDeviceIds) to the
    // actual OS device path, refreshing the cache once on a miss (e.g. a connection string
    // handed to this module instance without a prior onGetAvailableDevices() call, such as one
    // read back from a saved configuration). Throws NotFoundException if still unresolved.
    std::string resolveLocalDeviceId(const std::string& id);

    // Id -> enumerated entry, from the last scan (onGetAvailableDevices() or a cache-miss refresh).
    std::unordered_map<std::string, CameraDeviceEntry> localDevices;
};

END_NAMESPACE_VIDEO_DEVICE_MODULE
