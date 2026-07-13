#include <gtest/gtest.h>
#include <testutils/daq_memcheck_listener.h>

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);

    testing::TestEventListeners& listeners = testing::UnitTest::GetInstance()->listeners();
    listeners.Append(new DaqMemCheckListener());

    return RUN_ALL_TESTS();
}
