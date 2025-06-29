#if DAXA_BUILT_WITH_UTILS_MEM

#include "../impl_core.hpp"

#include <daxa/utils/mem.hpp>
#include <utility>

namespace daxa
{
    RingBuffer::RingBuffer(RingBufferInfo a_info)
        : m_info{std::move(a_info)},
          m_buffer{this->m_info.device.create_buffer({
              .size = this->m_info.capacity,
              .allocate_info = a_info.prefer_device_memory ? daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE : daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
              .name = this->m_info.name,
          })},
          buffer_device_address{this->m_info.device.device_address(this->m_buffer).value()},
          buffer_host_address{this->m_info.device.buffer_host_address(this->m_buffer).value()}
    {
    }

    RingBuffer::RingBuffer(RingBuffer && other)
    {
        std::swap(this->m_info, other.m_info);
        std::swap(this->live_allocations, other.live_allocations);
        std::swap(this->m_buffer, other.m_buffer);
        std::swap(this->buffer_device_address, other.buffer_device_address);
        std::swap(this->buffer_host_address, other.buffer_host_address);
        std::swap(this->claimed_start, other.claimed_start);
        std::swap(this->claimed_size, other.claimed_size);
    }

    auto RingBuffer::operator=(RingBuffer && other) -> RingBuffer &
    {
        if (!this->m_buffer.is_empty())
        {
            this->m_info.device.destroy_buffer(this->m_buffer);
        }
        std::swap(this->m_info, other.m_info);
        std::swap(this->live_allocations, other.live_allocations);
        std::swap(this->m_buffer, other.m_buffer);
        std::swap(this->buffer_device_address, other.buffer_device_address);
        std::swap(this->buffer_host_address, other.buffer_host_address);
        std::swap(this->claimed_start, other.claimed_start);
        std::swap(this->claimed_size, other.claimed_size);
        return *this;
    }

    RingBuffer::~RingBuffer()
    {
        if (!this->m_buffer.is_empty())
        {
            this->m_info.device.destroy_buffer(this->m_buffer);
        }
    }

    auto RingBuffer::allocate(u32 allocation_size, u32 alignment_requirement) -> std::optional<RingBuffer::Allocation>
    {
        u32 const tail_alloc_offset = (this->claimed_start + this->claimed_size) % this->m_info.capacity;
        auto up_align_offset = [](auto value, auto alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        };
        u32 const tail_alloc_offset_aligned = up_align_offset(tail_alloc_offset, alignment_requirement);
        u32 const tail_alloc_align_padding = tail_alloc_offset_aligned - tail_alloc_offset;
        // Two allocations are possible:
        // Tail allocation is when the allocation is placed directly at the end of all other allocations.
        // Zero offset allocation is possible when there is not enough space left at the tail BUT there is enough space from 0 up to the start of the other allocations.
        auto calc_tail_allocation_possible = [&]()
        {
            u32 const tail = tail_alloc_offset_aligned;
            bool const wrapped = this->claimed_start + this->claimed_size > this->m_info.capacity;
            u32 const end = wrapped ? this->claimed_start : this->m_info.capacity;
            return tail + allocation_size <= end;
        };
        auto calc_zero_offset_allocation_possible = [&]()
        {
            return this->claimed_start + this->claimed_size <= this->m_info.capacity && allocation_size < this->claimed_start;
        };
        // Firstly, test if there is enough continuous space left to allocate.
        bool tail_allocation_possible = calc_tail_allocation_possible();
        // When there is no tail space left, it may be the case that we can place the allocation at offset 0.
        // Illustration: |XXX ## |; "X": new allocation; "#": used up space; " ": free space.
        bool zero_offset_allocation_possible = calc_zero_offset_allocation_possible();
        if (!tail_allocation_possible && !zero_offset_allocation_possible)
        {
            this->reclaim_memory();
            tail_allocation_possible = calc_tail_allocation_possible();
            zero_offset_allocation_possible = calc_zero_offset_allocation_possible();
            if (!tail_allocation_possible && !zero_offset_allocation_possible)
            {
                return std::nullopt;
            }
        }
        u64 const current_timeline_value = m_info.device.latest_submit_index();
        u32 returned_allocation_offset = {};
        u32 actual_allocation_offset = {};
        u32 actual_allocation_size = {};
        if (tail_allocation_possible)
        {
            actual_allocation_size = allocation_size + tail_alloc_align_padding;
            returned_allocation_offset = tail_alloc_offset_aligned;
            actual_allocation_offset = tail_alloc_offset;
        }
        else // Zero offset allocation.
        {
            u32 const left_tail_space = this->m_info.capacity - (this->claimed_start + this->claimed_size);
            actual_allocation_size = allocation_size + left_tail_space;
            returned_allocation_offset = {};
            actual_allocation_offset = {};
        }
        this->claimed_size += actual_allocation_size;
        live_allocations.push_back(TrackedAllocation{
            .submit_index = current_timeline_value,
            .offset = actual_allocation_offset,
            .size = actual_allocation_size,
        });
        return Allocation{
            .device_address = this->buffer_device_address + returned_allocation_offset,
            .host_address = reinterpret_cast<void *>(reinterpret_cast<u8 *>(this->buffer_host_address) + returned_allocation_offset),
            .buffer_offset = returned_allocation_offset,
            .size = allocation_size,
            .submit_index = current_timeline_value,
        };
    }

    void RingBuffer::reclaim_memory()
    {
        auto const current_gpu_submit_index_value = this->m_info.device.oldest_pending_submit_index();
        while (!live_allocations.empty() && live_allocations.front().submit_index <= current_gpu_submit_index_value)
        {
            this->claimed_start = (this->claimed_start + live_allocations.front().size) % this->m_info.capacity;
            this->claimed_size -= live_allocations.front().size;
            live_allocations.pop_front();
        }
    }

    auto RingBuffer::info() const -> RingBufferInfo const &
    {
        return this->m_info;
    }

    auto RingBuffer::buffer() const -> daxa::BufferId
    {
        return this->m_buffer;
    }
    
    void RingBuffer::reuse_memory_after_pending_submits()
    {
        this->reclaim_submit_index = this->m_info.device.latest_submit_index();
        reclaim_memory();
    }
} // namespace daxa

#endif
