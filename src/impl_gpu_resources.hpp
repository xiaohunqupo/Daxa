#pragma once

#include "impl_core.hpp"

#include <daxa/gpu_resources.hpp>

namespace daxa
{
    static inline constexpr u32 BUFFER_BINDING = 0;
    static inline constexpr u32 STORAGE_IMAGE_BINDING = 1;
    static inline constexpr u32 SAMPLED_IMAGE_BINDING = 2;
    static inline constexpr u32 SAMPLER_BINDING = 3;

    struct ImplBufferSlot
    {
        BufferInfo info = {};
        VkBuffer vk_buffer = {};
        VmaAllocation vma_allocation = {};
    };

    static inline constexpr i32 NOT_OWNED_BY_SWAPCHAIN = -1;

    struct ImplImageViewSlot
    {
        ImageViewInfo info = {};
        VkImageView vk_image_view = {};
    };

    struct ImplImageSlot
    {
        ImplImageViewSlot view_slot = {};
        ImageInfo info = {};
        VkImage vk_image = {};
        VmaAllocation vma_allocation = {};
        i32 swapchain_image_index = NOT_OWNED_BY_SWAPCHAIN;
    };

    struct ImplSamplerSlot
    {
        SamplerInfo info = {};
        VkSampler vk_sampler = {};
    };

    /**
     * @brief GpuResourcePool is intended to be used akin to a specialized memory allocator, specific to gpu resource types (like image views).
     *
     * This struct is threadsafe if the following assumptions are met:
     * * never dereference a deleted resource
     * * never delete a resource twice
     * That means the function dereference_id can be used without synchonization, even calling get_new_slot or return_old_slot in parallel is safe.
     *
     * To check if these assumptions are met at runtime, the debug define DAXA_ENABLE_GPU_ID_VALIDATION can be used.
     * The define enables runtime checking to detect use after free and double free at the cost of performance.
     */
    template <typename ResourceT, usize MAX_RESOURCE_COUNT = 1u << 20u>
    struct GpuResourcePool
    {
        static constexpr inline usize PAGE_BITS = 12u;
        static constexpr inline usize PAGE_SIZE = 1u << PAGE_BITS;
        static constexpr inline usize PAGE_MASK = PAGE_SIZE - 1u;
        static constexpr inline usize PAGE_COUNT = MAX_RESOURCE_COUNT / PAGE_SIZE;

        using PageT = std::array<std::pair<ResourceT, u8>, PAGE_SIZE>;

        std::vector<u32> free_index_stack = {};
        u32 next_index = {};
        usize max_resources = {};

#if defined(DAXA_ENABLE_THREADSAFETY)
        std::mutex page_alloc_mtx = {};
#if defined(DAXA_ENABLE_GPU_ID_VALIDATION)
        std::mutex use_after_free_check_mtx = {};
#endif
#endif
        std::array<std::unique_ptr<PageT>, PAGE_COUNT> pages = {};

#if defined(DAXA_ENABLE_GPU_ID_VALIDATION)
        void verify_ressource_id(GPUResourceId id)
        {
            size_t page = id.index >> PAGE_BITS;
            size_t offset = id.index & PAGE_MASK;
            DAXA_DBG_ASSERT_TRUE_M(pages[page] != nullptr, "detected invalid ressource id");
            DAXA_DBG_ASSERT_TRUE_M(id.version != 0, "detected invalid resource id");
        }
#endif

        auto new_slot() -> std::pair<GPUResourceId, ResourceT &>
        {
#if defined(DAXA_ENABLE_THREADSAFETY)
#if defined(DAXA_ENABLE_GPU_ID_VALIDATION)
            std::unique_lock use_after_free_check_lock{use_after_free_check_mtx};
#endif
            std::unique_lock page_alloc_lock{page_alloc_mtx};
#endif
            u32 index = {};
            if (free_index_stack.empty())
            {
                index = next_index++;
                DAXA_DBG_ASSERT_TRUE_M(index < MAX_RESOURCE_COUNT, "exceded max resource count");
                DAXA_DBG_ASSERT_TRUE_M(index < max_resources, "exceded max resource count");
            }
            else
            {
                index = free_index_stack.back();
                free_index_stack.pop_back();
            }

            size_t page = index >> PAGE_BITS;
            size_t offset = index & PAGE_MASK;

            if (!pages[page])
            {
                pages[page] = std::make_unique<PageT>();
                for (u32 i = 0; i < PAGE_SIZE; ++i)
                {
                    pages[page]->at(i).second = 0; // set all version numbers to 0 (invalid)
                }
            }

            pages[page]->at(offset).second = std::max<u8>(pages[page]->at(index).second, 1); // make sure the version is at least one

            u8 version = pages[page]->at(offset).second;

            return {GPUResourceId{.index = index, .version = version}, pages[page]->at(offset).first};
        }

        auto return_slot(GPUResourceId id)
        {
            size_t page = id.index >> PAGE_BITS;
            size_t offset = id.index & PAGE_MASK;

#if defined(DAXA_ENABLE_GPU_ID_VALIDATION)
#if defined(DAXA_ENABLE_THREADSAFETY)
            std::unique_lock use_after_free_check_lock{use_after_free_check_mtx};
#endif
            verify_ressource_id(id);
            DAXA_DBG_ASSERT_TRUE_M(pages[page]->at(offset).second == id.version, "detected double delete for a resource id");
#endif

#if defined(DAXA_ENABLE_THREADSAFETY)
            std::unique_lock page_alloc_lock{page_alloc_mtx};
#endif

            pages[page]->at(offset).second = std::max<u8>(pages[page]->at(offset).second + 1, 1); // the max is needed, as version = 0 is invalid

            free_index_stack.push_back(id.index);
        }

        auto dereference_id(GPUResourceId id) -> ResourceT &
        {
            size_t page = id.index >> PAGE_BITS;
            size_t offset = id.index & PAGE_MASK;

#if defined(DAXA_ENABLE_GPU_ID_VALIDATION)
#if defined(DAXA_ENABLE_THREADSAFETY)
            std::unique_lock use_after_free_check_lock{use_after_free_check_mtx};
#endif
            verify_ressource_id(id);
            u8 version = pages[page]->at(offset).second;
            DAXA_DBG_ASSERT_TRUE_M(version == id.version, "detected use after free for a resource id");
#endif
            return pages[page]->at(offset).first;
        }
    };

    struct GPUResourceTable
    {
        GpuResourcePool<ImplBufferSlot> buffer_slots = {};
        GpuResourcePool<ImplImageSlot> image_slots = {};
        GpuResourcePool<ImplSamplerSlot> sampler_slots = {};

        VkDescriptorSetLayout vk_descriptor_set_layout = {};
        VkDescriptorSet vk_descriptor_set = {};
        VkDescriptorPool vk_descriptor_pool = {};

        // Contains pipeline layouts with variing push constant range size.
        // The first size is 0 word, second is 1 word, all others are a power of two (maximum is MAX_PUSH_CONSTANT_BYTE_SIZE).
        std::array<VkPipelineLayout, PIPELINE_LAYOUT_COUNT> pipeline_layouts = {};

        void initialize(usize max_buffers, usize max_images, usize max_samplers, VkDevice device);
        void cleanup(VkDevice device);
    };

    void write_descriptor_set_sampler(VkDevice vk_device, VkDescriptorSet vk_descriptor_set, VkSampler vk_sampler, u32 index);

    void write_descriptor_set_buffer(VkDevice vk_device, VkDescriptorSet vk_descriptor_set, VkBuffer vk_buffer, VkDeviceSize offset, VkDeviceSize range, u32 index);

    void write_descriptor_set_image(VkDevice vk_device, VkDescriptorSet vk_descriptor_set, VkImageView vk_image_view, ImageUsageFlags usage, u32 index);
} // namespace daxa
