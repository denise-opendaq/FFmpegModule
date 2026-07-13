#include <video_device_module/time_accumulator.h>
#include <coretypes/exceptions.h>
#include <coretypes/ratio_factory.h>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

TimeAccumulator::TimeAccumulator()
    : time(0)
    , remainderAccum(0)
    , ratio(Ratio(1,1))
    , frequency(Ratio(1,1))
    , delta(Ratio(1,1))
{
}

void TimeAccumulator::setStartTime(uint64_t time)
{
    this->time = time;
}

void TimeAccumulator::setRatio(const RatioPtr& ratio)
{
    this->time = getTime(ratio);
    this->ratio = ratio;
    updateTicksPerStep();
}

void TimeAccumulator::setFrequency(const RatioPtr& delta)
{
    this->frequency = delta;
    updateTicksPerStep();
}

RatioPtr TimeAccumulator::getRatio() const
{
    return ratio;
}

RatioPtr TimeAccumulator::getFrequency() const
{
    return frequency;
}

RatioPtr TimeAccumulator::getDelta() const
{
    return delta;
}

void TimeAccumulator::updateTicksPerStep()
{
    delta = Ratio(ratio.getDenominator() * frequency.getNumerator(),
                  ratio.getNumerator() * frequency.getDenominator()).simplify();
}

uint64_t TimeAccumulator::calculateSteps(uint64_t timeDelta, uint64_t& localRemainderAccum) const
{
    uint64_t totalNumerator = timeDelta * delta.getDenominator() + localRemainderAccum;
    uint64_t denominator = delta.getNumerator();
    uint64_t steps = totalNumerator / denominator;
    localRemainderAccum = totalNumerator % denominator;
    return steps;
}

uint64_t TimeAccumulator::update(uint64_t newTime)
{
    if (newTime <= time)
        return 0;

    uint64_t steps = calculateSteps(newTime - time, remainderAccum);
    incrementTimeBySteps(steps);
    return steps;
}

uint64_t TimeAccumulator::getSourceTime() const
{
    return time;
}

uint64_t TimeAccumulator::getTime(const RatioPtr& toRatio) const
{
    if (time == 0)
        return time;

    auto simplified = (ratio / toRatio).simplify();
    return time * simplified.getNumerator() / simplified.getDenominator();
}

uint64_t TimeAccumulator::getAvailableSteps(uint64_t newTime) const
{
    if (newTime <= time)
        return 0;

    uint64_t localRemainderAccum = remainderAccum;
    return calculateSteps(newTime - time, localRemainderAccum);
}

inline void TimeAccumulator::incrementTimeBySteps(uint64_t steps)
{
    time += steps * delta.getNumerator() / delta.getDenominator();
}

uint64_t TimeAccumulator::incrementTime(uint64_t deltaTime)
{
    uint64_t steps = calculateSteps(deltaTime, remainderAccum);
    incrementTimeBySteps(steps);
    return steps;
}

END_NAMESPACE_VIDEO_DEVICE_MODULE