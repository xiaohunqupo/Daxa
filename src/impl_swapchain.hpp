#pragma once

#include <daxa/swapchain.hpp>

#include "impl_core.hpp"

namespace daxa
{
    struct ImplDevice;

    struct ImplSwapchain final : ManagedSharedState
    {
        ManagedWeakPtr impl_device = {};
        SwapchainInfo info = {};
        VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;
        VkSurfaceKHR vk_surface = {};
        VkSurfaceFormatKHR vk_surface_format = {};
        std::vector<ImageId> images = {};
        std::vector<BinarySemaphore> acquire_semaphores = {};
        std::vector<BinarySemaphore> present_semaphores = {};
        usize cpu_frame_timeline = {};
        TimelineSemaphore gpu_frame_timeline;
        u32 current_image_index{};

        auto get_index_of_image(ImageId image) const -> usize;

        ImplSwapchain(ManagedWeakPtr device_impl, SwapchainInfo info);
        ~ImplSwapchain();

        void recreate();
        void cleanup();
        void recreate_surface();
    };
} // namespace daxa
