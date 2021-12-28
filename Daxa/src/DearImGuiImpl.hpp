#include "DaxaCore.hpp"
#include "gpu/Device.hpp"

#include <deque>

#include <imgui.h>
#include <imgui_impl_glfw.h>

namespace daxa {

    class ImGuiRenderer{
    public:
        ImGuiRenderer(gpu::DeviceHandle device, gpu::QueueHandle queue);

        void recordCommands(ImDrawData* draw_data, daxa::gpu::CommandListHandle& cmdList, gpu::ImageHandle& target);
    private:
        daxa::gpu::DeviceHandle device                  = {};
        daxa::gpu::PipelineHandle pipeline              = {};
        daxa::gpu::BindingSetAllocatorHandle setAlloc   = {};
        daxa::gpu::ImageHandle fontSheet                = {};
        daxa::gpu::BindingSetHandle fontSheetBinding    = {};

        struct PerFrameData{
            daxa::gpu::BufferHandle vertexBuffer        = {};
            daxa::gpu::BufferHandle indexBuffer         = {};
        };
        std::deque<PerFrameData> perFrameData           = {};

        void recreatePerFrameData(size_t newMinSizeVertex, size_t newMinSizeIndices);
    };
}