//
// Copyright (c) 2008-2022 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../../Precompiled.h"

#include <array>

#include "../../Graphics/Graphics.h"
#include "../../Graphics/GraphicsImpl.h"
#include "../../Graphics/Shader.h"
#include "../../Graphics/VertexBuffer.h"
#include "../../IO/File.h"
#include "../../IO/FileSystem.h"
#include "../../IO/Log.h"
#include "../../Resource/ResourceCache.h"

#include <d3dcompiler.h>

#include "../../DebugNew.h"

using namespace Diligent;

namespace Urho3D
{

static const std::unordered_map<std::string, ShaderParameterGroup> vsShaderParameterGroupMap =
{
    {"FrameVS", SP_FRAME},
    {"CameraVS", SP_CAMERA},
    {"ZoneVS", SP_ZONE},
    {"LightVS", SP_LIGHT},
    {"MaterialVS", SP_MATERIAL},
    {"ObjectVS", SP_OBJECT},
};

static const std::unordered_map<std::string, ShaderParameterGroup> psShaderParameterGroupMap =
{
    {"FramePS", SP_FRAME},
    {"CameraPS", SP_CAMERA},
    {"ZonePS", SP_ZONE},
    {"LightPS", SP_LIGHT},
    {"MaterialPS", SP_MATERIAL},
};

const char* ShaderVariation::elementSemanticNames[] =
{
    "POSITION",
    "NORMAL",
    "BINORMAL",
    "TANGENT",
    "TEXCOORD",
    "COLOR",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "OBJECTINDEX"
};

void ShaderVariation::OnDeviceLost()
{
    // No-op on Diligent
}

bool ShaderVariation::Create()
{
    Release();

    if (!graphics_)
        return false;

    if (!owner_)
    {
        compilerOutput_ = "Owner shader has expired";
        return false;
    }

    // Check for up-to-date bytecode on disk
    String path, name, extension;
    SplitPath(owner_->GetName(), path, name, extension);
    extension = type_ == VS ? ".vs4" : ".ps4";

    auto getDeviceTypeName = [](const RENDER_DEVICE_TYPE deviceType)
    {
        switch (deviceType)
        {
        case RENDER_DEVICE_TYPE_D3D11:
            return "d3d11";
        case RENDER_DEVICE_TYPE_D3D12:
            return "d3d12";
        case RENDER_DEVICE_TYPE_GL:
            return "gl";
        case RENDER_DEVICE_TYPE_GLES:
            return "gles";
        case RENDER_DEVICE_TYPE_VULKAN:
            return "vulkan";
        case RENDER_DEVICE_TYPE_METAL:
            return "metal";
        default:
            return "unknown";
        }
    };

    String binaryShaderName = graphics_->GetShaderCacheDir() + name + "_Diligent" +
                              getDeviceTypeName(graphics_->GetImpl()->GetDeviceType()) + "_" +
                              StringHash(defines_).ToString() + extension;

    if (!LoadByteCode(binaryShaderName))
    {
        // Compile shader if don't have valid bytecode
        if (CompileToBinary())
        {
            // Save the bytecode after successful compile, but not if the source is from a package
            if (owner_->GetTimeStamp())
                SaveByteCode(binaryShaderName);

            CreateFromBinary();
        }
        else
        {
            CreateFromSource();
        }
    }
    else
    {
        CreateFromSource();
    }

    // Update parameters
    if (object_.ptr_ != nullptr)
    {
        IShader* shader = (IShader*)object_.ptr_;

        const auto& shaderParameterGroupMap = type_ == VS ? vsShaderParameterGroupMap : psShaderParameterGroupMap;

        printf("Resources for shader %s\n", GetFullName().CString());

        const unsigned parameterGroupCount = shader->GetResourceCount();

        struct ResourceData
        {
            unsigned index_;
            const char* name_;
            unsigned shaderParameterGroup_;
        };
        std::vector<ResourceData> constantResources;
        std::array<bool, MAX_SHADER_PARAMETER_GROUPS> usedParameterGroups;

        constantResources.reserve(parameterGroupCount);
        usedParameterGroups.fill(false);

        for (unsigned i = 0; i < parameterGroupCount; i++)
        {
            ShaderResourceDesc srd;
            shader->GetResourceDesc(i, srd);

            if (srd.Type == SHADER_RESOURCE_TYPE_CONSTANT_BUFFER)
            {
                unsigned shaderParameterGroup = (unsigned)MAX_SHADER_PARAMETER_GROUPS;
                auto shaderParameterGroupIter = shaderParameterGroupMap.find(srd.Name);
                if (shaderParameterGroupIter != shaderParameterGroupMap.end())
                {
                    shaderParameterGroup = (unsigned)shaderParameterGroupIter->second;
                    usedParameterGroups[shaderParameterGroup] = true;
                }
                constantResources.push_back(ResourceData{i, srd.Name, shaderParameterGroup});
            }
        }

        unsigned nextGroupIndex = 0;
        for (auto& resource : constantResources)
        {
            if (resource.shaderParameterGroup_ >= (unsigned)MAX_SHADER_PARAMETER_GROUPS)
            {
                for (unsigned i = nextGroupIndex; i < MAX_SHADER_PARAMETER_GROUPS; i++)
                {
                    if (!usedParameterGroups[i])
                    {
                        resource.shaderParameterGroup_ = i;
                        usedParameterGroups[i] = true;
                        nextGroupIndex = i + 1;
                        break;
                    }
                }
            }
        }

        unsigned customShaderParameterGroupCount = 0;
        for (auto& resource : constantResources)
        {
            const char* shaderParameterName = resource.name_;
            const unsigned shaderParameterGroup = resource.shaderParameterGroup_;
            const auto* constantBufferDesc = shader->GetConstantBufferDesc(resource.index_);

            if (constantBufferDesc != nullptr)
            {
                // TODO: Remove before commit
#if 0
                printf("\tResource %s: pg=%u sz=%u vc=%u\n", shaderParameterName, shaderParameterGroup,
                       constantBufferDesc->Size, constantBufferDesc->NumVariables);
#endif
                constantBufferSizes_[shaderParameterGroup] = constantBufferDesc->Size;
                constantBufferNames_[shaderParameterGroup] = shaderParameterName;

                for (uint32_t j = 0; j < constantBufferDesc->NumVariables; j++)
                {
                    const auto& variable = constantBufferDesc->pVariables[j];

                // TODO: Remove before commit
#if 0
                    printf("\t\tVariable %s: tn=%s bt=%d offset=%u\n", variable.Name, variable.TypeName,
                           variable.BasicType, variable.Offset);
#endif

                    if (variable.Name[0] == 'c')
                    {
                        const char* variableName = &variable.Name[1];
                        parameters_[StringHash(variableName)] =
                            ShaderParameter{type_, variableName, variable.Offset, 0, shaderParameterGroup};
                    }
                }
            }
            else
            {
                printf("\tResource %s: pg=%u\n", shaderParameterName, shaderParameterGroup);
            }
        }

    }

    return object_.ptr_ != nullptr;
}

void ShaderVariation::Release()
{
#if 0
    if (object_.ptr_)
    {
        if (!graphics_)
            return;

        graphics_->CleanupShaderPrograms(this);

        if (type_ == VS)
        {
            if (graphics_->GetVertexShader() == this)
                graphics_->SetShaders(nullptr, nullptr);
        }
        else
        {
            if (graphics_->GetPixelShader() == this)
                graphics_->SetShaders(nullptr, nullptr);
        }

        URHO3D_SAFE_RELEASE(object_.ptr_);
    }

    compilerOutput_.Clear();

    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
        useTextureUnits_[i] = false;
    for (unsigned i = 0; i < MAX_SHADER_PARAMETER_GROUPS; ++i)
        constantBufferSizes_[i] = 0;
    parameters_.Clear();
    byteCode_.Clear();
    elementHash_ = 0;
#else
    PARTIALLY_IMPLEMENTED();

    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
        useTextureUnits_[i] = false;
    for (unsigned i = 0; i < MAX_SHADER_PARAMETER_GROUPS; ++i)
    {
        constantBufferSizes_[i] = 0;
        constantBufferNames_[i] = String::EMPTY;
    }
    parameters_.Clear();
    elementHash_ = 0;
#endif
}

void ShaderVariation::SetDefines(const String& defines)
{
    defines_ = defines;

    // Internal mechanism for appending the CLIPPLANE define, prevents runtime (every frame) string manipulation
    definesClipPlane_ = defines;
    if (!definesClipPlane_.EndsWith(" CLIPPLANE"))
        definesClipPlane_ += " CLIPPLANE";
}

bool ShaderVariation::LoadByteCode(const String& binaryShaderName)
{
#if 0
    ResourceCache* cache = owner_->GetSubsystem<ResourceCache>();
    if (!cache->Exists(binaryShaderName))
        return false;

    FileSystem* fileSystem = owner_->GetSubsystem<FileSystem>();
    unsigned sourceTimeStamp = owner_->GetTimeStamp();
    // If source code is loaded from a package, its timestamp will be zero. Else check that binary is not older
    // than source
    if (sourceTimeStamp && fileSystem->GetLastModifiedTime(cache->GetResourceFileName(binaryShaderName)) < sourceTimeStamp)
        return false;

    SharedPtr<File> file = cache->GetFile(binaryShaderName);
    if (!file || file->ReadFileID() != "USHD")
    {
        URHO3D_LOGERROR(binaryShaderName + " is not a valid shader bytecode file");
        return false;
    }

    /// \todo Check that shader type and model match
    /*unsigned short shaderType = */file->ReadUShort();
    /*unsigned short shaderModel = */file->ReadUShort();
    elementHash_ = file->ReadUInt();
    elementHash_ <<= 32;

    unsigned numParameters = file->ReadUInt();
    for (unsigned i = 0; i < numParameters; ++i)
    {
        String name = file->ReadString();
        unsigned buffer = file->ReadUByte();
        unsigned offset = file->ReadUInt();
        unsigned size = file->ReadUInt();

        parameters_[StringHash(name)] = ShaderParameter{type_, name, offset, size, buffer};
    }

    unsigned numTextureUnits = file->ReadUInt();
    for (unsigned i = 0; i < numTextureUnits; ++i)
    {
        /*String unitName = */file->ReadString();
        unsigned reg = file->ReadUByte();

        if (reg < MAX_TEXTURE_UNITS)
            useTextureUnits_[reg] = true;
    }

    unsigned byteCodeSize = file->ReadUInt();
    if (byteCodeSize)
    {
        byteCode_.Resize(byteCodeSize);
        file->Read(&byteCode_[0], byteCodeSize);

        if (type_ == VS)
            URHO3D_LOGDEBUG("Loaded cached vertex shader " + GetFullName());
        else
            URHO3D_LOGDEBUG("Loaded cached pixel shader " + GetFullName());

        CalculateConstantBufferSizes();
        return true;
    }
    else
    {
        URHO3D_LOGERROR(binaryShaderName + " has zero length bytecode");
        return false;
    }
#else
    NOT_IMPLEMENTED();

    return false;
#endif
}

bool ShaderVariation::CompileToBinary()
{
#if 0
    const String& sourceCode = owner_->GetSourceCode(type_);
    Vector<String> defines = defines_.Split(' ');

    // Set the entrypoint, profile and flags according to the shader being compiled
    const char* entryPoint = nullptr;
    const char* profile = nullptr;
    unsigned flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

    defines.Push("D3D11");

    if (type_ == VS)
    {
        entryPoint = "VS";
        defines.Push("COMPILEVS");
        profile = "vs_4_0";
    }
    else
    {
        entryPoint = "PS";
        defines.Push("COMPILEPS");
        profile = "ps_4_0";
        flags |= D3DCOMPILE_PREFER_FLOW_CONTROL;
    }

    defines.Push("MAXBONES=" + String(Graphics::GetMaxBones()));

    // Collect defines into macros
    Vector<String> defineValues;
    PODVector<D3D_SHADER_MACRO> macros;

    for (unsigned i = 0; i < defines.Size(); ++i)
    {
        unsigned equalsPos = defines[i].Find('=');
        if (equalsPos != String::NPOS)
        {
            defineValues.Push(defines[i].Substring(equalsPos + 1));
            defines[i].Resize(equalsPos);
        }
        else
            defineValues.Push("1");
    }
    for (unsigned i = 0; i < defines.Size(); ++i)
    {
        D3D_SHADER_MACRO macro;
        macro.Name = defines[i].CString();
        macro.Definition = defineValues[i].CString();
        macros.Push(macro);

        // In debug mode, check that all defines are referenced by the shader code
#ifdef _DEBUG
        if (sourceCode.Find(defines[i]) == String::NPOS)
            URHO3D_LOGWARNING("Shader " + GetFullName() + " does not use the define " + defines[i]);
#endif
    }

    D3D_SHADER_MACRO endMacro;
    endMacro.Name = nullptr;
    endMacro.Definition = nullptr;
    macros.Push(endMacro);

    // Compile using D3DCompile
    ID3DBlob* shaderCode = nullptr;
    ID3DBlob* errorMsgs = nullptr;

    HRESULT hr = D3DCompile(sourceCode.CString(), sourceCode.Length(), owner_->GetName().CString(), &macros.Front(), nullptr,
        entryPoint, profile, flags, 0, &shaderCode, &errorMsgs);
    if (FAILED(hr))
    {
        // Do not include end zero unnecessarily
        compilerOutput_ = String((const char*)errorMsgs->GetBufferPointer(), (unsigned)errorMsgs->GetBufferSize() - 1);
    }
    else
    {
        if (type_ == VS)
            URHO3D_LOGDEBUG("Compiled vertex shader " + GetFullName());
        else
            URHO3D_LOGDEBUG("Compiled pixel shader " + GetFullName());

        unsigned char* bufData = (unsigned char*)shaderCode->GetBufferPointer();
        unsigned bufSize = (unsigned)shaderCode->GetBufferSize();
        // Use the original bytecode to reflect the parameters
        ParseParameters(bufData, bufSize);
        CalculateConstantBufferSizes();

        // Then strip everything not necessary to use the shader
        ID3DBlob* strippedCode = nullptr;
        D3DStripShader(bufData, bufSize,
            D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_TEST_BLOBS, &strippedCode);
        byteCode_.Resize((unsigned)strippedCode->GetBufferSize());
        memcpy(&byteCode_[0], strippedCode->GetBufferPointer(), byteCode_.Size());
        strippedCode->Release();
    }

    URHO3D_SAFE_RELEASE(shaderCode);
    URHO3D_SAFE_RELEASE(errorMsgs);

    return !byteCode_.Empty();
#else
#if 0
    const String& sourceCode = owner_->GetSourceCode(type_);
    Vector<String> defines = defines_.Split(' ');

    // Set the entrypoint, profile and flags according to the shader being compiled
    const char* entryPoint = nullptr;
    const char* profile = nullptr;
    unsigned flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

    defines.Push("D3D11");

    if (type_ == VS)
    {
        entryPoint = "VS";
        defines.Push("COMPILEVS");
        profile = "vs_4_0";
    }
    else
    {
        entryPoint = "PS";
        defines.Push("COMPILEPS");
        profile = "ps_4_0";
        flags |= D3DCOMPILE_PREFER_FLOW_CONTROL;
    }

    defines.Push("MAXBONES=" + String(Graphics::GetMaxBones()));

    // Collect defines into macros
    Vector<String> defineValues;
    ShaderMacroHelper macros;

    for (unsigned i = 0; i < defines.Size(); ++i)
    {
        unsigned equalsPos = defines[i].Find('=');
        if (equalsPos != String::NPOS)
        {
            defineValues.Push(defines[i].Substring(equalsPos + 1));
            defines[i].Resize(equalsPos);
        }
        else
            defineValues.Push("1");
    }
    for (unsigned i = 0; i < defines.Size(); ++i)
    {
        macros.AddShaderMacro(defines[i].CString(), defineValues[i].CString());

        // In debug mode, check that all defines are referenced by the shader code
    #ifdef _DEBUG
        if (sourceCode.Find(defines[i]) == String::NPOS)
            URHO3D_LOGWARNING("Shader " + GetFullName() + " does not use the define " + defines[i]);
    #endif
    }

    ShaderCreateInfo shaderCreateInfo;
    memset(&shaderCreateInfo, 0, sizeof(shaderCreateInfo));
    shaderCreateInfo.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCreateInfo.UseCombinedTextureSamplers = true;
    shaderCreateInfo.CombinedSamplerSuffix = "_sampler";
    shaderCreateInfo.Desc.ShaderType = type_ == VS ? SHADER_TYPE_VERTEX : SHADER_TYPE_PIXEL;
    shaderCreateInfo.EntryPoint = entryPoint;
    shaderCreateInfo.Source = sourceCode.CString();
    shaderCreateInfo.SourceLength = sourceCode.Length();
    shaderCreateInfo.Macros = macros;
    shaderCreateInfo.HLSLVersion = {4, 0};

    IShader* shader;
    graphics_->GetImpl()->GetDevice()->CreateShader(shaderCreateInfo, &shader);
    if (shader == nullptr)
    {
        compilerOutput_ = "Could not create vertex shader";
    }
    else
    {
        ID3DBlob* shaderCode = shader->GetBytecode();

        if (type_ == VS)
            URHO3D_LOGDEBUG("Compiled vertex shader " + GetFullName());
        else
            URHO3D_LOGDEBUG("Compiled pixel shader " + GetFullName());

        unsigned char* bufData = (unsigned char*)shaderCode->GetBufferPointer();
        unsigned bufSize = (unsigned)shaderCode->GetBufferSize();
        // Use the original bytecode to reflect the parameters
        ParseParameters(bufData, bufSize);
        CalculateConstantBufferSizes();

        // Then strip everything not necessary to use the shader
        ID3DBlob* strippedCode = nullptr;
        D3DStripShader(bufData, bufSize,
                       D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_TEST_BLOBS,
                       &strippedCode);
        byteCode_.Resize((unsigned)strippedCode->GetBufferSize());
        memcpy(&byteCode_[0], strippedCode->GetBufferPointer(), byteCode_.Size());
        strippedCode->Release();
    }

    URHO3D_SAFE_RELEASE(shaderCode);

    return !byteCode_.Empty();
#else
    NOT_IMPLEMENTED();
    return false;
#endif
#endif
}

void ShaderVariation::CreateFromBinary()
{
#if 0
    // Then create shader from the bytecode
    IRenderDevice* device = graphics_->GetImpl()->GetDevice();
    if (type_ == VS)
    {
        if (device && byteCode_.Size())
        {
            ShaderCreateInfo shaderCreateInfo;
            memset(&shaderCreateInfo, 0, sizeof(shaderCreateInfo));
            shaderCreateInfo.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
            shaderCreateInfo.UseCombinedTextureSamplers = true;
            shaderCreateInfo.CombinedSamplerSuffix = "_sampler";
            shaderCreateInfo.Desc.ShaderType = SHADER_TYPE_VERTEX;
            shaderCreateInfo.EntryPoint = "VS";
            shaderCreateInfo.ByteCode = &byteCode_[0];
            shaderCreateInfo.ByteCodeSize = byteCode_.Size();

            device->CreateShader(shaderCreateInfo, (IShader**)&object_.ptr_);
            if (object_.ptr_ == nullptr)
            {
                compilerOutput_ = "Could not create vertex shader";
            }
        }
        else
            compilerOutput_ = "Could not create vertex shader, empty bytecode";
    }
    else
    {
        if (device && byteCode_.Size())
        {
            ShaderCreateInfo shaderCreateInfo;
            memset(&shaderCreateInfo, 0, sizeof(shaderCreateInfo));
            shaderCreateInfo.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
            shaderCreateInfo.UseCombinedTextureSamplers = true;
            shaderCreateInfo.CombinedSamplerSuffix = "_sampler";
            shaderCreateInfo.Desc.ShaderType = SHADER_TYPE_PIXEL;
            shaderCreateInfo.EntryPoint = "PS";
            shaderCreateInfo.ByteCode = &byteCode_[0];
            shaderCreateInfo.ByteCodeSize = byteCode_.Size();

            device->CreateShader(shaderCreateInfo, (IShader**)&object_.ptr_);
            if (object_.ptr_ == nullptr)
            {
                URHO3D_SAFE_RELEASE(object_.ptr_);
                compilerOutput_ = "Could not create pixel shader";
            }
        }
        else
            compilerOutput_ = "Could not create pixel shader, empty bytecode";
    }
#else
    NOT_IMPLEMENTED();
#endif
}

void ShaderVariation::CreateFromSource()
{
    const String& sourceCode = owner_->GetSourceCode(type_);
    Vector<String> defines = defines_.Split(' ');

    // Set the entrypoint, profile and flags according to the shader being compiled
    const char* entryPoint = nullptr;
    const char* profile = nullptr;
    unsigned flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

    defines.Push("DILIGENT");

    if (type_ == VS)
    {
        entryPoint = "VS";
        defines.Push("COMPILEVS");
        profile = "vs_4_0";
    }
    else
    {
        entryPoint = "PS";
        defines.Push("COMPILEPS");
        profile = "ps_4_0";
        flags |= D3DCOMPILE_PREFER_FLOW_CONTROL;
    }

    defines.Push("MAXBONES=" + String(Graphics::GetMaxBones()));

    // Collect defines into macros
    Vector<String> defineValues;
    ShaderMacroHelper macros;

    for (unsigned i = 0; i < defines.Size(); ++i)
    {
        unsigned equalsPos = defines[i].Find('=');
        if (equalsPos != String::NPOS)
        {
            defineValues.Push(defines[i].Substring(equalsPos + 1));
            defines[i].Resize(equalsPos);
        }
        else
            defineValues.Push("1");
    }
    for (unsigned i = 0; i < defines.Size(); ++i)
    {
        macros.AddShaderMacro(defines[i].CString(), defineValues[i].CString());

        // In debug mode, check that all defines are referenced by the shader code
#ifdef _DEBUG
        if (sourceCode.Find(defines[i]) == String::NPOS)
            URHO3D_LOGWARNING("Shader " + GetFullName() + " does not use the define " + defines[i]);
#endif
    }

    auto shaderFullName = GetFullName();

    ShaderCreateInfo shaderCreateInfo;
    // TODO: Remove
//    memset(&shaderCreateInfo, 0, sizeof(shaderCreateInfo));
    shaderCreateInfo.Desc.Name = shaderFullName.CString();
    shaderCreateInfo.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCreateInfo.Desc.UseCombinedTextureSamplers = true;
    shaderCreateInfo.Desc.CombinedSamplerSuffix = "_sampler";
    shaderCreateInfo.Desc.ShaderType = type_ == VS ? SHADER_TYPE_VERTEX : SHADER_TYPE_PIXEL;
    shaderCreateInfo.EntryPoint = entryPoint;
    shaderCreateInfo.Source = sourceCode.CString();
    shaderCreateInfo.SourceLength = sourceCode.Length();
    shaderCreateInfo.Macros = macros;
    shaderCreateInfo.LoadConstantBufferReflection = true;
    shaderCreateInfo.HLSLVersion = {5, 0};

    // TODO: Remove
#if 0
    const String fileName = GetFullName() + "_source_code_" + (type_ == VS ? "vs" : "ps") + ".txt";
    const char* fileNameCString = fileName.CString();
    const char* sourceCodeCString = sourceCode.CString();
    FILE* f = fopen(fileNameCString, "w");
    fprintf(f, "%s", sourceCodeCString);
    fclose(f);
#endif

    object_.ptr_ = nullptr;
    graphics_->GetImpl()->GetDevice()->CreateShader(shaderCreateInfo, (IShader**)&object_.ptr_);
    if (object_.ptr_ == nullptr)
    {
        compilerOutput_ = type_ == VS ? "Could not create vertex shader" : "Could not create pixel shader";
    }
    else
    {
        if (type_ == VS)
            URHO3D_LOGDEBUG("Compiled vertex shader " + GetFullName());
        else
            URHO3D_LOGDEBUG("Compiled pixel shader " + GetFullName());
    }
}

void ShaderVariation::ParseParameters(unsigned char* bufData, unsigned bufSize)
{
#if 0
    ID3D11ShaderReflection* reflection = nullptr;
    D3D11_SHADER_DESC shaderDesc;

    HRESULT hr = D3DReflect(bufData, bufSize, IID_ID3D11ShaderReflection, (void**)&reflection);
    if (FAILED(hr) || !reflection)
    {
        URHO3D_SAFE_RELEASE(reflection);
        URHO3D_LOGD3DERROR("Failed to reflect vertex shader's input signature", hr);
        return;
    }

    reflection->GetDesc(&shaderDesc);

    if (type_ == VS)
    {
        unsigned elementHash = 0;
        for (unsigned i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D11_SIGNATURE_PARAMETER_DESC paramDesc;
            reflection->GetInputParameterDesc((UINT)i, &paramDesc);
            VertexElementSemantic semantic = (VertexElementSemantic)GetStringListIndex(paramDesc.SemanticName, elementSemanticNames, MAX_VERTEX_ELEMENT_SEMANTICS, true);
            if (semantic != MAX_VERTEX_ELEMENT_SEMANTICS)
            {
                CombineHash(elementHash, semantic);
                CombineHash(elementHash, paramDesc.SemanticIndex);
            }
        }
        elementHash_ = elementHash;
        elementHash_ <<= 32;
    }

    HashMap<String, unsigned> cbRegisterMap;

    for (unsigned i = 0; i < shaderDesc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC resourceDesc;
        reflection->GetResourceBindingDesc(i, &resourceDesc);
        String resourceName(resourceDesc.Name);
        if (resourceDesc.Type == D3D_SIT_CBUFFER)
            cbRegisterMap[resourceName] = resourceDesc.BindPoint;
        else if (resourceDesc.Type == D3D_SIT_SAMPLER && resourceDesc.BindPoint < MAX_TEXTURE_UNITS)
            useTextureUnits_[resourceDesc.BindPoint] = true;
    }

    for (unsigned i = 0; i < shaderDesc.ConstantBuffers; ++i)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC cbDesc;
        cb->GetDesc(&cbDesc);
        unsigned cbRegister = cbRegisterMap[String(cbDesc.Name)];

        for (unsigned j = 0; j < cbDesc.Variables; ++j)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC varDesc;
            var->GetDesc(&varDesc);
            String varName(varDesc.Name);
            if (varName[0] == 'c')
            {
                varName = varName.Substring(1); // Strip the c to follow Urho3D constant naming convention
                parameters_[varName] = ShaderParameter{type_, varName, varDesc.StartOffset, varDesc.Size, cbRegister};
            }
        }
    }

    reflection->Release();
#else
    NOT_IMPLEMENTED();
#endif
}

void ShaderVariation::SaveByteCode(const String& binaryShaderName)
{
#if 0
    ResourceCache* cache = owner_->GetSubsystem<ResourceCache>();
    FileSystem* fileSystem = owner_->GetSubsystem<FileSystem>();

    // Filename may or may not be inside the resource system
    String fullName = binaryShaderName;
    if (!IsAbsolutePath(fullName))
    {
        // If not absolute, use the resource dir of the shader
        String shaderFileName = cache->GetResourceFileName(owner_->GetName());
        if (shaderFileName.Empty())
            return;
        fullName = shaderFileName.Substring(0, shaderFileName.Find(owner_->GetName())) + binaryShaderName;
    }
    String path = GetPath(fullName);
    if (!fileSystem->DirExists(path))
        fileSystem->CreateDir(path);

    SharedPtr<File> file(new File(owner_->GetContext(), fullName, FILE_WRITE));
    if (!file->IsOpen())
        return;

    file->WriteFileID("USHD");
    file->WriteShort((unsigned short)type_);
    file->WriteShort(4);
    file->WriteUInt(elementHash_ >> 32);

    file->WriteUInt(parameters_.Size());
    for (HashMap<StringHash, ShaderParameter>::ConstIterator i = parameters_.Begin(); i != parameters_.End(); ++i)
    {
        file->WriteString(i->second_.name_);
        file->WriteUByte((unsigned char)i->second_.buffer_);
        file->WriteUInt(i->second_.offset_);
        file->WriteUInt(i->second_.size_);
    }

    unsigned usedTextureUnits = 0;
    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
    {
        if (useTextureUnits_[i])
            ++usedTextureUnits;
    }
    file->WriteUInt(usedTextureUnits);
    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
    {
        if (useTextureUnits_[i])
        {
            file->WriteString(graphics_->GetTextureUnitName((TextureUnit)i));
            file->WriteUByte((unsigned char)i);
        }
    }

    file->WriteUInt(byteCode_.Size());
    if (byteCode_.Size())
        file->Write(&byteCode_[0], byteCode_.Size());
#else
    NOT_IMPLEMENTED();
#endif
}

void ShaderVariation::CalculateConstantBufferSizes()
{
    // Not used in Diligent
}

}
