// Headless METAL_IR binding test shaped like the renderer's pipeline layout:
// root CBVs b0-b2 in space0, set0 = byte-address buffers t0-t2 + sampler s0,
// sets 1-3 = texture tables t0-t3. Every output channel depends on a
// different resource, so a missing or shifted binding zeroes that channel.
#include <gpu/shader/dxc_compiler.h>
#include <plume_metal_ir.h>
#include <plume_render_interface.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace plume { std::unique_ptr<RenderInterface> CreateMetalInterface(); }

namespace
{
    constexpr const char* kSource = R"(
cbuffer VsConstants : register(b0, space0) { float4 c0; };
cbuffer Shared : register(b1, space0) { float4 c1; };
cbuffer PsConstants : register(b2, space0) { float4 c2; };
ByteAddressBuffer vfetch2 : register(t2, space0);
Texture2D<float4> tex2D_3 : register(t3, space1);
Texture2D<float4> tex3D_1 : register(t1, space2);

float4 vertex(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    float scale = float(vfetch2.Load(8)) / 1000.0 * c0.w;
    return float4((uv * float2(2.0, -2.0) + float2(-1.0, 1.0)) * scale, 0.0, 1.0);
}

float4 pixel(float4 pos : SV_Position) : SV_Target
{
    float4 a = tex2D_3.Load(int3(0, 0, 0));
    float4 b = tex3D_1.Load(int3(0, 0, 0));
    return float4(c0.x * a.r, c1.y * b.g, c2.z, float(vfetch2.Load(4)) / 255.0);
}
)";
}

int main()
{
    using namespace plume;
    auto api = CreateMetalInterface();
    if (!api) return 2;
    auto device = api->createDevice();
    if (!device) return 2;
    SetMetalShaderConverterDescriptorSets(device.get(), true);
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    if (!queue || !commands || !fence) return 2;

    const auto vs = xenos::CompileHlsl(kSource, "vertex", "vs_6_0", xenos::ShaderBinaryFormat::MetalIR);
    const auto ps = xenos::CompileHlsl(kSource, "pixel", "ps_6_0", xenos::ShaderBinaryFormat::MetalIR);
    if (!vs.ok || !ps.ok)
    {
        std::fprintf(stderr, "compile failed: %s%s\n", vs.errors.c_str(), ps.errors.c_str());
        return 1;
    }
    auto vertexShader = device->createShader(vs.bytecode.data(), vs.bytecode.size(), "vertex", RenderShaderFormat::METAL_IR);
    auto pixelShader = device->createShader(ps.bytecode.data(), ps.bytecode.size(), "pixel", RenderShaderFormat::METAL_IR);

    // Layout identical in shape to Renderer::Init's D3D12/Metal branch.
    RenderDescriptorSetBuilder setBuilders[4];
    setBuilders[0].begin();
    const uint32_t vfetchBase = setBuilders[0].addByteAddressBuffer(0, 3);
    const uint32_t samplerBase = setBuilders[0].addSampler(0, 1);
    setBuilders[0].end();
    uint32_t textureBase[4] = {};
    for (int i = 1; i < 4; ++i)
    {
        setBuilders[i].begin();
        textureBase[i] = setBuilders[i].addTexture(0, 4);
        setBuilders[i].end();
    }
    RenderPipelineLayoutBuilder layoutBuilder;
    layoutBuilder.begin(false, false);
    for (uint32_t i = 0; i < 3; ++i)
        layoutBuilder.addRootDescriptor(i, 0, RenderRootDescriptorType::CONSTANT_BUFFER);
    for (auto& builder : setBuilders)
        layoutBuilder.addDescriptorSet(builder);
    layoutBuilder.end();
    auto layout = layoutBuilder.create(device.get());
    if (!layout || !vertexShader || !pixelShader) return 2;

    constexpr uint32_t size = 4;
    const auto target = [&] {
        return device->createTexture(RenderTextureDesc::Texture2D(size, size, 1, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::RENDER_TARGET));
    };
    auto texA = target(), texB = target(), output = target();
    const RenderTexture* attachA[] = {texA.get()};
    const RenderTexture* attachB[] = {texB.get()};
    const RenderTexture* attachOut[] = {output.get()};
    auto fbA = device->createFramebuffer(RenderFramebufferDesc(attachA, 1));
    auto fbB = device->createFramebuffer(RenderFramebufferDesc(attachB, 1));
    auto fbOut = device->createFramebuffer(RenderFramebufferDesc(attachOut, 1));
    auto sampler = device->createSampler(RenderSamplerDesc());
    auto upload = device->createBuffer(RenderBufferDesc::UploadBuffer(768));
    auto arena = device->createBuffer(RenderBufferDesc::UploadBuffer(4096, RenderBufferFlag::STORAGE));
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(256 * size));
    if (!texA || !texB || !output || !fbA || !fbB || !fbOut || !sampler || !upload || !arena || !readback) return 2;

    // Root CBVs at the renderer's 256-byte upload alignment.
    const float constants[3][4] = {{0.25f, 0, 0, 1}, {0, 0.5f, 0, 0}, {0, 0, 0.75f, 0}};
    auto* uploadData = static_cast<uint8_t*>(upload->map());
    std::memset(uploadData, 0, 768);
    for (int i = 0; i < 3; ++i)
        std::memcpy(uploadData + i * 256, constants[i], sizeof(constants[i]));
    upload->unmap();
    auto* arenaData = static_cast<uint8_t*>(arena->map());
    std::memset(arenaData, 0, 4096);
    const uint32_t alpha = 128, scale = 1000;
    std::memcpy(arenaData + 4, &alpha, 4);
    std::memcpy(arenaData + 8, &scale, 4);
    arena->unmap();

    auto set0 = setBuilders[0].create(device.get());
    auto set1 = setBuilders[1].create(device.get());
    auto set2 = setBuilders[2].create(device.get());
    auto set3 = setBuilders[3].create(device.get());
    if (!set0 || !set1 || !set2 || !set3) return 2;
    for (uint32_t i = 0; i < 3; ++i)
        set0->setBuffer(vfetchBase + i, arena.get(), 4096);
    set0->setSampler(samplerBase, sampler.get());
    for (uint32_t i = 0; i < 4; ++i)
    {
        set1->setTexture(textureBase[1] + i, texA.get(), RenderTextureLayout::SHADER_READ);
        set2->setTexture(textureBase[2] + i, texB.get(), RenderTextureLayout::SHADER_READ);
        set3->setTexture(textureBase[3] + i, texB.get(), RenderTextureLayout::SHADER_READ);
    }

    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout.get();
    desc.vertexShader = vertexShader.get();
    desc.pixelShader = pixelShader.get();
    desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
    desc.renderTargetCount = 1;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.cullMode = RenderCullMode::NONE;
    auto pipeline = device->createGraphicsPipeline(desc);
    if (!pipeline) return 2;

    commands->begin();
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texA.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(fbA.get());
    commands->clearColor(0, RenderColor(1.0f, 0.5f, 0.25f, 1.0f));
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texB.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(fbB.get());
    commands->clearColor(0, RenderColor(0.0f, 1.0f, 0.0f, 1.0f));
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texA.get(), RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texB.get(), RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(output.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(fbOut.get());
    commands->clearColor(0, RenderColor(0, 0, 0, 0));
    RenderViewport viewport(0, 0, float(size), float(size));
    RenderRect scissor(0, 0, size, size);
    commands->setViewports(&viewport, 1);
    commands->setScissors(&scissor, 1);
    // Same call order as Renderer::DrawImpl: pipeline, layout, root CBVs, sets.
    commands->setPipeline(pipeline.get());
    commands->setGraphicsPipelineLayout(layout.get());
    for (uint32_t i = 0; i < 3; ++i)
        commands->setGraphicsRootDescriptor(RenderBufferReference(upload.get(), i * 256), i);
    commands->setGraphicsDescriptorSet(set0.get(), 0);
    commands->setGraphicsDescriptorSet(set1.get(), 1);
    commands->setGraphicsDescriptorSet(set2.get(), 2);
    commands->setGraphicsDescriptorSet(set3.get(), 3);
    commands->drawInstanced(3, 1, 0, 0);
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(output.get(), RenderTextureLayout::COPY_SOURCE));
    commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R8G8B8A8_UNORM, size, size, 1, 64, 0),
        RenderTextureCopyLocation::Subresource(output.get(), 0));
    commands->end();
    const RenderCommandList* lists[] = {commands.get()};
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());

    const auto* pixels = static_cast<const uint8_t*>(readback->map());
    const int expected[4] = {64, 128, 191, 128};
    size_t mismatches = 0;
    for (uint32_t y = 0; y < size; ++y)
    {
        const uint8_t* row = pixels + y * 256;
        for (uint32_t x = 0; x < size; ++x)
        {
            const uint8_t* p = row + x * 4;
            bool wrong = false;
            for (int c = 0; c < 4; ++c)
                wrong |= std::abs(int(p[c]) - expected[c]) > 1;
            mismatches += wrong;
            if (wrong || (x == 0 && y == 0))
                std::printf("pixel %u,%u: %u %u %u %u (expected 64 128 191 128)\n", x, y, p[0], p[1], p[2], p[3]);
        }
    }
    readback->unmap();
    if (mismatches)
    {
        std::printf("FAIL: %zu pixels; R=b0*space1 G=b1*space2 B=b2 A=space0 buffer\n", mismatches);
        return 1;
    }
    std::puts("PASS: METAL_IR root CBVs b0-b2, byte-address buffer, texture tables in spaces 1-2");
    return 0;
}
