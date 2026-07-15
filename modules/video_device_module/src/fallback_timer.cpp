#include <video_device_module/fallback_timer.h>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

FallbackTimer::~FallbackTimer()
{
    stop();
}

void FallbackTimer::start(Float frameRateHz, TickCallback onTick)
{
    stop();

    {
        std::lock_guard lock(mutex);
        this->frameRateHz = frameRateHz;
        this->onTick = std::move(onTick);
    }

    acquisitionRunning.store(true, std::memory_order_relaxed);
    thread = std::thread(&FallbackTimer::timerLoop, this);
}

void FallbackTimer::stop()
{
    if (!acquisitionRunning.exchange(false, std::memory_order_relaxed))
        return;

    cv.notify_one();

    if (thread.joinable())
        thread.join();
}

bool FallbackTimer::isRunning() const
{
    return acquisitionRunning.load(std::memory_order_relaxed);
}

void FallbackTimer::timerLoop()
{
    Float rateHz;
    {
        std::lock_guard lock(mutex);
        rateHz = frameRateHz;
    }

    const auto framePeriod =
        rateHz > 0.0f
            ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<Float>(1.0f / rateHz))
            : std::chrono::seconds(1);

    auto nextTick = std::chrono::steady_clock::now();

    while (acquisitionRunning.load(std::memory_order_relaxed))
    {
        {
            std::unique_lock lock(mutex);
            nextTick += framePeriod;
            cv.wait_until(lock, nextTick, [this] { return !acquisitionRunning.load(std::memory_order_relaxed); });
            if (!acquisitionRunning.load(std::memory_order_relaxed))
                break;
        }

        TickCallback tick;
        {
            std::lock_guard lock(mutex);
            tick = onTick;
        }

        if (tick)
            tick();
    }
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
