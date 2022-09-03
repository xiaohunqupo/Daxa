#include "impl_pipeline.hpp"
#include "impl_swapchain.hpp"
#include "impl_device.hpp"

#include <regex>
#include <thread>

static const std::regex PRAGMA_ONCE_REGEX = std::regex(R"reg(#\s*pragma\s*once\s*)reg");
static const std::regex REPLACE_REGEX = std::regex(R"reg(\W)reg");
static void shader_preprocess(std::string & file_str, std::filesystem::path const & path)
{
    std::smatch matches = {};
    std::string line = {};
    std::stringstream file_ss{file_str};
    std::stringstream result_ss = {};
    bool has_pragma_once = false;
    auto abspath_str = std::filesystem::absolute(path).string();
    for (daxa::usize line_num = 0; std::getline(file_ss, line); ++line_num)
    {
        if (std::regex_match(line, matches, PRAGMA_ONCE_REGEX))
        {
            result_ss << "#if !defined(";
            std::regex_replace(std::ostreambuf_iterator<char>(result_ss), abspath_str.begin(), abspath_str.end(), REPLACE_REGEX, "");
            result_ss << ")\n";
            has_pragma_once = true;
        }
        else
        {
            result_ss << line << "\n";
        }
    }
    if (has_pragma_once)
    {
        result_ss << "\n#define ";
        std::regex_replace(std::ostreambuf_iterator<char>(result_ss), abspath_str.begin(), abspath_str.end(), REPLACE_REGEX, "");
        result_ss << "\n#endif\n";
    }
    file_str = result_ss.str();
}

namespace daxa
{
#if DAXA_BUILT_WITH_SHADERC
    class ShadercFileIncluder : public shaderc::CompileOptions::IncluderInterface
    {
      public:
        constexpr static inline size_t DELETE_SOURCE_NAME = 0x1;
        constexpr static inline size_t DELETE_CONTENT = 0x2;

        ImplPipelineCompiler * impl_pipeline_compiler;

        virtual shaderc_include_result * GetInclude(
            const char * requested_source,
            shaderc_include_type type,
            const char * requesting_source,
            size_t include_depth) override
        {
            shaderc_include_result * res = new shaderc_include_result{};
            if (include_depth <= 10)
            {
                res->source_name = requested_source;
                res->source_name_length = strlen(requested_source);
                res->user_data = 0;

                auto result = impl_pipeline_compiler->full_path_to_file(requested_source);
                if (result.is_err())
                {
                    res->content = "#error could not find file";
                    res->content_length = strlen(res->content);

                    // res->source_name = nullptr;
                    // res->source_name_length = 0;
                    return res;
                }

                auto full_path = result.value();
                auto search_pred = [&](std::filesystem::path const & p)
                { return p == full_path; };

                if (std::find_if(
                        impl_pipeline_compiler->current_seen_shader_files.begin(),
                        impl_pipeline_compiler->current_seen_shader_files.end(),
                        search_pred) != impl_pipeline_compiler->current_seen_shader_files.end())
                {
                    // Return empty string blob if this file has been included before
                    static const char null_str[] = " ";
                    res->content = null_str;
                    res->content_length = 0;
                    return res;
                }

                impl_pipeline_compiler->current_observed_hotload_files->insert({full_path, std::chrono::file_clock::now()});
                auto shadercode_result = impl_pipeline_compiler->load_shader_source_from_file(full_path);

                if (shadercode_result.is_err())
                {
                    res->content = "#error could not load shader source";
                    res->content_length = strlen(res->content);

                    res->source_name = nullptr;
                    res->source_name_length = 0;
                    return res;
                }

                auto & shadercode_str = shadercode_result.value().string;
                res->content_length = shadercode_str.size();
                char * res_content = new char[res->content_length + 1];
                for (usize i = 0; i < res->content_length; ++i)
                {
                    res_content[i] = shadercode_str[i];
                }
                res_content[res->content_length] = '\0';
                res->content = res_content;

                auto full_path_str = full_path.string();
                res->source_name_length = full_path_str.size();
                char * res_source_name = new char[res->source_name_length + 1];
                for (usize i = 0; i < res->source_name_length; ++i)
                {
                    auto c = full_path_str[i];
                    if (c == '\\')
                        c = '/';
                    res_source_name[i] = c;
                }
                res_source_name[res->source_name_length] = '\0';
                res->source_name = res_source_name;

                res->user_data = reinterpret_cast<void *>(reinterpret_cast<size_t>(res->user_data) | DELETE_CONTENT);
                res->user_data = reinterpret_cast<void *>(reinterpret_cast<size_t>(res->user_data) | DELETE_SOURCE_NAME);
            }
            else
            {
                // max include depth exceeded
                res->content = "current include depth of 10 was exceeded";
                res->content_length = std::strlen(res->content);

                res->source_name = nullptr;
                res->source_name_length = 0;
            }
            return res;
        }

        virtual void ReleaseInclude(shaderc_include_result * data) override
        {
            if (data)
            {
                if (reinterpret_cast<size_t>(data->user_data) & DELETE_CONTENT)
                {
                    delete data->content;
                }
                if (reinterpret_cast<size_t>(data->user_data) & DELETE_SOURCE_NAME)
                {
                    delete data->source_name;
                }
                delete data;
            }
        };
    };
#endif

#if DAXA_BUILT_WITH_DXC
    struct DxcCustomIncluder : public IDxcIncludeHandler
    {
        IDxcIncludeHandler * default_includer;
        ImplPipelineCompiler * impl_pipeline_compiler;

        virtual ~DxcCustomIncluder() {}
        HRESULT LoadSource(LPCWSTR filename, IDxcBlob ** include_source) override
        {
            if (filename[0] == '.')
            {
                filename += 2;
            }

            auto result = impl_pipeline_compiler->full_path_to_file(filename);
            if (result.is_err())
            {
                *include_source = nullptr;
                return SCARD_E_FILE_NOT_FOUND;
            }
            auto full_path = result.value();
            auto search_pred = [&](std::filesystem::path const & p)
            { return p == full_path; };

            ComPtr<IDxcBlobEncoding> dxc_blob_encoding = {};
            if (std::find_if(impl_pipeline_compiler->current_seen_shader_files.begin(),
                             impl_pipeline_compiler->current_seen_shader_files.end(), search_pred) != impl_pipeline_compiler->current_seen_shader_files.end())
            {
                // Return empty string blob if this file has been included before
                static const char null_str[] = " ";
                impl_pipeline_compiler->dxc_backend.dxc_utils->CreateBlob(null_str, sizeof(null_str), CP_UTF8, &dxc_blob_encoding);
                *include_source = dxc_blob_encoding.Detach();
                return S_OK;
            }
            else
            {
                impl_pipeline_compiler->current_observed_hotload_files->insert({full_path, std::chrono::file_clock::now()});
            }

            auto str_result = impl_pipeline_compiler->load_shader_source_from_file(full_path);
            if (str_result.is_err())
            {
                *include_source = nullptr;
                return SCARD_E_INVALID_PARAMETER;
            }
            std::string str = str_result.value().string;

            impl_pipeline_compiler->dxc_backend.dxc_utils->CreateBlob(str.c_str(), static_cast<u32>(str.size()), CP_UTF8, &dxc_blob_encoding);
            *include_source = dxc_blob_encoding.Detach();
            return S_OK;
        }

        HRESULT QueryInterface(REFIID riid, void ** object) override
        {
            return default_includer->QueryInterface(riid, object);
        }

        unsigned long STDMETHODCALLTYPE AddRef(void) override { return 0; }
        unsigned long STDMETHODCALLTYPE Release(void) override { return 0; }
    };
#endif
} // namespace daxa

namespace daxa
{
    void ShaderCompileOptions::inherit(ShaderCompileOptions const & other)
    {
        if (!this->entry_point.has_value())
            this->entry_point = other.entry_point;
        if (!this->opt_level.has_value())
            this->opt_level = other.opt_level;
        if (!this->shader_model.has_value())
            this->shader_model = other.shader_model;
        if (!this->language.has_value())
            this->language = other.language;

        this->root_paths.insert(this->root_paths.begin(), other.root_paths.begin(), other.root_paths.end());
        this->defines.insert(this->defines.end(), other.defines.begin(), other.defines.end());
    }

    RasterPipeline::RasterPipeline(ManagedPtr impl) : ManagedPtr(std::move(impl)) {}

    ComputePipeline::ComputePipeline(ManagedPtr impl) : ManagedPtr(std::move(impl)) {}

    PipelineCompiler::PipelineCompiler(ManagedPtr impl) : ManagedPtr(std::move(impl)) {}

    auto PipelineCompiler::create_raster_pipeline(RasterPipelineInfo const & info) -> Result<RasterPipeline>
    {
        auto & impl = *as<ImplPipelineCompiler>();
        auto modified_info = info;
        modified_info.vertex_shader_info.compile_options.inherit(impl.info.shader_compile_options);
        modified_info.fragment_shader_info.compile_options.inherit(impl.info.shader_compile_options);

        if (modified_info.push_constant_size > MAX_PUSH_CONSTANT_BYTE_SIZE)
        {
            return ResultErr{std::string("push constant size of ") + std::to_string(modified_info.push_constant_size) + std::string(" exceeds the maximum size of ") + std::to_string(MAX_PUSH_CONSTANT_BYTE_SIZE)};
        }
        if (modified_info.push_constant_size % 4 != 0)
        {
            return ResultErr{std::string("push constant size of ") + std::to_string(modified_info.push_constant_size) + std::string(" is not a multiple of 4(bytes)")};
        }

        auto impl_pipeline = new ImplRasterPipeline(impl.impl_device, modified_info);
        impl.current_observed_hotload_files = &impl_pipeline->observed_hotload_files;

        auto v_spirv_result = impl.get_spirv(modified_info.vertex_shader_info, VkShaderStageFlagBits::VK_SHADER_STAGE_VERTEX_BIT);
        if (v_spirv_result.is_err())
        {
            return ResultErr{.message = v_spirv_result.message()};
        }
        std::vector<u32> v_spirv = v_spirv_result.value();

        auto p_spirv_result = impl.get_spirv(modified_info.fragment_shader_info, VkShaderStageFlagBits::VK_SHADER_STAGE_FRAGMENT_BIT);
        if (p_spirv_result.is_err())
        {
            return ResultErr{.message = p_spirv_result.message()};
        }
        std::vector<u32> p_spirv = p_spirv_result.value();

        VkShaderModule v_vk_shader_module = {};
        VkShaderModule p_vk_shader_module = {};

        VkShaderModuleCreateInfo shader_module_vertex{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .codeSize = static_cast<u32>(v_spirv.size() * sizeof(u32)),
            .pCode = v_spirv.data(),
        };
        vkCreateShaderModule(impl.impl_device.as<ImplDevice>()->vk_device, &shader_module_vertex, nullptr, &v_vk_shader_module);

        VkShaderModuleCreateInfo shader_module_pixel{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .codeSize = static_cast<u32>(p_spirv.size() * sizeof(u32)),
            .pCode = p_spirv.data(),
        };
        vkCreateShaderModule(impl.impl_device.as<ImplDevice>()->vk_device, &shader_module_pixel, nullptr, &p_vk_shader_module);

        impl_pipeline->vk_pipeline_layout = impl.impl_device.as<ImplDevice>()->gpu_table.pipeline_layouts[(modified_info.push_constant_size + 3) / 4];

        VkPipelineShaderStageCreateInfo vk_pipeline_shader_stage_create_infos[2] = {
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = {},
                .stage = VkShaderStageFlagBits::VK_SHADER_STAGE_VERTEX_BIT,
                .module = v_vk_shader_module,
                .pName = modified_info.vertex_shader_info.compile_options.entry_point.value().c_str(),
                .pSpecializationInfo = nullptr,
            },
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = {},
                .stage = VkShaderStageFlagBits::VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = p_vk_shader_module,
                .pName = modified_info.fragment_shader_info.compile_options.entry_point.value().c_str(),
                .pSpecializationInfo = nullptr,
            },
        };

        constexpr VkPipelineVertexInputStateCreateInfo vk_vertex_input_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .vertexBindingDescriptionCount = 0,
            .pVertexBindingDescriptions = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions = nullptr,
        };
        constexpr VkPipelineInputAssemblyStateCreateInfo vk_input_assembly_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .topology = VkPrimitiveTopology::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        constexpr VkPipelineMultisampleStateCreateInfo vk_multisample_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .rasterizationSamples = VkSampleCountFlagBits::VK_SAMPLE_COUNT_1_BIT,
            .minSampleShading = 1.0f,
        };

        VkPipelineRasterizationStateCreateInfo vk_raster_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .polygonMode = *reinterpret_cast<VkPolygonMode const *>(&info.raster.polygon_mode),
            .cullMode = *reinterpret_cast<VkCullModeFlags const *>(&info.raster.face_culling),
            .frontFace = VkFrontFace::VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        VkPipelineDepthStencilStateCreateInfo vk_depth_stencil_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .depthTestEnable = modified_info.depth_test.enable_depth_test,
            .depthWriteEnable = modified_info.depth_test.enable_depth_write,
            .depthCompareOp = static_cast<VkCompareOp>(modified_info.depth_test.depth_test_compare_op),
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = modified_info.depth_test.min_depth_bounds,
            .maxDepthBounds = modified_info.depth_test.max_depth_bounds,
        };

        DAXA_DBG_ASSERT_TRUE_M(modified_info.color_attachments.size() < PIPELINE_COMPILER_MAX_ATTACHMENTS, "too many color attachments, make pull request to bump max");

        std::array<VkPipelineColorBlendAttachmentState, PIPELINE_COMPILER_MAX_ATTACHMENTS> vk_pipeline_color_blend_attachment_blend_states = {};
        for (usize i = 0; i < modified_info.color_attachments.size(); ++i)
        {
            vk_pipeline_color_blend_attachment_blend_states[i] = *reinterpret_cast<VkPipelineColorBlendAttachmentState const *>(&modified_info.color_attachments[i].blend);
        }

        std::array<VkFormat, PIPELINE_COMPILER_MAX_ATTACHMENTS> vk_pipeline_color_attachment_formats = {};
        for (usize i = 0; i < modified_info.color_attachments.size(); ++i)
        {
            vk_pipeline_color_attachment_formats[i] = *reinterpret_cast<VkFormat const *>(&modified_info.color_attachments[i].format);
        }

        VkPipelineColorBlendStateCreateInfo vk_color_blend_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .logicOpEnable = VK_FALSE,
            .logicOp = {},
            .attachmentCount = static_cast<u32>(modified_info.color_attachments.size()),
            .pAttachments = vk_pipeline_color_blend_attachment_blend_states.data(),
            .blendConstants = {1.0f, 1.0f, 1.0f, 1.0f},
        };

        constexpr VkViewport DEFAULT_VIEWPORT{.width = 1, .height = 1};
        constexpr VkRect2D DEFAULT_SCISSOR{.extent = {1, 1}};

        VkPipelineViewportStateCreateInfo vk_viewport_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .viewportCount = 1,
            .pViewports = &DEFAULT_VIEWPORT,
            .scissorCount = 1,
            .pScissors = &DEFAULT_SCISSOR,
        };

        auto dynamic_state = std::array{
            VkDynamicState::VK_DYNAMIC_STATE_VIEWPORT,
            VkDynamicState::VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo vk_dynamic_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .dynamicStateCount = static_cast<u32>(dynamic_state.size()),
            .pDynamicStates = dynamic_state.data(),
        };

        VkPipelineRenderingCreateInfo vk_pipeline_rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
            .pNext = nullptr,
            .colorAttachmentCount = static_cast<u32>(modified_info.color_attachments.size()),
            .pColorAttachmentFormats = vk_pipeline_color_attachment_formats.data(),
            .depthAttachmentFormat = static_cast<VkFormat>(modified_info.depth_test.depth_attachment_format),
        };

        VkGraphicsPipelineCreateInfo vk_graphics_pipeline_create_info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &vk_pipeline_rendering,
            .flags = {},
            .stageCount = 2,
            .pStages = vk_pipeline_shader_stage_create_infos,
            .pVertexInputState = &vk_vertex_input_state,
            .pInputAssemblyState = &vk_input_assembly_state,
            .pViewportState = &vk_viewport_state,
            .pRasterizationState = &vk_raster_state,
            .pMultisampleState = &vk_multisample_state,
            .pDepthStencilState = &vk_depth_stencil_state,
            .pColorBlendState = &vk_color_blend_state,
            .pDynamicState = &vk_dynamic_state,
            .layout = impl_pipeline->vk_pipeline_layout,
            .renderPass = nullptr,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };

        auto pipeline_result = vkCreateGraphicsPipelines(
            impl.impl_device.as<ImplDevice>()->vk_device,
            VK_NULL_HANDLE,
            1u,
            &vk_graphics_pipeline_create_info,
            nullptr,
            &impl_pipeline->vk_pipeline);

        DAXA_DBG_ASSERT_TRUE_M(pipeline_result == VK_SUCCESS, "failed to create graphics pipeline");

        vkDestroyShaderModule(impl.impl_device.as<ImplDevice>()->vk_device, v_vk_shader_module, nullptr);
        vkDestroyShaderModule(impl.impl_device.as<ImplDevice>()->vk_device, p_vk_shader_module, nullptr);

        if (impl.impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->enable_debug_names && modified_info.debug_name.size() > 0)
        {
            auto raster_pipeline_name = modified_info.debug_name + std::string(" [Daxa RasterPipeline]");
            VkDebugUtilsObjectNameInfoEXT name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_PIPELINE,
                .objectHandle = reinterpret_cast<uint64_t>(impl_pipeline->vk_pipeline),
                .pObjectName = raster_pipeline_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(impl.impl_device.as<ImplDevice>()->vk_device, &name_info);
        }
        return RasterPipeline{ManagedPtr{impl_pipeline}};
    }

    auto PipelineCompiler::create_compute_pipeline(ComputePipelineInfo const & info) -> Result<ComputePipeline>
    {
        auto & impl = *as<ImplPipelineCompiler>();
        auto modified_info = info;
        modified_info.shader_info.compile_options.inherit(impl.info.shader_compile_options);

        if (modified_info.push_constant_size > MAX_PUSH_CONSTANT_BYTE_SIZE)
        {
            return ResultErr{std::string("push constant size of ") + std::to_string(modified_info.push_constant_size) + std::string(" exceeds the maximum size of ") + std::to_string(MAX_PUSH_CONSTANT_BYTE_SIZE)};
        }
        if (modified_info.push_constant_size % 4 != 0)
        {
            return ResultErr{std::string("push constant size of ") + std::to_string(modified_info.push_constant_size) + std::string(" is not a multiple of 4(bytes)")};
        }

        auto impl_pipeline = new ImplComputePipeline(impl.impl_device, modified_info);
        impl.current_observed_hotload_files = &impl_pipeline->observed_hotload_files;

        auto spirv_result = impl.get_spirv(modified_info.shader_info, VkShaderStageFlagBits::VK_SHADER_STAGE_COMPUTE_BIT);
        if (spirv_result.is_err())
        {
            return ResultErr{.message = spirv_result.message()};
        }
        std::vector<u32> spirv = spirv_result.value();

        VkShaderModule vk_shader_module = {};

        VkShaderModuleCreateInfo shader_module_ci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .codeSize = static_cast<u32>(spirv.size() * sizeof(u32)),
            .pCode = spirv.data(),
        };
        vkCreateShaderModule(impl.impl_device.as<ImplDevice>()->vk_device, &shader_module_ci, nullptr, &vk_shader_module);

        impl_pipeline->vk_pipeline_layout = impl.impl_device.as<ImplDevice>()->gpu_table.pipeline_layouts[(modified_info.push_constant_size + 3) / 4];

        VkComputePipelineCreateInfo vk_compute_pipeline_create_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .stage = VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = {},
                .stage = VkShaderStageFlagBits::VK_SHADER_STAGE_COMPUTE_BIT,
                .module = vk_shader_module,
                .pName = modified_info.shader_info.compile_options.entry_point.value().c_str(),
                .pSpecializationInfo = nullptr,
            },
            .layout = impl_pipeline->vk_pipeline_layout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };

        auto pipeline_result = vkCreateComputePipelines(
            impl.impl_device.as<ImplDevice>()->vk_device,
            VK_NULL_HANDLE,
            1u,
            &vk_compute_pipeline_create_info,
            nullptr,
            &impl_pipeline->vk_pipeline);

        DAXA_DBG_ASSERT_TRUE_M(pipeline_result == VK_SUCCESS, "failed to create compute pipeline");

        vkDestroyShaderModule(impl.impl_device.as<ImplDevice>()->vk_device, vk_shader_module, nullptr);

        if (impl.impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->enable_debug_names && info.debug_name.size() > 0)
        {
            auto raster_pipeline_name = modified_info.debug_name + std::string(" [Daxa ComputePipeline]");
            VkDebugUtilsObjectNameInfoEXT name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_PIPELINE,
                .objectHandle = reinterpret_cast<uint64_t>(impl_pipeline->vk_pipeline),
                .pObjectName = raster_pipeline_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(impl.impl_device.as<ImplDevice>()->vk_device, &name_info);
        }

        return ComputePipeline{ManagedPtr{impl_pipeline}};
    }

    auto PipelineCompiler::recreate_raster_pipeline(RasterPipeline const & pipeline) -> Result<RasterPipeline>
    {
        auto & impl_pipeline = *pipeline.as<ImplRasterPipeline>();
        auto result = create_raster_pipeline(impl_pipeline.info);
        if (result.is_ok())
        {
            return {std::move(result.value())};
        }
        return ResultErr{.message = result.message()};
    }

    auto PipelineCompiler::recreate_compute_pipeline(ComputePipeline const & pipeline) -> Result<ComputePipeline>
    {
        auto & impl_pipeline = *pipeline.as<ImplComputePipeline>();
        auto result = create_compute_pipeline(impl_pipeline.info);
        if (result.is_ok())
        {
            return {std::move(result.value())};
        }
        return ResultErr{.message = result.message()};
    }

    auto PipelineCompiler::check_if_sources_changed(RasterPipeline & pipeline) -> bool
    {
        auto & impl = *as<ImplPipelineCompiler>();
        auto & pipeline_impl = *pipeline.as<ImplRasterPipeline>();
        auto now = std::chrono::file_clock::now();
        using namespace std::chrono_literals;
        if (now - pipeline_impl.last_hotload_time < 250ms)
        {
            return false;
        }
        pipeline_impl.last_hotload_time = now;
        bool reload = false;
        for (auto & [path, recordedWriteTime] : pipeline_impl.observed_hotload_files)
        {
            auto ifs = std::ifstream(path);
            if (ifs.good())
            {
                auto latestWriteTime = std::filesystem::last_write_time(path);
                if (latestWriteTime > recordedWriteTime)
                {
                    reload = true;
                }
            }
        }
        if (reload)
        {
            for (auto & pair : pipeline_impl.observed_hotload_files)
            {
                auto ifs = std::ifstream(pair.first);
                if (ifs.good())
                {
                    pair.second = std::filesystem::last_write_time(pair.first);
                }
            }
        }
        return reload;
    }

    auto PipelineCompiler::check_if_sources_changed(ComputePipeline & pipeline) -> bool
    {
        auto & impl = *as<ImplPipelineCompiler>();
        auto & pipeline_impl = *pipeline.as<ImplComputePipeline>();
        auto now = std::chrono::file_clock::now();
        using namespace std::chrono_literals;
        if (now - pipeline_impl.last_hotload_time < 250ms)
        {
            return false;
        }
        pipeline_impl.last_hotload_time = now;
        bool reload = false;
        for (auto & [path, recordedWriteTime] : pipeline_impl.observed_hotload_files)
        {
            auto ifs = std::ifstream(path);
            if (ifs.good())
            {
                auto latestWriteTime = std::filesystem::last_write_time(path);
                if (latestWriteTime > recordedWriteTime)
                {
                    reload = true;
                }
            }
        }
        if (reload)
        {
            for (auto & pair : pipeline_impl.observed_hotload_files)
            {
                auto ifs = std::ifstream(pair.first);
                if (ifs.good())
                {
                    pair.second = std::filesystem::last_write_time(pair.first);
                }
            }
        }
        return reload;
    }

    ImplPipelineCompiler::ImplPipelineCompiler(ManagedWeakPtr a_impl_device, PipelineCompilerInfo const & info)
        : impl_device{std::move(a_impl_device)}, info{info}
    {
        if (!this->info.shader_compile_options.entry_point.has_value())
            this->info.shader_compile_options.entry_point = std::optional<std::string>{"main"};
        if (!this->info.shader_compile_options.opt_level.has_value())
            this->info.shader_compile_options.opt_level = std::optional<u32>{0};
        if (!this->info.shader_compile_options.shader_model.has_value())
            this->info.shader_compile_options.shader_model = std::optional<ShaderModel>{ShaderModel{.major = 6, .minor = 6}};
        if (!this->info.shader_compile_options.language.has_value())
            this->info.shader_compile_options.language = std::optional<ShaderLanguage>{ShaderLanguage::HLSL};

#if DAXA_BUILT_WITH_SHADERC
        {
            auto includer = ShadercFileIncluder{};
            includer.impl_pipeline_compiler = this;
            shaderc_backend.options.SetIncluder(std::make_unique<ShadercFileIncluder>(includer));
        }
#endif

#if DAXA_BUILT_WITH_DXC
        {
            HRESULT dxc_utils_result = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&this->dxc_backend.dxc_utils));
            DAXA_DBG_ASSERT_TRUE_M(SUCCEEDED(dxc_utils_result), "Failed to create DXC utils");
            HRESULT dxc_compiler_result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&this->dxc_backend.dxc_compiler));
            DAXA_DBG_ASSERT_TRUE_M(SUCCEEDED(dxc_compiler_result), "Failed to create DXC compiler");
            ComPtr<DxcCustomIncluder> dxc_includer = new DxcCustomIncluder();
            dxc_includer->impl_pipeline_compiler = this;
            this->dxc_backend.dxc_utils->CreateDefaultIncludeHandler(&(dxc_includer->default_includer));
            this->dxc_backend.dxc_includer = dxc_includer.Detach();
        }
#endif
    }

    ImplPipelineCompiler::~ImplPipelineCompiler() {}

    auto ImplPipelineCompiler::get_spirv(ShaderInfo const & shader_info, VkShaderStageFlagBits shader_stage) -> Result<std::vector<u32>>
    {
        std::vector<u32> spirv = {};
        if (shader_info.source.index() == 2)
        {
            ShaderSPIRV const & input_spirv = std::get<ShaderSPIRV>(shader_info.source);
            spirv.resize(input_spirv.size);
            for (usize i = 0; i < input_spirv.size; ++i)
            {
                spirv[i] = input_spirv.data[i];
            }
        }
        else
        {
            ShaderCode code = {};
            if (auto shader_source = std::get_if<ShaderFile>(&shader_info.source))
            {
                auto ret = full_path_to_file(shader_source->path);
                if (ret.is_err())
                {
                    return ResultErr{ret.message()};
                }
                auto code_ret = load_shader_source_from_file(ret.value());
                if (code_ret.is_err())
                {
                    return ResultErr{code_ret.message()};
                }
                code = code_ret.value();
            }
            else
            {
                code = std::get<ShaderCode>(shader_info.source);
            }

            Result<std::vector<u32>> ret = ResultErr{.message = "No shader was compiled"};

            switch (shader_info.compile_options.language.value())
            {
#if DAXA_BUILT_WITH_SHADERC
            case ShaderLanguage::GLSL:
                ret = gen_spirv_from_shaderc(shader_info, shader_stage, code);
                break;
#endif
#if DAXA_BUILT_WITH_DXC
            case ShaderLanguage::HLSL:
                ret = gen_spirv_from_dxc(shader_info, shader_stage, code);
                break;
#endif
            default: break;
            }

            if (ret.is_err())
            {
                return ResultErr{ret.message()};
            }
            spirv = ret.value();
        }
        return spirv;
    }

    auto ImplPipelineCompiler::full_path_to_file(std::filesystem::path const & file) -> Result<std::filesystem::path>
    {
        if (std::filesystem::exists(file))
        {
            return {file};
        }
        std::filesystem::path potential_path;
        // TODO: FIX THIS!! URGENT. Root paths should be got from the shader
        for (auto & root : this->info.shader_compile_options.root_paths)
        {
            potential_path.clear();
            potential_path = root / file;
            if (std::filesystem::exists(potential_path))
            {
                return {potential_path};
            }
        }
        std::string error_msg = {};
        error_msg += "could not find file :\"";
        error_msg += file.string();
        error_msg += "\"";
        return ResultErr{.message = std::move(error_msg)};
    }

    auto ImplPipelineCompiler::load_shader_source_from_file(std::filesystem::path const & path) -> Result<ShaderCode>
    {
        auto result_path = full_path_to_file(path);
        if (result_path.is_err())
        {
            return ResultErr{.message = result_path.message()};
        }
        auto start_time = std::chrono::steady_clock::now();
        while ((std::chrono::steady_clock::now() - start_time).count() < 100'000'000)
        {
            std::ifstream ifs{path};
            DAXA_DBG_ASSERT_TRUE_M(ifs.good(), "Could not open shader file");
            current_observed_hotload_files->insert({
                result_path.value(),
                std::filesystem::last_write_time(result_path.value()),
            });
            std::string str = {};
            ifs.seekg(0, std::ios::end);
            str.reserve(ifs.tellg());
            ifs.seekg(0, std::ios::beg);
            str.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
            if (str.size() < 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            shader_preprocess(str, path);
            return ShaderCode{.string = str};
        }
        std::string err = "timeout while trying to read file: \"";
        err += result_path.value().string() + "\"";
        return ResultErr{.message = err};
    }

    auto ImplPipelineCompiler::gen_spirv_from_shaderc(ShaderInfo const & shader_info, VkShaderStageFlagBits shader_stage, ShaderCode const & code) -> Result<std::vector<u32>>
    {
#if DAXA_BUILT_WITH_SHADERC
        auto translate_shader_stage = [](VkShaderStageFlagBits stage) -> shaderc_shader_kind
        {
            switch (stage)
            {
            case VkShaderStageFlagBits::VK_SHADER_STAGE_VERTEX_BIT: return shaderc_shader_kind::shaderc_vertex_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return shaderc_shader_kind::shaderc_tess_control_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return shaderc_shader_kind::shaderc_tess_evaluation_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_GEOMETRY_BIT: return shaderc_shader_kind::shaderc_geometry_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_FRAGMENT_BIT: return shaderc_shader_kind::shaderc_fragment_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_COMPUTE_BIT: return shaderc_shader_kind::shaderc_compute_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_RAYGEN_BIT_KHR: return shaderc_shader_kind::shaderc_raygen_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_ANY_HIT_BIT_KHR: return shaderc_shader_kind::shaderc_anyhit_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return shaderc_shader_kind::shaderc_closesthit_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_MISS_BIT_KHR: return shaderc_shader_kind::shaderc_miss_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_INTERSECTION_BIT_KHR: return shaderc_shader_kind::shaderc_intersection_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_CALLABLE_BIT_KHR: return shaderc_shader_kind::shaderc_callable_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_TASK_BIT_NV: return shaderc_shader_kind::shaderc_task_shader;
            case VkShaderStageFlagBits::VK_SHADER_STAGE_MESH_BIT_NV: return shaderc_shader_kind::shaderc_mesh_shader;
            default:
                std::cerr << "error: unknown shader stage!\n";
                std::abort();
            }
        };

        DAXA_DBG_ASSERT_TRUE_M(shader_info.compile_options.opt_level < 3, "For shaderc, The optimization level must be between 0 and 2 (inclusive)");

        auto spirv_stage = translate_shader_stage(shader_stage);
        shaderc_backend.options.SetSourceLanguage(shaderc_source_language_glsl);
        shaderc_backend.options.SetTargetEnvironment(shaderc_target_env_vulkan, VK_API_VERSION_1_1);
        shaderc_backend.options.SetTargetSpirv(shaderc_spirv_version_1_3);
        shaderc_backend.options.SetOptimizationLevel(static_cast<shaderc_optimization_level>(shader_info.compile_options.opt_level.value()));

        for (auto const & shader_define : shader_info.compile_options.defines)
        {
            if (shader_define.value.size() > 0)
            {
                shaderc_backend.options.AddMacroDefinition(shader_define.name, shader_define.value);
            }
            else
            {
                shaderc_backend.options.AddMacroDefinition(shader_define.name);
            }
        }

        shaderc_backend.options.AddMacroDefinition("_DAXA_SHADER", "1");
        shaderc_backend.options.AddMacroDefinition("_DAXA_GLSL", "1");
        shaderc_backend.options.AddMacroDefinition("DAXA_SHADER_INCLUDE", "<daxa/daxa.inl>");

        std::string debug_name = "unnamed shader";
        if (ShaderFile const * shader_file = std::get_if<ShaderFile>(&shader_info.source))
            debug_name = shader_file->path.string();
        else if (shader_info.debug_name.size() > 0)
            debug_name = shader_info.debug_name;

        shaderc::SpvCompilationResult spv_module = shaderc_backend.compiler.CompileGlslToSpv(
            code.string.c_str(), spirv_stage, debug_name.c_str(), shaderc_backend.options);

        if (spv_module.GetCompilationStatus() != shaderc_compilation_status_success)
            return daxa::ResultErr{.message = std::string("SHADERC: ") + spv_module.GetErrorMessage()};

        auto spv = std::vector<u32>{spv_module.begin(), spv_module.end()};

        std::ofstream spv_output{"glsl_compute.spv", std::ios::binary};
        spv_output.write((char const*)spv.data(), spv.size() * sizeof(spv[0]));

        return spv;
#else
        return ResultErr{.message = "Asked for Shaderc compilation without enabling Shaderc"};
#endif
    }

    auto ImplPipelineCompiler::gen_spirv_from_dxc(ShaderInfo const & shader_info, VkShaderStageFlagBits shader_stage, ShaderCode const & code) -> Result<std::vector<u32>>
    {
#if DAXA_BUILT_WITH_DXC
        auto u8_ascii_to_wstring = [](char const * str) -> std::wstring
        {
            std::wstring ret = {};
            for (int i = 0; i < std::strlen(str) + 1 && str != nullptr; i++)
            {
                ret.push_back(str[i]);
            }
            return ret;
        };

        std::vector<const wchar_t *> args = {};

        std::vector<std::wstring> wstring_buffer = {};

        wstring_buffer.reserve(shader_info.compile_options.defines.size() + 1 + shader_info.compile_options.root_paths.size());

        for (auto & define : shader_info.compile_options.defines)
        {
            auto define_str = define.name;
            if (define.value.length() > 0)
                define_str = define_str + "=" + define.value;
            wstring_buffer.push_back(u8_ascii_to_wstring(define_str.c_str()));
            args.push_back(L"-D");
            args.push_back(wstring_buffer.back().c_str());
        }
        args.push_back(L"-D_DAXA_SHADER");
        args.push_back(L"-D_DAXA_HLSL");
        args.push_back(L"-DDAXA_SHADER_INCLUDE=\"daxa/daxa.inl\"");

        if (shader_info.source.index() == 0)
        {
            wstring_buffer.push_back(std::get<ShaderFile>(shader_info.source).path.wstring());
            args.push_back(wstring_buffer.back().c_str());
        }

        for (auto & root : shader_info.compile_options.root_paths)
        {
            args.push_back(L"-I");
            wstring_buffer.push_back(root.wstring());
            args.push_back(wstring_buffer.back().c_str());
        }

        // set matrix packing to column major
        args.push_back(L"-Zpc");
        // set warnings as errors
        args.push_back(DXC_ARG_WARNINGS_ARE_ERRORS); //-WX
        // setting target
        args.push_back(L"-spirv");
        args.push_back(L"-fspv-target-env=vulkan1.1");
        // set optimization setting
        switch (shader_info.compile_options.opt_level.value())
        {
        case 0: args.push_back(L"-O0"); break;
        case 1: args.push_back(L"-O1"); break;
        case 2: args.push_back(L"-O2"); break;
        case 3: args.push_back(L"-O3"); break;
        default: DAXA_DBG_ASSERT_TRUE_M(false, "Bad optimization level set in Pipeline Compiler"); break;
        }
        // setting entry point
        args.push_back(L"-E");
        auto entry_point_wstr = u8_ascii_to_wstring(shader_info.compile_options.entry_point.value().c_str());
        args.push_back(entry_point_wstr.c_str());

        if (this->impl_device.as<ImplDevice>()->info.use_scalar_layout)
        {
            args.push_back(L"-fvk-use-scalar-layout");
        }

        // set shader model
        args.push_back(L"-T");
        std::wstring profile = L"vs_x_x";
        profile[3] = L'0' + shader_info.compile_options.shader_model.value().major;
        profile[5] = L'0' + shader_info.compile_options.shader_model.value().minor;
        switch (shader_stage)
        {
        case VkShaderStageFlagBits::VK_SHADER_STAGE_VERTEX_BIT: profile[0] = L'v'; break;
        case VkShaderStageFlagBits::VK_SHADER_STAGE_FRAGMENT_BIT: profile[0] = L'p'; break;
        case VkShaderStageFlagBits::VK_SHADER_STAGE_COMPUTE_BIT: profile[0] = L'c'; break;
        default: break;
        }
        args.push_back(profile.c_str());
        // set hlsl version to 2021
        args.push_back(L"-HV");
        args.push_back(L"2021");
        DxcBuffer source_buffer{
            .Ptr = code.string.c_str(),
            .Size = static_cast<u32>(code.string.size()),
            .Encoding = static_cast<u32>(0),
        };

        IDxcResult * result;
        this->dxc_backend.dxc_compiler->Compile(
            &source_buffer, args.data(), static_cast<u32>(args.size()),
            this->dxc_backend.dxc_includer, IID_PPV_ARGS(&result));
        IDxcBlobUtf8 * error_message;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&error_message), nullptr);

        if (error_message && error_message->GetStringLength() > 0)
        {
            auto str = std::string();
            str.resize(error_message->GetBufferSize());
            for (usize i = 0; i < str.size(); i++)
            {
                str[i] = static_cast<char const *>(error_message->GetBufferPointer())[i];
            }
            str = std::string("DXC: ") + str;
            return daxa::ResultErr{.message = str};
        }

        IDxcBlob * shaderobj;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderobj), nullptr);

        std::vector<u32> spv;
        spv.resize(shaderobj->GetBufferSize() / sizeof(u32));
        for (usize i = 0; i < spv.size(); i++)
        {
            spv[i] = static_cast<u32 *>(shaderobj->GetBufferPointer())[i];
        }

        std::ofstream spv_output{"compute.spv", std::ios::binary};
        spv_output.write((char const*)spv.data(), spv.size() * sizeof(spv[0]));

        return {spv};
#else
        return ResultErr{.message = "Asked for Dxc compilation without enabling Dxc"};
#endif
    }

    ImplRasterPipeline::ImplRasterPipeline(ManagedWeakPtr a_impl_device, RasterPipelineInfo const & info)
        : impl_device{std::move(a_impl_device)}, info{info}
    {
    }

    ImplRasterPipeline::~ImplRasterPipeline()
    {
        vkDestroyPipeline(this->impl_device.as<ImplDevice>()->vk_device, this->vk_pipeline, nullptr);
    }

    auto ImplRasterPipeline::managed_cleanup() -> bool
    {
        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->impl_device.as<ImplDevice>()->main_queue_zombies_mtx});
        u64 main_queue_cpu_timeline_value = DAXA_ATOMIC_FETCH(this->impl_device.as<ImplDevice>()->main_queue_cpu_timeline);
        this->impl_device.as<ImplDevice>()->main_queue_raster_pipeline_zombies.push_front({main_queue_cpu_timeline_value, std::unique_ptr<ImplRasterPipeline>{this}});
        return false;
    }

    ImplComputePipeline::ImplComputePipeline(ManagedWeakPtr a_impl_device, ComputePipelineInfo const & info)
        : impl_device{std::move(a_impl_device)}, info{info}
    {
    }

    ImplComputePipeline::~ImplComputePipeline()
    {
        vkDestroyPipeline(this->impl_device.as<ImplDevice>()->vk_device, this->vk_pipeline, nullptr);
    }

    auto ImplComputePipeline::managed_cleanup() -> bool
    {
        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->impl_device.as<ImplDevice>()->main_queue_zombies_mtx});
        u64 main_queue_cpu_timeline_value = DAXA_ATOMIC_FETCH(this->impl_device.as<ImplDevice>()->main_queue_cpu_timeline);
        this->impl_device.as<ImplDevice>()->main_queue_compute_pipeline_zombies.push_front({main_queue_cpu_timeline_value, std::unique_ptr<ImplComputePipeline>{this}});
        return false;
    }
} // namespace daxa
