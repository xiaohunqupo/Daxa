#pragma once

#include <daxa/daxa.inl>

struct DrawVertex
{
    f32vec4 pos, col;
};

// clang-format off
#define BlockID_Debug           0
#define BlockID_Air             1
#define BlockID_Bedrock         2
#define BlockID_Brick           3
#define BlockID_Cactus          4
#define BlockID_Cobblestone     5
#define BlockID_CompressedStone 6
#define BlockID_DiamondOre      7
#define BlockID_Dirt            8
#define BlockID_DriedShrub      9
#define BlockID_Grass           10
#define BlockID_Gravel          11
#define BlockID_Lava            12
#define BlockID_Leaves          13
#define BlockID_Log             14
#define BlockID_MoltenRock      15
#define BlockID_Planks          16
#define BlockID_Rose            17
#define BlockID_Sand            18
#define BlockID_Sandstone       19
#define BlockID_Stone           20
#define BlockID_TallGrass       21
#define BlockID_Water           22

#define BlockFace_Left    0
#define BlockFace_Right   1
#define BlockFace_Bottom  2
#define BlockFace_Top     3
#define BlockFace_Back    4
#define BlockFace_Front   5
#define BlockFace_Cross_A 6
#define BlockFace_Cross_B 7
// clang-format on

struct UnpackedFace
{
    f32vec3 block_pos;
    f32vec3 pos;
    f32vec3 nrm;
    f32vec2 uv;
    u32 block_id;
    u32 block_face;
    u32 tex_id;
    u32 vert_id;
};

DAXA_DECL_BUFFER_STRUCT(
    DrawVertexBuffer,
    {
        u32 data[32 * 32 * 32 * 6];
    });

struct DrawPush
{
    f32mat4x4 vp_mat;
    f32vec3 chunk_pos;
    daxa_BufferRef(DrawVertexBuffer) face_buffer;
    ImageViewId atlas_texture;
    SamplerId atlas_sampler;
};
