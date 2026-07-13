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
#include <coretypes/ratio_ptr.h>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

struct TimeAccumulator
{
    explicit TimeAccumulator();

    void setStartTime(uint64_t time);
    void setRatio(const RatioPtr& ratio);
    void setFrequency(const RatioPtr& delta);

    RatioPtr getRatio() const;
    RatioPtr getFrequency() const;
    RatioPtr getDelta() const;
    
    uint64_t update(uint64_t newTime);
    uint64_t getSourceTime() const;
    uint64_t getTime(const RatioPtr& toRatio) const;
    uint64_t getAvailableSteps(uint64_t newTime) const;
    void incrementTimeBySteps(uint64_t steps);
    uint64_t incrementTime(uint64_t deltaTime);

private:
    uint64_t calculateSteps(uint64_t timeDelta, uint64_t& remainderAccum) const;
    void updateTicksPerStep();

    uint64_t time;
    uint64_t remainderAccum;

    daq::RatioPtr ratio;
    daq::RatioPtr frequency;
    daq::RatioPtr delta;
};

END_NAMESPACE_VIDEO_DEVICE_MODULE