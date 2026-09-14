#include "dxc_compiler.h"
#include "cache.h"
#include "binary_cache.h"
#include "resource_cpx_index_sha256.h"
#include <os/shader_log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <dxcapi.h>
#else
#include <dlfcn.h>
#ifdef __APPLE__
#include <metal_irconverter/metal_irconverter.h>
#include <plume_metal_ir.h>
#endif
#include <type_traits>
#ifndef __EMULATE_UUID
#define __EMULATE_UUID 1
#endif
#include <WinAdapter.h>
#include <dxcapi.h>
#endif

namespace xenos
{
    namespace
    {
        // GUIDs from dxcapi.h, defined here so no import library is needed.
        const CLSID kClsidDxcCompiler = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
        const CLSID kClsidDxcUtils = { 0x6245d6af, 0x66e0, 0x48fd, { 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c } };

        DxcCreateInstanceProc g_createInstance = nullptr;
#ifdef _WIN32
        HMODULE g_module = nullptr;
#else
        void* g_module = nullptr;
        std::filesystem::path g_modulePath;
#endif
        std::atomic<uint64_t> g_calls{0}, g_succeeded{0}, g_rejected{0}, g_infrastructureFailed{0};
        std::once_flag g_loadOnce;

#ifdef _WIN32
        void LoadDxc()
        {
            HMODULE module = LoadLibraryW(L"dxcompiler.dll");
            if (!module)
            {
                // Allow custom installations without embedding workstation paths.
                wchar_t configuredPath[32768];
                DWORD length = GetEnvironmentVariableW(L"LO_DXC_PATH", configuredPath, 32768);
                if (length && length < 32768)
                    module = LoadLibraryW(configuredPath);
            }
            if (!module)
            {
                wchar_t programFiles[32768];
                DWORD length = GetEnvironmentVariableW(L"ProgramFiles(x86)", programFiles, 32768);
                std::vector<std::filesystem::path> candidates;
                if (length && length < 32768)
                {
                    const auto sdkBin = std::filesystem::path(programFiles) / L"Windows Kits" / L"10" / L"bin";
                    std::error_code error;
                    std::filesystem::directory_iterator entry(sdkBin, error), end;
                    while (!error && entry != end)
                    {
                        candidates.push_back(entry->path() / L"x64" / L"dxcompiler.dll");
                        entry.increment(error);
                    }
                }
                std::sort(candidates.rbegin(), candidates.rend());
                for (const auto& path : candidates)
                {
                    module = LoadLibraryW(path.c_str());
                    if (module)
                        break;
                }
            }
            if (module)
                g_module = module;
            if (module)
                g_createInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
        }
#else
        void LoadDxc()
        {
#ifdef __APPLE__
            // The repository's macOS library is a universal (arm64 + x86_64) build.
            constexpr const char* kLibraryName = "libdxcompiler.dylib";
            constexpr const char* kRepositoryLibrary = "tools/XenosRecomp/thirdparty/dxc-bin/lib/arm64/libdxcompiler.dylib";
#else
            constexpr const char* kLibraryName = "libdxcompiler.so";
            constexpr const char* kRepositoryLibrary = "tools/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxcompiler.so";
#endif
            std::vector<std::filesystem::path> candidates;
            candidates.push_back(kLibraryName);
            if (const char* envPath = std::getenv("LO_DXC_PATH"); envPath && *envPath)
            {
                candidates.push_back(envPath);
            }
            std::error_code ec;
            const auto cwd = std::filesystem::current_path(ec);
            if (!ec)
            {
                candidates.push_back(cwd / kLibraryName);
            }
            const std::filesystem::path relativeDxc = kRepositoryLibrary;
            if (std::filesystem::exists(relativeDxc, ec))
            {
                candidates.push_back(relativeDxc);
            }

            void* module = nullptr;
            std::filesystem::path loadedCandidate;
            for (const auto& candidate : candidates)
            {
                if (candidate.empty())
                    continue;
                module = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (module)
                {
                    auto proc = reinterpret_cast<DxcCreateInstanceProc>(dlsym(module, "DxcCreateInstance"));
                    if (proc)
                    {
                        g_module = module;
                        g_createInstance = proc;
                        loadedCandidate = candidate;
                        break;
                    }
                    dlclose(module);
                    module = nullptr;
                }
            }

            if (g_module && g_createInstance)
            {
                Dl_info info{};
                if (dladdr(reinterpret_cast<void*>(g_createInstance), &info) && info.dli_fname && info.dli_fname[0] != '\0')
                {
                    std::error_code canonEc;
                    auto p = std::filesystem::canonical(info.dli_fname, canonEc);
                    g_modulePath = canonEc ? std::filesystem::path(info.dli_fname) : p;
                }
                else if (!loadedCandidate.empty())
                {
                    std::error_code canonEc;
                    auto p = std::filesystem::canonical(loadedCandidate, canonEc);
                    g_modulePath = canonEc ? loadedCandidate : p;
                }
            }
        }
#endif

        template<typename T>
        struct ComPtr
        {
            T* p = nullptr;
            ~ComPtr() { if (p) p->Release(); }
            T** operator&() { return &p; }
            T* operator->() { return p; }
            explicit operator bool() const { return p != nullptr; }
        };
    }

    namespace
    {
        const char* FormatName(ShaderBinaryFormat format)
        {
            switch (format)
            {
            case ShaderBinaryFormat::Spirv: return "spirv";
            case ShaderBinaryFormat::MetalIR: return "metal-ir";
            default: return "dxil";
            }
        }

#ifdef __APPLE__
        // IRCompiler instances are not reentrant, so each compiling thread keeps its own.
        struct MetalIRCompiler
        {
            IRCompiler* compiler = IRCompilerCreate();

            MetalIRCompiler()
            {
                if (!compiler) return;
                IRCompilerSetMinimumGPUFamily(compiler, IRGPUFamilyApple7);
                IRCompilerSetMinimumDeploymentTarget(compiler, IROperatingSystem_macOS, "15.0");
                // Guest depth pre-passes and later equal-depth passes need identical positions.
                IRCompilerSetCompatibilityFlags(compiler, IRCompatibilityFlagPositionInvariance);
            }
            ~MetalIRCompiler() { if (compiler) IRCompilerDestroy(compiler); }
        };

        bool MapResourceType(IRResourceType type, plume::MetalIRResourceType& mapped)
        {
            switch (type)
            {
            case IRResourceTypeSRV: mapped = plume::MetalIRResourceType::SRV; return true;
            case IRResourceTypeUAV: mapped = plume::MetalIRResourceType::UAV; return true;
            case IRResourceTypeCBV: mapped = plume::MetalIRResourceType::CBV; return true;
            case IRResourceTypeSampler: mapped = plume::MetalIRResourceType::SAMPLER; return true;
            default: return false;
            }
        }
#endif

        // Replaces DXIL in result with plume's METAL_IR container: resource table, entry point, metallib.
        void ConvertToMetalIR(CompiledShader& result, const char* entryPoint, const char* profile)
        {
            result.ok = false;
#ifdef __APPLE__
            const std::string_view stage(profile);
            const bool pixel = stage.starts_with("ps_");
            if (!pixel && !stage.starts_with("vs_"))
            {
                result.errors += "\nMetal Shader Converter output is only used for vertex and pixel shaders";
                result.deterministicFailure = true;
                return;
            }

            thread_local MetalIRCompiler converter;
            if (!converter.compiler)
            {
                result.errors += "\nMetal Shader Converter unavailable";
                return;
            }

            IRObject* input = IRObjectCreateFromDXIL(result.bytecode.data(), result.bytecode.size(), IRBytecodeOwnershipNone);
            IRError* error = nullptr;
            IRObject* output = input ? IRCompilerAllocCompileAndLink(converter.compiler, entryPoint, input, &error) : nullptr;
            IRMetalLibBinary* metallib = IRMetalLibBinaryCreate();
            IRShaderReflection* reflection = IRShaderReflectionCreate();
            const IRShaderStage irStage = pixel ? IRShaderStageFragment : IRShaderStageVertex;
            bool converted = output && IRObjectGetMetalLibBinary(output, irStage, metallib) &&
                IRObjectGetReflection(output, irStage, reflection);

            std::vector<uint8_t> container;
            if (converted)
            {
                std::vector<IRResourceLocation> locations(IRShaderReflectionGetResourceCount(reflection));
                IRShaderReflectionGetResourceLocations(reflection, locations.data());
                std::vector<plume::MetalIRResource> resources;
                resources.reserve(locations.size());
                for (const auto& location : locations)
                {
                    plume::MetalIRResource resource;
                    if (!MapResourceType(location.resourceType, resource.type))
                    {
                        result.errors += "\nMetal Shader Converter produced an unsupported top-level resource type";
                        converted = false;
                        break;
                    }
                    resource.space = location.space;
                    resource.slot = location.slot;
                    resource.topLevelOffset = location.topLevelOffset;
                    resources.push_back(resource);
                }

                const char* function = IRShaderReflectionGetEntryPointFunctionName(reflection);
                const std::string_view functionName = function ? function : entryPoint;
                const size_t metallibSize = IRMetalLibGetBytecodeSize(metallib);
                if (converted && metallibSize)
                {
                    plume::MetalIRShaderHeader header;
                    header.resourceCount = uint32_t(resources.size());
                    header.entryPointLength = uint32_t(functionName.size());
                    header.metallibSize = metallibSize;
                    const size_t tableBytes = resources.size() * sizeof(plume::MetalIRResource);
                    container.resize(sizeof(header) + tableBytes + functionName.size() + metallibSize);
                    uint8_t* cursor = container.data();
                    std::memcpy(cursor, &header, sizeof(header));
                    cursor += sizeof(header);
                    std::memcpy(cursor, resources.data(), tableBytes);
                    cursor += tableBytes;
                    std::memcpy(cursor, functionName.data(), functionName.size());
                    cursor += functionName.size();
                    IRMetalLibGetBytecode(metallib, cursor);
                }
                else converted = false;
            }

            if (converted)
            {
                result.bytecode = std::move(container);
                result.ok = true;
            }
            else if (!input)
            {
                result.errors += "\nMetal Shader Converter rejected the DXIL container";
                result.deterministicFailure = true;
            }
            else if (error)
            {
                result.errors += "\nMetal Shader Converter error code " + std::to_string(IRErrorGetCode(error));
                result.deterministicFailure = true;
            }
            else if (result.errors.find("Metal Shader Converter") == std::string::npos)
            {
                result.errors += "\nMetal Shader Converter produced no metallib";
            }

            IRShaderReflectionDestroy(reflection);
            IRMetalLibBinaryDestroy(metallib);
            if (error) IRErrorDestroy(error);
            if (output) IRObjectDestroy(output);
            if (input) IRObjectDestroy(input);
#else
            (void)entryPoint;
            (void)profile;
            result.errors += "\nMetal Shader Converter output is only available on macOS";
#endif
        }
    }

    const std::string& ShaderCompilerIdentity(ShaderBinaryFormat format)
    {
#ifdef __APPLE__
        if (format == ShaderBinaryFormat::MetalIR)
        {
            static const std::string identity = DxcIdentity().empty() ? std::string() :
                DxcIdentity() + ";metal-shaderconverter=" + std::to_string(IR_VERSION_MAJOR) + "." + std::to_string(IR_VERSION_MINOR);
            return identity;
        }
#endif
        (void)format;
        return DxcIdentity();
    }

    bool DxcAvailable()
    {
        std::call_once(g_loadOnce, LoadDxc);
        return g_createInstance != nullptr;
    }

    const std::string& DxcIdentity()
    {
        static const std::string identity = []() -> std::string {
            if (!DxcAvailable()) return {};
            try {
                auto hash = [](const std::filesystem::path& path) {
                    std::ifstream in(path, std::ios::binary | std::ios::ate);
                    const auto size = in.tellg();
                    if (size <= 0 || size > 128 * 1024 * 1024) throw std::runtime_error("DXC module read unavailable");
                    std::vector<uint8_t> bytes(static_cast<size_t>(size)); in.seekg(0);
                    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) throw std::runtime_error("DXC module read incomplete");
                    return resources::Sha256Hex(resources::Sha256(bytes));
                };
#ifdef _WIN32
                auto pathOf = [](HMODULE module) {
                    wchar_t path[32768];
                    const DWORD size = GetModuleFileNameW(module, path, 32768);
                    if (!size || size == 32768) throw std::runtime_error("DXC module path unavailable");
                    return std::filesystem::path(path);
                };
                const auto compiler = pathOf(g_module);
                // Retain the actual validator module, including an already
                // loaded one, so its identity cannot drift after certification.
                const auto loadedValidator = GetModuleHandleW(L"dxil.dll");
                const auto validatorPath = loadedValidator ? pathOf(loadedValidator) : compiler.parent_path() / "dxil.dll";
                static HMODULE retainedValidator = LoadLibraryW(validatorPath.c_str());
                if (!retainedValidator) return {};
                return hash(compiler) + ":" + hash(pathOf(retainedValidator));
#else
                if (g_modulePath.empty()) return {};
                return hash(g_modulePath);
#endif
            } catch (...) { return {}; }
        }();
        return identity;
    }

    DxcStatistics GetDxcStatistics()
    {
        return {g_calls.load(), g_succeeded.load(), g_rejected.load(), g_infrastructureFailed.load()};
    }

    static CompiledShader CompileHlslImpl(const std::string& source, const char* entryPoint, const char* profile, ShaderBinaryFormat format, bool debugInfo)
    {
        CompiledShader result;
        if (!DxcAvailable())
        {
            ++g_infrastructureFailed;
#ifdef _WIN32
            result.errors = "dxcompiler.dll not available";
#else
            result.errors = "dxcompiler library not available";
#endif
            return result;
        }

        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        if (FAILED(g_createInstance(kClsidDxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(&utils))) ||
            FAILED(g_createInstance(kClsidDxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&compiler))))
        {
            result.errors = "DxcCreateInstance failed";
            ++g_infrastructureFailed;
            return result;
        }

        std::wstring entry(entryPoint, entryPoint + strlen(entryPoint));
        std::wstring target(profile, profile + strlen(profile));
        std::vector<LPCWSTR> args = {
            L"-E", entry.c_str(),
            L"-T", target.c_str(),
            L"-HV", L"2021",
            L"-Wno-parentheses-equality",
            L"-Wno-unused-value",
            L"-all-resources-bound",
        };
        if (format == ShaderBinaryFormat::Spirv)
        {
            args.insert(args.end(), {L"-spirv", L"-fspv-target-env=vulkan1.2", L"-fvk-use-dx-layout"});
            if (target.starts_with(L"vs_")) args.push_back(L"-fvk-invert-y");
        }
        if (debugInfo && format == ShaderBinaryFormat::Dxil)
        {
            args.push_back(L"-Zi");
            args.push_back(L"-Qembed_debug");
        }
        else
        {
            args.push_back(L"-O3");
            args.push_back(L"-Qstrip_debug");
            if (format == ShaderBinaryFormat::Dxil) args.push_back(L"-Qstrip_reflect");
        }

        DxcBuffer buffer{};
        buffer.Ptr = source.data();
        buffer.Size = source.size();
        buffer.Encoding = DXC_CP_UTF8;

        ComPtr<IDxcResult> compileResult;
        ++g_calls;
        HRESULT hr = compiler->Compile(&buffer, args.data(), uint32_t(args.size()), nullptr, __uuidof(IDxcResult), reinterpret_cast<void**>(&compileResult));
        if (FAILED(hr) || !compileResult)
        {
            result.errors = "IDxcCompiler3::Compile failed";
            ++g_infrastructureFailed;
            return result;
        }

        ComPtr<IDxcBlobUtf8> errors;
        compileResult->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&errors), nullptr);
        if (errors && errors->GetStringLength() > 0)
            result.errors.assign(errors->GetStringPointer(), errors->GetStringLength());

        HRESULT status = E_FAIL;
        compileResult->GetStatus(&status);
        if (FAILED(status)) {
            // Only ordinary source diagnostics are reusable. Internal compiler
            // and allocation failures are transient and must be retried.
            result.deterministicFailure = status != E_OUTOFMEMORY &&
                result.errors.find("error:") != std::string::npos &&
                result.errors.find("out of memory") == std::string::npos &&
                result.errors.find("internal compiler error") == std::string::npos;
            if (result.deterministicFailure) ++g_rejected;
            else ++g_infrastructureFailed;
            return result;
        }

        ComPtr<IDxcBlob> object;
        compileResult->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&object), nullptr);
        if (!object)
        {
            result.errors += "\nno object output";
            ++g_infrastructureFailed;
            return result;
        }
        auto* data = static_cast<const uint8_t*>(object->GetBufferPointer());
        result.bytecode.assign(data, data + object->GetBufferSize());
        result.ok = true;
        ++g_succeeded;
        return result;
    }
}

namespace xenos
{
    CompiledShader CompileHlsl(const std::string& source, const char* entry, const char* profile, ShaderBinaryFormat format, bool debugInfo)
    {
        const auto start = std::chrono::steady_clock::now();
        // Metal Shader Converter input is ordinary DXIL.
        auto result = CompileHlslImpl(source, entry, profile,
            format == ShaderBinaryFormat::MetalIR ? ShaderBinaryFormat::Dxil : format, debugInfo);
        if (result.ok && format == ShaderBinaryFormat::MetalIR)
            ConvertToMetalIR(result, entry, profile);
        // This is compilation-time provenance, not a guest shader or cache key.
        // Do not add content hashing to the cache-hit/per-draw path.
        if (os::shaderlog::Current().IsOpen())
        {
            const auto hash = resources::Sha256Hex(resources::Sha256(
                std::span(reinterpret_cast<const uint8_t*>(source.data()), source.size())));
            os::shaderlog::Log(result.ok ? LogType::Info : LogType::Error,
                result.ok ? "compile-success" : result.deterministicFailure ? "compile-rejected" : "compile-failed",
                os::shaderlog::HashNamespace::HlslSourceSha256,
                "source={} profile={} entry={} format={} debug={} bytes={} elapsed_ms={:.3f}\n{}", hash, profile, entry,
                FormatName(format), debugInfo, result.bytecode.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), result.errors);
        }
        return result;
    }

    CompiledShader CompileHlsl(const std::string& source, const char* entry, const char* profile, bool debugInfo)
    {
        return CompileHlsl(source, entry, profile, ShaderBinaryFormat::Dxil, debugInfo);
    }

    CompiledShader CompileCachedHlsl(const std::string& source, const char* entry, const char* profile, ShaderBinaryFormat format)
    {
        const std::string key = source + '\0' + entry + '\0' + profile + "lo-dxc-vulkan12-dx-layout-v1";
        uint64_t hash = 0xcbf29ce484222325ull;
        for (uint8_t byte : key) { hash ^= byte; hash *= 0x100000001b3ull; }
        const auto backend = format == ShaderBinaryFormat::Spirv ? cache::Backend::Vulkan :
            format == ShaderBinaryFormat::MetalIR ? cache::Backend::Metal : cache::Backend::D3D12;
        auto identity = cache::MakeIdentity(backend, ShaderCompilerIdentity(format));
        identity.variant = "builtin:" + std::to_string(std::strlen(entry)) + ":" + entry +
            ":" + std::to_string(std::strlen(profile)) + ":" + profile;
        const bool pixel = std::string_view(profile).starts_with("ps_");
        const char* configured = std::getenv("LO_SHADER_CACHE_DIR");
        const auto directory = std::filesystem::path(configured ? configured : "cache/shaders") / "builtin";
        const auto path = directory / cache::FileName(pixel, hash, identity);
        CompiledShader result;
        if (!configured || *configured) result.bytecode = cache::ReadBinary(path, pixel, hash, identity);
        if (!result.bytecode.empty()) {
            result.ok = true;
            if (os::shaderlog::Current().IsOpen())
                SHADER_LOG_INFO("cache-hit", BuiltinKeyFnv, "builtin key={:016x} profile={} entry={} format={} bytes={}",
                    hash, profile, entry, FormatName(format), result.bytecode.size());
            return result;
        }
        if (os::shaderlog::Current().IsOpen())
            SHADER_LOG_INFO("cache-miss", BuiltinKeyFnv, "builtin key={:016x} profile={} entry={} format={}",
                hash, profile, entry, FormatName(format));
        result = CompileHlsl(source, entry, profile, format);
        if (result.ok && (!configured || *configured)) {
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (!error) cache::WriteBinary(path, pixel, hash, identity, result.bytecode);
        }
        return result;
    }
}
