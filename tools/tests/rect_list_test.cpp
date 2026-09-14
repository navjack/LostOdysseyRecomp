// Headless rect-list expansion test: a vertex shader with the translator's entry
// signature, wrapped by xenos::WrapRectListVertexShader, must cover whole
// rectangles from three corners (right angle at any corner) and extrapolate the
// fourth corner's interpolators.
#include <gpu/shader/dxc_compiler.h>
#include <gpu/shader/xenos_translator.h>
#include <plume_render_interface.h>
#include <fmt/format.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef __APPLE__
#include <plume_metal_ir.h>
#endif

namespace plume {
#ifdef __APPLE__
    std::unique_ptr<RenderInterface> CreateMetalInterface();
#else
    std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
}

int main()
{
    using namespace plume;
#ifdef __APPLE__
    auto api = CreateMetalInterface();
    const auto binaryFormat = xenos::ShaderBinaryFormat::MetalIR;
    const auto shaderFormat = RenderShaderFormat::METAL_IR;
#else
    auto api = CreateD3D12Interface();
    const auto binaryFormat = xenos::ShaderBinaryFormat::Dxil;
    const auto shaderFormat = RenderShaderFormat::DXIL;
#endif
    if (!api) return 2;
    auto device = api->createDevice();
    if (!device) return 2;
#ifdef __APPLE__
    SetMetalShaderConverterDescriptorSets(device.get(), true);
#endif
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    if (!queue || !commands || !fence) return 2;

    // Same entry signature the translator emits (see EmitPrologue).
    std::string guest = "ByteAddressBuffer corners : register(t0, space0);\n"
                        "void main(\n\tin uint xeVertexId : SV_VertexID,\n\tout precise float4 oPos : SV_Position";
    for (int i = 0; i < 16; ++i)
        guest += fmt::format(",\n\tout float4 o{0} : TEXCOORD{0}", i);
    guest += ")\n{\n";
    for (int i = 0; i < 16; ++i)
        guest += fmt::format("\to{} = 0.0;\n", i);
    guest += "\tfloat2 p = float2(asfloat(corners.Load(xeVertexId * 8u)), asfloat(corners.Load(xeVertexId * 8u + 4u)));\n"
             "\toPos = float4(p, 0.5, 1.0);\n"
             "\to0 = float4(p * float2(0.5, -0.5) + 0.5, 0.0, 1.0);\n"
             "}\n";
    const std::string wrapped = xenos::WrapRectListVertexShader(guest);
    if (wrapped.empty())
    {
        std::puts("FAIL: wrapper did not recognise the translator entry signature");
        return 1;
    }
    const auto vs = xenos::CompileHlsl(wrapped, "main", "vs_6_0", binaryFormat);
    const auto ps = xenos::CompileHlsl("float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) : SV_Target { return float4(uv.xy, 0.0, 1.0); }",
        "main", "ps_6_0", binaryFormat);
    if (!vs.ok || !ps.ok)
    {
        std::fprintf(stderr, "compile failed: %s%s\n", vs.errors.c_str(), ps.errors.c_str());
        return 1;
    }
    auto vertexShader = device->createShader(vs.bytecode.data(), vs.bytecode.size(), "main", shaderFormat);
    auto pixelShader = device->createShader(ps.bytecode.data(), ps.bytecode.size(), "main", shaderFormat);

    RenderDescriptorSetBuilder setBuilder;
    setBuilder.begin();
    const uint32_t cornerBase = setBuilder.addByteAddressBuffer(0, 1);
    setBuilder.end();
    RenderPipelineLayoutBuilder layoutBuilder;
    layoutBuilder.begin(false, false);
    layoutBuilder.addDescriptorSet(setBuilder);
    layoutBuilder.end();
    auto layout = layoutBuilder.create(device.get());
    auto set = setBuilder.create(device.get());
    if (!vertexShader || !pixelShader || !layout || !set) return 2;

    // Two rects as guest triangles: left half with the right angle at its second
    // vertex, right half with the right angle at its first vertex.
    const float corners[6][2] = {
        {0, -1}, {-1, -1}, {-1, 1},
        {1, 1}, {1, -1}, {0, 1},
    };
    auto cornerBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(256, RenderBufferFlag::STORAGE));
    auto indexBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(256, RenderBufferFlag::INDEX));
    constexpr uint32_t size = 16;
    auto target = device->createTexture(RenderTextureDesc::Texture2D(size, size, 1, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::RENDER_TARGET));
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(256 * size));
    if (!cornerBuffer || !indexBuffer || !target || !readback) return 2;
    std::memcpy(cornerBuffer->map(), corners, sizeof(corners));
    cornerBuffer->unmap();
    // Renderer packing: 6 * first guest vertex + corner.
    uint32_t indices[12];
    for (uint32_t r = 0; r < 2; ++r)
        for (uint32_t k = 0; k < 6; ++k)
            indices[r * 6 + k] = (r * 3) * 6 + k;
    std::memcpy(indexBuffer->map(), indices, sizeof(indices));
    indexBuffer->unmap();
    set->setBuffer(cornerBase, cornerBuffer.get(), 256);

    const RenderTexture* attachments[] = {target.get()};
    auto framebuffer = device->createFramebuffer(RenderFramebufferDesc(attachments, 1));
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout.get();
    desc.vertexShader = vertexShader.get();
    desc.pixelShader = pixelShader.get();
    desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
    desc.renderTargetCount = 1;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.cullMode = RenderCullMode::NONE;
    auto pipeline = device->createGraphicsPipeline(desc);
    if (!framebuffer || !pipeline) return 2;

    commands->begin();
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(framebuffer.get());
    commands->clearColor(0, RenderColor(0, 0, 0, 0));
    RenderViewport viewport(0, 0, float(size), float(size));
    RenderRect scissor(0, 0, size, size);
    commands->setViewports(&viewport, 1);
    commands->setScissors(&scissor, 1);
    commands->setPipeline(pipeline.get());
    commands->setGraphicsPipelineLayout(layout.get());
    commands->setGraphicsDescriptorSet(set.get(), 0);
    RenderIndexBufferView indexView(RenderBufferReference(indexBuffer.get(), 0), sizeof(indices), RenderFormat::R32_UINT);
    commands->setIndexBuffer(&indexView);
    commands->drawIndexedInstanced(12, 1, 0, 0, 0);
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
    commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R8G8B8A8_UNORM, size, size, 1, 64, 0),
        RenderTextureCopyLocation::Subresource(target.get(), 0));
    commands->end();
    const RenderCommandList* lists[] = {commands.get()};
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());

    const auto* pixels = static_cast<const uint8_t*>(readback->map());
    size_t uncovered = 0, wrongUv = 0;
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            const uint8_t* p = pixels + y * 256 + x * 4;
            const int expectedU = int(std::lround(255.0 * (x + 0.5) / size));
            const int expectedV = int(std::lround(255.0 * (y + 0.5) / size));
            const bool covered = p[3] == 255;
            const bool uv = std::abs(p[0] - expectedU) <= 2 && std::abs(p[1] - expectedV) <= 2;
            uncovered += !covered;
            wrongUv += covered && !uv;
            if (!covered || !uv)
                std::printf("pixel %2u,%2u: rgba %3u %3u %3u %3u expected uv %d %d\n", x, y, p[0], p[1], p[2], p[3], expectedU, expectedV);
        }
    }
    readback->unmap();
    if (uncovered || wrongUv)
    {
        std::printf("FAIL: %zu uncovered pixels, %zu wrong interpolators\n", uncovered, wrongUv);
        return 1;
    }
    std::puts("PASS: rect-list vertex-shader expansion covers both rects with extrapolated fourth corners");
    return 0;
}
