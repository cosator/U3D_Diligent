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

#pragma once

#include <Graphics/GraphicsEngineD3D11/interface/EngineFactoryD3D11.h>
#include <Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h>
#include <Graphics/GraphicsEngineOpenGL/interface/EngineFactoryOpenGL.h>
#include <Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h>

#include <Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <Graphics/GraphicsEngine/interface/SwapChain.h>

#include <Graphics/GraphicsTools/interface/ShaderMacroHelper.hpp>

#include <Common/interface/RefCntAutoPtr.hpp>

#include "../../Graphics/ConstantBuffer.h"
#include "../../Graphics/GraphicsDefs.h"
#include "../../Graphics/ShaderProgram.h"
#include "../../Graphics/VertexDeclaration.h"
#include "../../Math/Color.h"

#include <d3d11.h>
#include <dxgi.h>

// TODO: Remove
#ifndef __FUNCTION_NAME__
#ifdef WIN32 // WINDOWS
#define __FUNCTION_NAME__ __FUNCTION__
#else //*NIX
#define __FUNCTION_NAME__ __func__
#endif
#endif
#define NOT_IMPLEMENTED()                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        static bool first = true;                                                                                      \
        if (first)                                                                                                     \
        {                                                                                                              \
            first = false;                                                                                             \
            printf("Function %s not implemented\n", __FUNCTION_NAME__);                                                \
        }                                                                                                              \
    } while (false)

#define PARTIALLY_IMPLEMENTED()                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        static bool first = true;                                                                                      \
        if (first)                                                                                                     \
        {                                                                                                              \
            first = false;                                                                                             \
            printf("Function %s partially implemented\n", __FUNCTION_NAME__);                                          \
        }                                                                                                              \
    } while (false)

namespace std
{
namespace
{

// Code from boost
// Reciprocal of the golden ratio helps spread entropy
//     and handles duplicates.
// See Mike Seymour in magic-numbers-in-boosthash-combine:
//     https://stackoverflow.com/questions/4948780

template <class T> inline void hash_combine(std::size_t& seed, T const& v)
{
    seed ^= hash<T>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// Recursive template code derived from Matthieu M.
template <class Tuple, size_t Index = std::tuple_size<Tuple>::value - 1> struct HashValueImpl
{
    static void apply(size_t& seed, Tuple const& tuple)
    {
        HashValueImpl<Tuple, Index - 1>::apply(seed, tuple);
        hash_combine(seed, get<Index>(tuple));
    }
};

template <class Tuple> struct HashValueImpl<Tuple, 0>
{
    static void apply(size_t& seed, Tuple const& tuple) { hash_combine(seed, get<0>(tuple)); }
};
} // namespace

template <typename... TT> struct hash<std::tuple<TT...>>
{
    size_t operator()(std::tuple<TT...> const& tt) const
    {
        size_t seed = 0;
        HashValueImpl<std::tuple<TT...>>::apply(seed, tt);
        return seed;
    }
};
} // namespace std

namespace Urho3D
{

#define URHO3D_SAFE_RELEASE(p) if (p) { ((Diligent::IObject*)p)->Release();  p = 0; }

#define URHO3D_LOGD3DERROR(msg, hr) URHO3D_LOGERRORF("%s (HRESULT %x)", msg, (unsigned)hr)

using ShaderProgramMap = HashMap<Pair<ShaderVariation*, ShaderVariation*>, SharedPtr<ShaderProgram> >;
using VertexDeclarationMap = HashMap<unsigned long long, SharedPtr<VertexDeclaration> >;
using ConstantBufferMap = HashMap<unsigned, SharedPtr<ConstantBuffer> >;

/// %Graphics implementation. Holds API-specific objects.
class URHO3D_API GraphicsImpl
{
    friend class Graphics;

public:
    /// Construct.
    GraphicsImpl();

    /// Return Diligent render device.
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> GetDevice() const { return device_; }

    /// Return Diligent render device type.
    Diligent::RENDER_DEVICE_TYPE GetDeviceType() const { return deviceType_; }

    /// Return Diligent immediate device context.
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> GetDeviceContext() const { return deviceContext_; }

    /// Return swapchain.
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> GetSwapChain() const { return swapChain_; }

    /// Return whether multisampling is supported for a given texture format and sample count.
    bool CheckMultiSampleSupport(Diligent::TEXTURE_FORMAT format, unsigned sampleCount) const;

    /// Return multisample quality level for a given texture format and sample count. The sample count must be supported. On D3D feature level 10.1+, uses the standard level. Below that uses the best quality.
    unsigned GetMultiSampleQuality(Diligent::TEXTURE_FORMAT format, unsigned sampleCount) const;

    void SetPrimitiveType(const PrimitiveType primitiveType);

    Diligent::PRIMITIVE_TOPOLOGY GetPrimitiveTopology();

private:
    struct PipelineState
    {
        Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipelineState_;
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> shaderResourceBinding_;

        struct TextureMapEntry
        {
            unsigned textureUnit;
            Diligent::IShaderResourceVariable* variable;
        };
        typedef std::vector<TextureMapEntry> TextureMap;
        std::shared_ptr<TextureMap> textureMap_;
    };

    Diligent::SwapChainDesc swapChainInitDesc_;
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> deviceContext_;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain_;
    std::unordered_map<
        std::tuple<ShaderVariation*, ShaderVariation*, unsigned, unsigned, unsigned, unsigned long long, PrimitiveType, uint8_t>,
        PipelineState> pipelineStates_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> currentPipelineState_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> currentShaderResourceBinding_;
    std::shared_ptr<PipelineState::TextureMap> currentTextureMap_;
    Diligent::RENDER_DEVICE_TYPE deviceType_ = Diligent::RENDER_DEVICE_TYPE_D3D11;

    /// Default (backbuffer) rendertarget view.
    Diligent::ITextureView* defaultRenderTargetView_ = nullptr;
    /// Default depth-stencil view.
    Diligent::ITextureView* defaultDepthStencilView_ = nullptr;
    /// Current color rendertarget views.
    Diligent::ITextureView* renderTargetViews_[MAX_RENDERTARGETS] = {nullptr};
    /// Current depth-stencil view.
    Diligent::ITextureView* depthStencilView_ = nullptr;

    /// Intermediate texture for multisampled screenshots and less than whole viewport multisampled resolve, created on
    /// demand.
    Diligent::ITexture* resolveTexture_ = nullptr;

    /// Rendertargets dirty flag.
    bool renderTargetsDirty_;
    /// Textures dirty flag.
    bool texturesDirty_;
    /// Vertex declaration dirty flag.
    bool vertexDeclarationDirty_;
    /// Vertex declaration dirty flag.
    bool vertexShaderDirty_ = false;
    /// Vertex declaration dirty flag.
    bool pixelShaderDirty_ = false;
    /// Blend state dirty flag.
    bool blendStateDirty_;
    /// Depth state dirty flag.
    bool depthStateDirty_;
    /// Rasterizer state dirty flag.
    bool rasterizerStateDirty_;
    /// Scissor rect dirty flag.
    bool scissorRectDirty_;
    /// Stencil ref dirty flag.
    bool stencilRefDirty_;
    /// First dirtied texture unit.
    unsigned firstDirtyTexture_;
    /// Last dirtied texture unit.
    unsigned lastDirtyTexture_;

    /// Bound shader resource views.
    Diligent::ITextureView* shaderResourceViews_[MAX_TEXTURE_UNITS];
    /// Bound sampler state objects.
    Diligent::ISampler* samplers_[MAX_TEXTURE_UNITS];

    /// Bound vertex buffers.
    Diligent::IBuffer* vertexBuffers_[MAX_VERTEX_STREAMS];
    /// Bound constant buffers.
    Diligent::IBuffer* constantBuffers_[2][MAX_SHADER_PARAMETER_GROUPS];
    /// Vertex sizes per buffer.
    unsigned vertexSizes_[MAX_VERTEX_STREAMS];
    /// Vertex stream offsets per buffer.
    Diligent::Uint64 vertexOffsets_[MAX_VERTEX_STREAMS];

    /// First dirtied vertex buffer.
    unsigned firstDirtyVB_ = M_MAX_UNSIGNED;
    /// Last dirtied vertex buffer.
    unsigned lastDirtyVB_ = M_MAX_UNSIGNED;

    /// Constant buffer search map.
    ConstantBufferMap allConstantBuffers_;
    /// Currently dirty constant buffers.
    PODVector<ConstantBuffer*> dirtyConstantBuffers_;
    /// Shader programs.
    ShaderProgramMap shaderPrograms_;
    /// Shader program in use.
    ShaderProgram* shaderProgram_;

    /// Hash of current blend state.
    unsigned blendStateHash_;
    /// Hash of current depth state.
    unsigned depthStateHash_;
    /// Hash of current rasterizer state.
    unsigned rasterizerStateHash_;

    /// Current draw call primitive type.
    PrimitiveType primitiveType_ = TRIANGLE_LIST;
    /// Primitive type dirty flag.
    bool primitiveTypeDirty_ = false;

    uint8_t renderTargetHash_ = 0;

#if 0
    /// Graphics device.
    ID3D11Device* device_;
    /// Immediate device context.
    ID3D11DeviceContext* deviceContext_;
    /// Swap chain.
    IDXGISwapChain* swapChain_;
    /// Default (backbuffer) rendertarget view.
    ID3D11RenderTargetView* defaultRenderTargetView_;
    /// Default depth-stencil texture.
    ID3D11Texture2D* defaultDepthTexture_;
    /// Default depth-stencil view.
    ID3D11DepthStencilView* defaultDepthStencilView_;
    /// Current color rendertarget views.
    ID3D11RenderTargetView* renderTargetViews_[MAX_RENDERTARGETS];
    /// Current depth-stencil view.
    ID3D11DepthStencilView* depthStencilView_;
    /// Created blend state objects.
    HashMap<unsigned, ID3D11BlendState*> blendStates_;
    /// Created depth state objects.
    HashMap<unsigned, ID3D11DepthStencilState*> depthStates_;
    /// Created rasterizer state objects.
    HashMap<unsigned, ID3D11RasterizerState*> rasterizerStates_;
    /// Intermediate texture for multisampled screenshots and less than whole viewport multisampled resolve, created on demand.
    ID3D11Texture2D* resolveTexture_;
    /// Bound shader resource views.
    ID3D11ShaderResourceView* shaderResourceViews_[MAX_TEXTURE_UNITS];
    /// Bound sampler state objects.
    ID3D11SamplerState* samplers_[MAX_TEXTURE_UNITS];
    /// Bound vertex buffers.
    ID3D11Buffer* vertexBuffers_[MAX_VERTEX_STREAMS];
    /// Bound constant buffers.
    ID3D11Buffer* constantBuffers_[2][MAX_SHADER_PARAMETER_GROUPS];
    /// Vertex sizes per buffer.
    unsigned vertexSizes_[MAX_VERTEX_STREAMS];
    /// Vertex stream offsets per buffer.
    unsigned vertexOffsets_[MAX_VERTEX_STREAMS];
    /// Rendertargets dirty flag.
    bool renderTargetsDirty_;
    /// Textures dirty flag.
    bool texturesDirty_;
    /// Vertex declaration dirty flag.
    bool vertexDeclarationDirty_;
    /// Blend state dirty flag.
    bool blendStateDirty_;
    /// Depth state dirty flag.
    bool depthStateDirty_;
    /// Rasterizer state dirty flag.
    bool rasterizerStateDirty_;
    /// Scissor rect dirty flag.
    bool scissorRectDirty_;
    /// Stencil ref dirty flag.
    bool stencilRefDirty_;
    /// Hash of current blend state.
    unsigned blendStateHash_;
    /// Hash of current depth state.
    unsigned depthStateHash_;
    /// Hash of current rasterizer state.
    unsigned rasterizerStateHash_;
    /// First dirtied texture unit.
    unsigned firstDirtyTexture_;
    /// Last dirtied texture unit.
    unsigned lastDirtyTexture_;
    /// First dirtied vertex buffer.
    unsigned firstDirtyVB_;
    /// Last dirtied vertex buffer.
    unsigned lastDirtyVB_;
    /// Vertex declarations.
    VertexDeclarationMap vertexDeclarations_;
    /// Constant buffer search map.
    ConstantBufferMap allConstantBuffers_;
    /// Currently dirty constant buffers.
    PODVector<ConstantBuffer*> dirtyConstantBuffers_;
    /// Shader programs.
    ShaderProgramMap shaderPrograms_;
    /// Shader program in use.
    ShaderProgram* shaderProgram_;
#endif
};

}
