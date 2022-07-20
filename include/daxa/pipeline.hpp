#pragma once

#include <daxa/core.hpp>

namespace daxa
{
    struct ShaderFile
    {
        std::filesystem::path path;
    };

    // This string will only work if it is valid HLSL
    struct ShaderCode
    {
        std::string string;
    };

    struct ShaderSPIRV
    {
        u32 * data;
        usize size;
    };

    using ShaderSource = std::variant<ShaderFile, ShaderCode, ShaderSPIRV>;

    struct ShaderInfo
    {
        ShaderSource source;
        std::string entry_point = {"main"};
        std::vector<std::string> defines = {};
        std::string debug_name = {};
    };

    struct GraphicsPipelineInfo
    {
        ShaderInfo shader_info = {};
        u32 push_constant_size = {};
        std::string debug_name = {};
    };

    struct ComputePipelineInfo
    {
        ShaderInfo shader_info = {};
        u32 push_constant_size = {};
        std::string debug_name = {};
    };

    struct ComputePipeline : Handle
    {
        ~ComputePipeline();

        auto info() const -> ComputePipelineInfo const &;

      private:
        friend struct PipelineCompiler;
        friend struct CommandList;
        ComputePipeline(std::shared_ptr<void> impl);
    };

    struct DepthTestInfo
    {
        Format depth_attachment_format = {};
        bool enable_depth_test = false;
        bool enable_depth_write = false;
        CompareOp depth_test_compare_op = {};
        f32 min_depth_bounds = 0.0f;
        f32 max_depth_bounds = 1.0f;
    };

    enum class PolygonMode
    {

    };

    using FaceCullFlags = u32;
    struct FaceCullFlagBits
    {
    };

    struct RasterizerInfo
    {
        PolygonMode polygon_mode = {};
        FaceCullFlags face_culling = {};
        bool depth_clamp_enable = false;
        bool rasterizer_discard_enable = false;
        bool depth_bias_enable = false;
        f32 depth_bias_constant_factor = 0.0f;
        f32 depth_bias_clamp = 0.0f;
        f32 depth_bias_slope_factor = 0.0f;
        f32 line_width = 1.0f;
    };

    struct BlendInfo
    {
    };

    struct RenderAttachment
    {
        Format format = {};
        BlendInfo blend = {};
    };

    struct RasterPipelineInfo
    {
        ShaderInfo vertex_shader_info = {};
        ShaderInfo fragment_shader_info = {};
        std::vector<RenderAttachment> render_attachment = {};
        DepthTestInfo depth_test = {};
        RasterizerInfo raster = {};
        u32 push_constant_size = {};
        std::string debug_name = {};
    };

    struct RasterPipeline : Handle
    {
        ~RasterPipeline();

        auto info() const -> RasterPipelineInfo const &;

      private:
        friend struct PipelineCompiler;
        friend struct CommandList;
        RasterPipeline(std::shared_ptr<void> impl);
    };

    struct PipelineCompilerInfo
    {
        std::vector<std::filesystem::path> root_paths = {};
        std::string debug_name = {};
    };

    struct PipelineCompiler : Handle
    {
        auto create_graphics_pipeline(GraphicsPipelineInfo const & info) -> Result<GraphicsPipeline>;
        auto recreate_graphics_pipeline(GraphicsPipeline const & pipeline) -> Result<GraphicsPipeline>;
        auto create_compute_pipeline(ComputePipelineInfo const & info) -> Result<ComputePipeline>;
        auto recreate_compute_pipeline(ComputePipeline const & pipeline) -> Result<ComputePipeline>;
        auto check_if_sources_changed(GraphicsPipeline const & pipeline) -> bool;
        auto check_if_sources_changed(ComputePipeline const & pipeline) -> bool;

      private:
        friend struct Device;
        PipelineCompiler(std::shared_ptr<void> impl);
    };
} // namespace daxa
