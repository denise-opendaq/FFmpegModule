#include <video_device_module/video_display_fb_impl.h>
#include <video_device_module/module_dll.h>

#include <gtest/gtest.h>
#include <opendaq/opendaq.h>

using namespace daq;
using namespace daq::modules::video_device_module;

namespace
{
ContextPtr createContext()
{
    const auto logger = Logger();
    return Context(Scheduler(logger), logger, TypeManager(), nullptr, nullptr);
}

ModulePtr CreateModule(const ContextPtr& context)
{
    ModulePtr module;
    createVideoDeviceModule(&module, context);
    return module;
}

FunctionBlockPtr createVideoDisplay(const ModulePtr& module)
{
    return module.createFunctionBlock(VideoDisplayFbImpl::Id, nullptr, "VideoDisplay");
}
}  // namespace

TEST(VideoDisplayFbImplTest, CreateType)
{
    const auto module = CreateModule(createContext());

    DictPtr<IString, IFunctionBlockType> types;
    ASSERT_NO_THROW(types = module.getAvailableFunctionBlockTypes());
    ASSERT_TRUE(types.hasKey(VideoDisplayFbImpl::Id));

    const FunctionBlockTypePtr type = types.get(VideoDisplayFbImpl::Id);
    ASSERT_EQ(type.getName(), VideoDisplayFbImpl::Name);
}

TEST(VideoDisplayFbImplTest, RejectsInputWithoutDomainSignal)
{
    const auto context = createContext();
    const auto module = CreateModule(context);
    const auto display = createVideoDisplay(module);
    const auto orphanSignal = Signal(context, nullptr, "orphan");
    orphanSignal.setDescriptor(DataDescriptorBuilder()
                                   .setSampleType(SampleType::Binary)
                                   .setName("Video")
                                   .setRule(ExplicitDataRule())
                                   .build());

    display.getInputPorts()[0].connect(orphanSignal);

    ASSERT_EQ(display.getStatusContainer().getStatus("ComponentStatus"), ComponentStatus::Error);
}
