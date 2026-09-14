#pragma once

#include "dxc_compiler.h"
#include <plume_render_interface_types.h>

namespace xenos
{
    // What the runtime compiles for a device's native shader format, and the plume
    // format that loads it. Metal devices load Metal Shader Converter output.
    struct DeviceShaderFormat
    {
        ShaderBinaryFormat binary = ShaderBinaryFormat::Dxil;
        plume::RenderShaderFormat render = plume::RenderShaderFormat::DXIL;
        bool vulkan = false; // SPIR-V register and push-constant conventions
    };

    inline DeviceShaderFormat ShaderFormatFor(plume::RenderShaderFormat deviceFormat)
    {
        switch (deviceFormat)
        {
        case plume::RenderShaderFormat::SPIRV:
            return { ShaderBinaryFormat::Spirv, plume::RenderShaderFormat::SPIRV, true };
        case plume::RenderShaderFormat::METAL:
            return { ShaderBinaryFormat::MetalIR, plume::RenderShaderFormat::METAL_IR, false };
        default:
            return {};
        }
    }
}
