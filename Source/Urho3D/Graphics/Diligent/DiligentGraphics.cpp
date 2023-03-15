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

#include <Graphics/GraphicsEngine/interface/GraphicsTypes.h>

#include "../../Core/Context.h"
#include "../../Core/ProcessUtils.h"
#include "../../Core/Profiler.h"
#include "../../Graphics/ConstantBuffer.h"
#include "../../Graphics/Geometry.h"
#include "../../Graphics/Graphics.h"
#include "../../Graphics/GraphicsEvents.h"
#include "../../Graphics/GraphicsImpl.h"
#include "../../Graphics/IndexBuffer.h"
#include "../../Graphics/Renderer.h"
#include "../../Graphics/Shader.h"
#include "../../Graphics/ShaderPrecache.h"
#include "../../Graphics/ShaderProgram.h"
#include "../../Graphics/Texture2D.h"
#include "../../Graphics/TextureCube.h"
#include "../../Graphics/VertexBuffer.h"
#include "../../IO/File.h"
#include "../../IO/Log.h"
#include "../../Resource/ResourceCache.h"

#include <SDL/SDL.h>
#include <SDL/SDL_syswm.h>

#include "../../DebugNew.h"

#ifdef _MSC_VER
#pragma warning(disable:4355)
#endif

// Prefer the high-performance GPU on switchable GPU systems
extern "C"
{
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

using namespace Diligent;

namespace Urho3D
{

// clang-format off

static const COMPARISON_FUNCTION diligentCmpFunc[] =
{
    COMPARISON_FUNC_ALWAYS,
    COMPARISON_FUNC_EQUAL,
    COMPARISON_FUNC_NOT_EQUAL,
    COMPARISON_FUNC_LESS,
    COMPARISON_FUNC_LESS_EQUAL,
    COMPARISON_FUNC_GREATER,
    COMPARISON_FUNC_GREATER_EQUAL
};

static const Bool diligentBlendEnable[] =
{
    false,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true
};

static const BLEND_FACTOR diligentSrcBlend[] =
{
    BLEND_FACTOR_ONE,
    BLEND_FACTOR_ONE,
    BLEND_FACTOR_DEST_COLOR,
    BLEND_FACTOR_SRC_ALPHA,
    BLEND_FACTOR_SRC_ALPHA,
    BLEND_FACTOR_ONE,
    BLEND_FACTOR_INV_DEST_ALPHA,
    BLEND_FACTOR_ONE,
    BLEND_FACTOR_SRC_ALPHA
};

static const BLEND_FACTOR diligentDestBlend[] =
{
    BLEND_FACTOR_ZERO,
    BLEND_FACTOR_ONE,
    BLEND_FACTOR_ZERO,
    BLEND_FACTOR_INV_SRC_ALPHA,
    BLEND_FACTOR_ONE,
    BLEND_FACTOR_INV_SRC_ALPHA,
    BLEND_FACTOR_DEST_ALPHA,
    BLEND_FACTOR_ONE, BLEND_FACTOR_ONE
};

static const BLEND_OPERATION diligentBlendOp[] =
{
    BLEND_OPERATION_ADD,
    BLEND_OPERATION_ADD,
    BLEND_OPERATION_ADD,
    BLEND_OPERATION_ADD,
    BLEND_OPERATION_ADD,
    BLEND_OPERATION_ADD,
    BLEND_OPERATION_ADD,
    BLEND_OPERATION_REV_SUBTRACT,
    BLEND_OPERATION_REV_SUBTRACT
};

static const STENCIL_OP diligentStencilOp[] =
{
    STENCIL_OP_KEEP,
    STENCIL_OP_ZERO,
    STENCIL_OP_REPLACE,
    STENCIL_OP_INCR_WRAP,
    STENCIL_OP_DECR_WRAP
};

static const CULL_MODE diligentCullMode[] =
{
    CULL_MODE_NONE,
    CULL_MODE_BACK,
    CULL_MODE_FRONT
};

static const FILL_MODE diligentFillMode[] =
{
    FILL_MODE_SOLID,
    FILL_MODE_WIREFRAME,
    FILL_MODE_WIREFRAME // Point fill mode not supported
};

static const VALUE_TYPE diligentValueType[] =
{
    VT_INT32,
    VT_FLOAT32,
    VT_FLOAT32,
    VT_FLOAT32,
    VT_FLOAT32,
    VT_UINT8,
    VT_UINT8
};

static const Uint32 diligentNumComponents[] = {
    1,
    1,
    2,
    3,
    4,
    4,
    4
};

static const bool diligentIsNormalized[] = {
    false,
    false,
    false,
    false,
    false,
    false,
    true
};

static const VALUE_TYPE diligentIndexType[] = {
    VT_UNDEFINED,
    VT_UINT8,
    VT_UINT16,
    VT_UNDEFINED,
    VT_UINT32
};

// clang-format on

static unsigned GetPrimitiveCount(unsigned elementCount, PrimitiveType type)
{
    switch (type)
    {
    case TRIANGLE_LIST:
        return elementCount / 3;

    case LINE_LIST:
        return elementCount / 2;

    case POINT_LIST:
        return elementCount;

    case TRIANGLE_STRIP:
        return elementCount - 2;

    case LINE_STRIP:
        return elementCount - 1;

    case TRIANGLE_FAN:
        // Triangle fan is not supported
        return 0;
    }

    return 0;
}

static HWND GetWindowHandle(SDL_Window* window)
{
    SDL_SysWMinfo sysInfo;

    SDL_VERSION(&sysInfo.version);
    SDL_GetWindowWMInfo(window, &sysInfo);
    return sysInfo.info.win.window;
}

const Vector2 Graphics::pixelUVOffset(0.0f, 0.0f);
bool Graphics::gl3Support = false;

Graphics::Graphics(Context* context) :
    Object(context),
    impl_(new GraphicsImpl()),
    position_(SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED),
    shaderPath_("Shaders/Diligent/"),
    shaderExtension_(".hlsl"),
    orientations_("LandscapeLeft LandscapeRight"),
    apiName_("D3D11")
{
    SetTextureUnitMappings();
    ResetCachedState();

    context_->RequireSDL(SDL_INIT_VIDEO);

    // Register Graphics library object factories
    RegisterGraphicsLibrary(context_);
}

Graphics::~Graphics()
{
#if 0
    {
        MutexLock lock(gpuObjectMutex_);

        // Release all GPU objects that still exist
        for (PODVector<GPUObject*>::Iterator i = gpuObjects_.Begin(); i != gpuObjects_.End(); ++i)
            (*i)->Release();
        gpuObjects_.Clear();
    }

    impl_->vertexDeclarations_.Clear();
    impl_->allConstantBuffers_.Clear();

    for (HashMap<unsigned, ID3D11BlendState*>::Iterator i = impl_->blendStates_.Begin(); i != impl_->blendStates_.End(); ++i)
    {
        URHO3D_SAFE_RELEASE(i->second_);
    }
    impl_->blendStates_.Clear();

    for (HashMap<unsigned, ID3D11DepthStencilState*>::Iterator i = impl_->depthStates_.Begin(); i != impl_->depthStates_.End(); ++i)
    {
        URHO3D_SAFE_RELEASE(i->second_);
    }
    impl_->depthStates_.Clear();

    for (HashMap<unsigned, ID3D11RasterizerState*>::Iterator i = impl_->rasterizerStates_.Begin();
         i != impl_->rasterizerStates_.End(); ++i)
    {
        URHO3D_SAFE_RELEASE(i->second_);
    }
    impl_->rasterizerStates_.Clear();

    URHO3D_SAFE_RELEASE(impl_->defaultRenderTargetView_);
    URHO3D_SAFE_RELEASE(impl_->defaultDepthStencilView_);
    URHO3D_SAFE_RELEASE(impl_->defaultDepthTexture_);
    URHO3D_SAFE_RELEASE(impl_->resolveTexture_);
    URHO3D_SAFE_RELEASE(impl_->swapChain_);
    URHO3D_SAFE_RELEASE(impl_->deviceContext_);
    URHO3D_SAFE_RELEASE(impl_->device_);

    if (window_)
    {
        SDL_ShowCursor(SDL_TRUE);
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    delete impl_;
    impl_ = nullptr;

    context_->ReleaseSDL();
#else
    PARTIALLY_IMPLEMENTED();

    if (window_)
    {
        SDL_ShowCursor(SDL_TRUE);
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    delete impl_;
    impl_ = nullptr;

    context_->ReleaseSDL();
#endif
}

bool Graphics::SetScreenMode(int width, int height, const ScreenModeParams& params, bool maximize)
{
    URHO3D_PROFILE(SetScreenMode);

    // Ensure that parameters are properly filled
    ScreenModeParams newParams = params;
    AdjustScreenMode(width, height, newParams, maximize);

    // Find out the full screen mode display format (match desktop color depth)
    SDL_DisplayMode mode;
    SDL_GetDesktopDisplayMode(newParams.monitor_, &mode);
    const DXGI_FORMAT fullscreenFormat =
        SDL_BITSPERPIXEL(mode.format) == 16 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    // If nothing changes, do not reset the device
    if (width == width_ && height == height_ && newParams == screenParams_)
        return true;

    SDL_SetHint(SDL_HINT_ORIENTATIONS, orientations_.CString());

    if (!window_)
    {
        if (!OpenWindow(width, height, newParams.resizable_, newParams.borderless_))
            return false;
    }

    AdjustWindow(width, height, newParams.fullscreen_, newParams.borderless_, newParams.monitor_);

    if (maximize)
    {
        Maximize();
        SDL_GetWindowSize(window_, &width, &height);
    }

    const int oldMultiSample = screenParams_.multiSample_;
    screenParams_ = newParams;

    if (!impl_->device_ || screenParams_.multiSample_ != oldMultiSample)
        CreateDevice(width, height);
    UpdateSwapChain(width, height);

    OnScreenModeChanged();

    return true;
}

void Graphics::SetSRGB(bool enable)
{
#if 0
    bool newEnable = enable && sRGBWriteSupport_;
    if (newEnable != sRGB_)
    {
        sRGB_ = newEnable;
        if (impl_->swapChain_)
        {
            // Recreate swap chain for the new backbuffer format
            CreateDevice(width_, height_);
            UpdateSwapChain(width_, height_);
        }
    }
#else
    NOT_IMPLEMENTED();
#endif
}

void Graphics::SetDither(bool enable)
{
    // No effect on Diligent
}

void Graphics::SetFlushGPU(bool enable)
{
    flushGPU_ = enable;

    if (impl_->swapChain_)
    {
        impl_->swapChain_->SetMaximumFrameLatency(enable ? 1 : 3);
    }
}

void Graphics::SetForceGL2(bool enable)
{
    // No effect on Diligent
}

void Graphics::Close()
{
    if (window_)
    {
        SDL_ShowCursor(SDL_TRUE);
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool Graphics::TakeScreenShot(Image& destImage)
{
#if 0
    URHO3D_PROFILE(TakeScreenShot);

    if (!impl_->device_)
        return false;

    D3D11_TEXTURE2D_DESC textureDesc;
    memset(&textureDesc, 0, sizeof textureDesc);
    textureDesc.Width = (UINT)width_;
    textureDesc.Height = (UINT)height_;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = D3D11_USAGE_STAGING;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = impl_->device_->CreateTexture2D(&textureDesc, nullptr, &stagingTexture);
    if (FAILED(hr))
    {
        URHO3D_SAFE_RELEASE(stagingTexture);
        URHO3D_LOGD3DERROR("Could not create staging texture for screenshot", hr);
        return false;
    }

    ID3D11Resource* source = nullptr;
    impl_->defaultRenderTargetView_->GetResource(&source);

    if (screenParams_.multiSample_ > 1)
    {
        // If backbuffer is multisampled, need another DEFAULT usage texture to resolve the data to first
        CreateResolveTexture();

        if (!impl_->resolveTexture_)
        {
            stagingTexture->Release();
            source->Release();
            return false;
        }

        impl_->deviceContext_->ResolveSubresource(impl_->resolveTexture_, 0, source, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        impl_->deviceContext_->CopyResource(stagingTexture, impl_->resolveTexture_);
    }
    else
        impl_->deviceContext_->CopyResource(stagingTexture, source);

    source->Release();

    D3D11_MAPPED_SUBRESOURCE mappedData;
    mappedData.pData = nullptr;
    hr = impl_->deviceContext_->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedData);
    if (FAILED(hr) || !mappedData.pData)
    {
        URHO3D_LOGD3DERROR("Could not map staging texture for screenshot", hr);
        stagingTexture->Release();
        return false;
    }

    destImage.SetSize(width_, height_, 3);
    unsigned char* destData = destImage.GetData();
    for (int y = 0; y < height_; ++y)
    {
        unsigned char* src = (unsigned char*)mappedData.pData + y * mappedData.RowPitch;
        for (int x = 0; x < width_; ++x)
        {
            *destData++ = *src++;
            *destData++ = *src++;
            *destData++ = *src++;
            ++src;
        }
    }

    impl_->deviceContext_->Unmap(stagingTexture, 0);
    stagingTexture->Release();

    return true;
#else
    NOT_IMPLEMENTED();

    return true;
#endif
}

bool Graphics::BeginFrame()
{
    if (!IsInitialized())
        return false;

    // If using an external window, check it for size changes, and reset screen mode if necessary
    if (externalWindow_)
    {
        int width, height;

        SDL_GetWindowSize(window_, &width, &height);
        if (width != width_ || height != height_)
            SetMode(width, height);
    }
    else
    {
        // To prevent a loop of endless device loss and flicker, do not attempt to render when in fullscreen
        // and the window is minimized
        if (screenParams_.fullscreen_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED))
            return false;
    }

    // Set default rendertarget and depth buffer
    ResetRenderTargets();

    // Cleanup textures from previous frame
    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
        SetTexture(i, nullptr);

    numPrimitives_ = 0;
    numBatches_ = 0;

    SendEvent(E_BEGINRENDERING);

    return true;
}

void Graphics::EndFrame()
{
    if (!IsInitialized())
        return;

    {
        URHO3D_PROFILE(Present);

        SendEvent(E_ENDRENDERING);
        impl_->swapChain_->Present(screenParams_.vsync_ ? 1 : 0);
    }

    // Clean up too large scratch buffers
    CleanupScratchBuffers();
}

void Graphics::Clear(ClearTargetFlags flags, const Color& color, float depth, unsigned stencil)
{
    IntVector2 rtSize = GetRenderTargetDimensions();

    bool oldColorWrite = colorWrite_;
    bool oldDepthWrite = depthWrite_;

    // D3D11 clear always clears the whole target regardless of viewport or scissor test settings
    // Emulate partial clear by rendering a quad
    if (!viewport_.left_ && !viewport_.top_ && viewport_.right_ == rtSize.x_ && viewport_.bottom_ == rtSize.y_)
    {
        // Make sure we use the read-write version of the depth stencil
        SetDepthWrite(true);
        PrepareDraw();

        if ((flags & CLEAR_COLOR) && impl_->renderTargetViews_[0])
            impl_->deviceContext_->ClearRenderTarget(impl_->renderTargetViews_[0], color.Data(), 
                RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        if ((flags & (CLEAR_DEPTH | CLEAR_STENCIL)) && impl_->depthStencilView_)
        {
            CLEAR_DEPTH_STENCIL_FLAGS depthClearFlags = CLEAR_DEPTH_FLAG_NONE;
            if (flags & CLEAR_DEPTH)
                depthClearFlags |= CLEAR_DEPTH_FLAG;
            if (flags & CLEAR_STENCIL)
                depthClearFlags |= CLEAR_STENCIL_FLAG;
            impl_->deviceContext_->ClearDepthStencil(impl_->depthStencilView_, depthClearFlags, depth, (UINT8)stencil,
                RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }
    else
    {
        Renderer* renderer = GetSubsystem<Renderer>();
        if (!renderer)
            return;

        Geometry* geometry = renderer->GetQuadGeometry();

        Matrix3x4 model = Matrix3x4::IDENTITY;
        Matrix4 projection = Matrix4::IDENTITY;
        model.m23_ = Clamp(depth, 0.0f, 1.0f);

        SetBlendMode(BLEND_REPLACE);
        SetColorWrite(flags & CLEAR_COLOR);
        SetCullMode(CULL_NONE);
        SetDepthTest(CMP_ALWAYS);
        SetDepthWrite(flags & CLEAR_DEPTH);
        SetFillMode(FILL_SOLID);
        SetScissorTest(false);
        SetStencilTest(flags & CLEAR_STENCIL, CMP_ALWAYS, OP_REF, OP_KEEP, OP_KEEP, stencil);
        SetShaders(GetShader(VS, "ClearFramebuffer"), GetShader(PS, "ClearFramebuffer"));
        SetShaderParameter(VSP_MODEL, model);
        SetShaderParameter(VSP_VIEWPROJ, projection);
        SetShaderParameter(PSP_MATDIFFCOLOR, color);

        geometry->Draw(this);

        SetStencilTest(false);
        ClearParameterSources();
    }

    // Restore color & depth write state now
    SetColorWrite(oldColorWrite);
    SetDepthWrite(oldDepthWrite);
}

bool Graphics::ResolveToTexture(Texture2D* destination, const IntRect& viewport)
{
    if (!destination || !destination->GetRenderSurface())
        return false;

    URHO3D_PROFILE(ResolveToTexture);

    IntRect vpCopy = viewport;
    if (vpCopy.right_ <= vpCopy.left_)
        vpCopy.right_ = vpCopy.left_ + 1;
    if (vpCopy.bottom_ <= vpCopy.top_)
        vpCopy.bottom_ = vpCopy.top_ + 1;

    Box srcBox;
    srcBox.MinX = Clamp(vpCopy.left_, 0, width_);
    srcBox.MinY = Clamp(vpCopy.top_, 0, height_);
    srcBox.MaxX = Clamp(vpCopy.right_, 0, width_);
    srcBox.MaxY = Clamp(vpCopy.bottom_, 0, height_);
    srcBox.MinZ = 0;
    srcBox.MaxZ = 1;

    const bool resolve = screenParams_.multiSample_ > 1;
    ITexture* source = impl_->defaultRenderTargetView_->GetTexture();

    if (!resolve)
    {
        CopyTextureAttribs copyTextureAttribs(source, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                              (ITexture*)destination->GetGPUObject(),
                                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        if (!srcBox.MinX && !srcBox.MinY && srcBox.MaxX == width_ && srcBox.MaxY == height_)
            copyTextureAttribs.pSrcBox = nullptr;
        else
            copyTextureAttribs.pSrcBox = &srcBox;

        impl_->deviceContext_->CopyTexture(copyTextureAttribs);
    }
    else
    {
        if (!srcBox.MinX && !srcBox.MinY && srcBox.MaxX == width_ && srcBox.MaxY == height_)
        {
            ResolveTextureSubresourceAttribs resolveTextureSubresourceAttribs;
            resolveTextureSubresourceAttribs.Format = (TEXTURE_FORMAT)destination->GetFormat();
            resolveTextureSubresourceAttribs.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            resolveTextureSubresourceAttribs.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

            impl_->deviceContext_->ResolveTextureSubresource(source, (ITexture*)destination->GetGPUObject(),
                                                             resolveTextureSubresourceAttribs);
        }
        else
        {
            CreateResolveTexture();

            if (impl_->resolveTexture_)
            {
                ResolveTextureSubresourceAttribs resolveTextureSubresourceAttribs;
                resolveTextureSubresourceAttribs.Format = TEX_FORMAT_RGBA8_UNORM;
                resolveTextureSubresourceAttribs.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
                resolveTextureSubresourceAttribs.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

                impl_->deviceContext_->ResolveTextureSubresource(source, impl_->resolveTexture_,
                                                                 resolveTextureSubresourceAttribs);

                CopyTextureAttribs copyTextureAttribs(source, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                                      (ITexture*)destination->GetGPUObject(),
                                                      RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                copyTextureAttribs.pSrcBox = &srcBox;

                impl_->deviceContext_->CopyTexture(copyTextureAttribs);
            }
        }
    }

    return true;
}

bool Graphics::ResolveToTexture(Texture2D* texture)
{
    if (!texture)
        return false;
    RenderSurface* surface = texture->GetRenderSurface();
    if (!surface)
        return false;

    texture->SetResolveDirty(false);
    surface->SetResolveDirty(false);
    ITexture* source = (ITexture*)texture->GetGPUObject();
    ITexture* dest = (ITexture*)texture->GetResolveTexture();
    if (!source || !dest)
        return false;

    ResolveTextureSubresourceAttribs resolveTextureSubresourceAttribs;
    resolveTextureSubresourceAttribs.Format = (TEXTURE_FORMAT)texture->GetFormat();
    resolveTextureSubresourceAttribs.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    resolveTextureSubresourceAttribs.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

    impl_->deviceContext_->ResolveTextureSubresource(source, dest, resolveTextureSubresourceAttribs);

    return true;
}

bool Graphics::ResolveToTexture(TextureCube* texture)
{
    if (!texture)
        return false;

    texture->SetResolveDirty(false);
    ITexture* source = (ITexture*)texture->GetGPUObject();
    ITexture* dest = (ITexture*)texture->GetResolveTexture();
    if (!source || !dest)
        return false;

    for (unsigned i = 0; i < MAX_CUBEMAP_FACES; ++i)
    {
        // Resolve only the surface(s) that were actually rendered to
        RenderSurface* surface = texture->GetRenderSurface((CubeMapFace)i);
        if (!surface->IsResolveDirty())
            continue;

        surface->SetResolveDirty(false);

        ResolveTextureSubresourceAttribs resolveTextureSubresourceAttribs;
        resolveTextureSubresourceAttribs.Format = (TEXTURE_FORMAT)texture->GetFormat();
        resolveTextureSubresourceAttribs.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        resolveTextureSubresourceAttribs.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        resolveTextureSubresourceAttribs.DstMipLevel = 0;
        resolveTextureSubresourceAttribs.DstSlice = i;

        impl_->deviceContext_->ResolveTextureSubresource(source, dest, resolveTextureSubresourceAttribs);
    }

    return true;
}


void Graphics::Draw(PrimitiveType type, unsigned vertexStart, unsigned vertexCount)
{
    if (!vertexCount || !impl_->shaderProgram_)
        return;

    if (fillMode_ == FILL_POINT)
        type = POINT_LIST;

    impl_->SetPrimitiveType(type);
    PrepareDraw();

    DrawAttribs drawAttribs;
    drawAttribs.NumVertices = vertexCount;
    drawAttribs.StartVertexLocation = vertexStart;
    impl_->deviceContext_->Draw(drawAttribs);

    numPrimitives_ += GetPrimitiveCount(vertexCount, type);
    ++numBatches_;
}

void Graphics::Draw(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount)
{
    if (!vertexCount || !impl_->shaderProgram_)
        return;

    if (fillMode_ == FILL_POINT)
        type = POINT_LIST;

    impl_->SetPrimitiveType(type);
    PrepareDraw();

    DrawIndexedAttribs drawAttribs;
    drawAttribs.IndexType = diligentIndexType[indexBuffer_->GetIndexSize()];
    drawAttribs.FirstIndexLocation = indexStart;
    drawAttribs.NumIndices = indexCount;
    impl_->deviceContext_->DrawIndexed(drawAttribs);

    numPrimitives_ += GetPrimitiveCount(indexCount, type);
    ++numBatches_;
}

void Graphics::Draw(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned baseVertexIndex, unsigned minVertex, unsigned vertexCount)
{
    if (!vertexCount || !impl_->shaderProgram_)
        return;

    if (fillMode_ == FILL_POINT)
        type = POINT_LIST;

    impl_->SetPrimitiveType(type);
    PrepareDraw();

    DrawIndexedAttribs drawAttribs;
    drawAttribs.IndexType = diligentIndexType[indexBuffer_->GetIndexSize()];
    drawAttribs.FirstIndexLocation = indexStart;
    drawAttribs.NumIndices = indexCount;
    drawAttribs.BaseVertex = baseVertexIndex;
    impl_->deviceContext_->DrawIndexed(drawAttribs);

    numPrimitives_ += GetPrimitiveCount(indexCount, type);
    ++numBatches_;
}

void Graphics::DrawInstanced(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount,
    unsigned instanceCount)
{
    if (!indexCount || !instanceCount || !impl_->shaderProgram_)
        return;

    if (fillMode_ == FILL_POINT)
        type = POINT_LIST;

    impl_->SetPrimitiveType(type);
    PrepareDraw();

    DrawIndexedAttribs drawAttribs;
    drawAttribs.IndexType = diligentIndexType[indexBuffer_->GetIndexSize()];
    drawAttribs.FirstIndexLocation = indexStart;
    drawAttribs.NumIndices = indexCount;
    drawAttribs.NumInstances = instanceCount;
    impl_->deviceContext_->DrawIndexed(drawAttribs);

    numPrimitives_ += instanceCount * GetPrimitiveCount(indexCount, type);
    ++numBatches_;
}

void Graphics::DrawInstanced(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned baseVertexIndex, unsigned minVertex, unsigned vertexCount,
    unsigned instanceCount)
{
    if (!indexCount || !instanceCount || !impl_->shaderProgram_)
        return;

    if (fillMode_ == FILL_POINT)
        type = POINT_LIST;

    impl_->SetPrimitiveType(type);
    PrepareDraw();

    DrawIndexedAttribs drawAttribs;
    drawAttribs.IndexType = diligentIndexType[indexBuffer_->GetIndexSize()];
    drawAttribs.FirstIndexLocation = indexStart;
    drawAttribs.NumIndices = indexCount;
    drawAttribs.BaseVertex = baseVertexIndex;
    drawAttribs.NumInstances = instanceCount;
    impl_->deviceContext_->DrawIndexed(drawAttribs);

    numPrimitives_ += instanceCount * GetPrimitiveCount(indexCount, type);
    ++numBatches_;
}

void Graphics::SetVertexBuffer(VertexBuffer* buffer)
{
    // Note: this is not multi-instance safe
    static PODVector<VertexBuffer*> vertexBuffers(1);
    vertexBuffers[0] = buffer;
    SetVertexBuffers(vertexBuffers);
}

bool Graphics::SetVertexBuffers(const PODVector<VertexBuffer*>& buffers, unsigned instanceOffset)
{
    if (buffers.Size() > MAX_VERTEX_STREAMS)
    {
        URHO3D_LOGERROR("Too many vertex buffers");
        return false;
    }

    for (unsigned i = 0; i < MAX_VERTEX_STREAMS; ++i)
    {
        VertexBuffer* buffer = nullptr;
        bool changed = false;

        buffer = i < buffers.Size() ? buffers[i] : nullptr;
        if (buffer)
        {
            const PODVector<VertexElement>& elements = buffer->GetElements();
            // Check if buffer has per-instance data
            bool hasInstanceData = elements.Size() && elements[0].perInstance_;
            unsigned offset = hasInstanceData ? instanceOffset * buffer->GetVertexSize() : 0;

            if (buffer != vertexBuffers_[i] || offset != impl_->vertexOffsets_[i])
            {
                vertexBuffers_[i] = buffer;
                impl_->vertexBuffers_[i] = (IBuffer*)buffer->GetGPUObject();
                impl_->vertexSizes_[i] = buffer->GetVertexSize();
                impl_->vertexOffsets_[i] = offset;
                changed = true;
            }
        }
        else if (vertexBuffers_[i])
        {
            vertexBuffers_[i] = nullptr;
            impl_->vertexBuffers_[i] = nullptr;
            impl_->vertexSizes_[i] = 0;
            impl_->vertexOffsets_[i] = 0;
            changed = true;
        }

        if (changed)
        {
            impl_->vertexDeclarationDirty_ = true;

            if (impl_->firstDirtyVB_ == M_MAX_UNSIGNED)
                impl_->firstDirtyVB_ = impl_->lastDirtyVB_ = i;
            else
            {
                if (i < impl_->firstDirtyVB_)
                    impl_->firstDirtyVB_ = i;
                if (i > impl_->lastDirtyVB_)
                    impl_->lastDirtyVB_ = i;
            }
        }
    }

    return true;
}

bool Graphics::SetVertexBuffers(const Vector<SharedPtr<VertexBuffer> >& buffers, unsigned instanceOffset)
{
    return SetVertexBuffers(reinterpret_cast<const PODVector<VertexBuffer*>&>(buffers), instanceOffset);
}

void Graphics::SetIndexBuffer(IndexBuffer* buffer)
{
    if (buffer != indexBuffer_)
    {
        if (buffer)
            impl_->deviceContext_->SetIndexBuffer((IBuffer*)buffer->GetGPUObject(), 0,
                                                  RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        else
            impl_->deviceContext_->SetIndexBuffer(nullptr, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        indexBuffer_ = buffer;
    }
}

void Graphics::SetShaders(ShaderVariation* vs, ShaderVariation* ps)
{
    // Switch to the clip plane variations if necessary
    if (useClipPlane_)
    {
        if (vs)
            vs = vs->GetOwner()->GetVariation(VS, vs->GetDefinesClipPlane());
        if (ps)
            ps = ps->GetOwner()->GetVariation(PS, ps->GetDefinesClipPlane());
    }

    if (vs != vertexShader_)
    {
        vertexShader_ = vs;
        impl_->vertexShaderDirty_ = true;
        impl_->vertexDeclarationDirty_ = true;

        // Create the shader now if not yet created. If already attempted, do not retry
        if (vertexShader_ && !vertexShader_->GetGPUObject())
        {
            if (vertexShader_->GetCompilerOutput().Empty())
            {
                URHO3D_PROFILE(CompileVertexShader);

                bool success = vertexShader_->Create();
                if (!success)
                {
                    URHO3D_LOGERROR("Failed to compile vertex shader " + vertexShader_->GetFullName() + ":\n" +
                                    vertexShader_->GetCompilerOutput());
                    vertexShader_ = nullptr;
                }
            }
            else
                vertexShader_ = nullptr;
        }
    }

    if (ps != pixelShader_)
    {
        pixelShader_ = ps;
        impl_->pixelShaderDirty_ = true;

        if (pixelShader_ && !pixelShader_->GetGPUObject())
        {
            if (pixelShader_->GetCompilerOutput().Empty())
            {
                URHO3D_PROFILE(CompilePixelShader);

                bool success = pixelShader_->Create();
                if (!success)
                {
                    URHO3D_LOGERROR("Failed to compile pixel shader " + pixelShader_->GetFullName() + ":\n" +
                                    pixelShader_->GetCompilerOutput());
                    pixelShader_ = nullptr;
                }
            }
            else
                pixelShader_ = nullptr;
        }
    }

    if (vertexShader_ && pixelShader_)
    {
        Pair<ShaderVariation*, ShaderVariation*> key = MakePair(vertexShader_, pixelShader_);
        ShaderProgramMap::Iterator i = impl_->shaderPrograms_.Find(key);
        if (i != impl_->shaderPrograms_.End())
            impl_->shaderProgram_ = i->second_.Get();
        else
        {
            ShaderProgram* newProgram = impl_->shaderPrograms_[key] =
                new ShaderProgram(this, vertexShader_, pixelShader_);
            impl_->shaderProgram_ = newProgram;
        }

        for (unsigned i = 0; i < MAX_SHADER_PARAMETER_GROUPS; ++i)
        {
            void* vsBuffer = impl_->shaderProgram_->vsConstantBuffers_[i]
                                 ? impl_->shaderProgram_->vsConstantBuffers_[i]->GetGPUObject()
                                 : nullptr;
            if (vsBuffer != (void*)impl_->constantBuffers_[VS][i])
            {
                shaderParameterSources_[i] = (const void*)M_MAX_UNSIGNED;
            }

            void* psBuffer = impl_->shaderProgram_->psConstantBuffers_[i]
                                 ? impl_->shaderProgram_->psConstantBuffers_[i]->GetGPUObject()
                                 : nullptr;
            if (psBuffer != (void*)impl_->constantBuffers_[PS][i])
            {
                shaderParameterSources_[i] = (const void*)M_MAX_UNSIGNED;
            }
        }
    }
    else
        impl_->shaderProgram_ = nullptr;

    // Store shader combination if shader dumping in progress
    if (shaderPrecache_)
        shaderPrecache_->StoreShaders(vertexShader_, pixelShader_);

    // Update clip plane parameter if necessary
    if (useClipPlane_)
        SetShaderParameter(VSP_CLIPPLANE, clipPlane_);
}

void Graphics::SetShaderParameter(StringHash param, const float* data, unsigned count)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, (unsigned)(count * sizeof(float)), data);
}

void Graphics::SetShaderParameter(StringHash param, float value)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(float), &value);
}

void Graphics::SetShaderParameter(StringHash param, int value)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(int), &value);
}

void Graphics::SetShaderParameter(StringHash param, bool value)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(bool), &value);
}

void Graphics::SetShaderParameter(StringHash param, const Color& color)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(Color), &color);
}

void Graphics::SetShaderParameter(StringHash param, const Vector2& vector)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(Vector2), &vector);
}

void Graphics::SetShaderParameter(StringHash param, const Matrix3& matrix)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetVector3ArrayParameter(i->second_.offset_, 3, &matrix);
}

void Graphics::SetShaderParameter(StringHash param, const Vector3& vector)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(Vector3), &vector);
}

void Graphics::SetShaderParameter(StringHash param, const Matrix4& matrix)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(Matrix4), &matrix);
}

void Graphics::SetShaderParameter(StringHash param, const Vector4& vector)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(Vector4), &vector);
}

void Graphics::SetShaderParameter(StringHash param, const Matrix3x4& matrix)
{
    HashMap<StringHash, ShaderParameter>::Iterator i;
    if (!impl_->shaderProgram_ ||
        (i = impl_->shaderProgram_->parameters_.Find(param)) == impl_->shaderProgram_->parameters_.End())
        return;

    ConstantBuffer* buffer = i->second_.bufferPtr_;
    if (!buffer->IsDirty())
        impl_->dirtyConstantBuffers_.Push(buffer);
    buffer->SetParameter(i->second_.offset_, sizeof(Matrix3x4), &matrix);
}

bool Graphics::NeedParameterUpdate(ShaderParameterGroup group, const void* source)
{
    if ((unsigned)(size_t)shaderParameterSources_[group] == M_MAX_UNSIGNED || shaderParameterSources_[group] != source)
    {
        shaderParameterSources_[group] = source;
        return true;
    }
    else
        return false;
}

bool Graphics::HasShaderParameter(StringHash param)
{
    return impl_->shaderProgram_ && impl_->shaderProgram_->parameters_.Find(param) != impl_->shaderProgram_->parameters_.End();
}

bool Graphics::HasTextureUnit(TextureUnit unit)
{
#if 0
    return (vertexShader_ && vertexShader_->HasTextureUnit(unit)) || (pixelShader_ && pixelShader_->HasTextureUnit(unit));
#else
    PARTIALLY_IMPLEMENTED();
//    return (vertexShader_ && vertexShader_->HasTextureUnit(unit)) || (pixelShader_ && pixelShader_->HasTextureUnit(unit));
    return true;
#endif
}

void Graphics::ClearParameterSource(ShaderParameterGroup group)
{
    shaderParameterSources_[group] = (const void*)M_MAX_UNSIGNED;
}

void Graphics::ClearParameterSources()
{
    for (unsigned i = 0; i < MAX_SHADER_PARAMETER_GROUPS; ++i)
        shaderParameterSources_[i] = (const void*)M_MAX_UNSIGNED;
}

void Graphics::ClearTransformSources()
{
    shaderParameterSources_[SP_CAMERA] = (const void*)M_MAX_UNSIGNED;
    shaderParameterSources_[SP_OBJECT] = (const void*)M_MAX_UNSIGNED;
}

void Graphics::SetTexture(unsigned index, Texture* texture)
{
    if (index >= MAX_TEXTURE_UNITS)
        return;

    // Check if texture is currently bound as a rendertarget. In that case, use its backup texture, or blank if not
    // defined
    if (texture)
    {
        if (renderTargets_[0] && renderTargets_[0]->GetParentTexture() == texture)
            texture = texture->GetBackupTexture();
        else
        {
            // Resolve multisampled texture now as necessary
            if (texture->GetMultiSample() > 1 && texture->GetAutoResolve() && texture->IsResolveDirty())
            {
                if (texture->GetType() == Texture2D::GetTypeStatic())
                    ResolveToTexture(static_cast<Texture2D*>(texture));
                if (texture->GetType() == TextureCube::GetTypeStatic())
                    ResolveToTexture(static_cast<TextureCube*>(texture));
            }
        }

        if (texture && texture->GetLevelsDirty())
            texture->RegenerateLevels();
    }

    if (texture && texture->GetParametersDirty())
    {
        texture->UpdateParameters();
        textures_[index] = nullptr; // Force reassign
    }

    if (texture != textures_[index])
    {
        if (impl_->firstDirtyTexture_ == M_MAX_UNSIGNED)
            impl_->firstDirtyTexture_ = impl_->lastDirtyTexture_ = index;
        else
        {
            if (index < impl_->firstDirtyTexture_)
                impl_->firstDirtyTexture_ = index;
            if (index > impl_->lastDirtyTexture_)
                impl_->lastDirtyTexture_ = index;
        }

        textures_[index] = texture;
        impl_->shaderResourceViews_[index] = texture ? (ITextureView*)texture->GetShaderResourceView() : nullptr;
        impl_->samplers_[index] = texture ? (ISampler*)texture->GetSampler() : nullptr;
        impl_->texturesDirty_ = true;
    }
}

void SetTextureForUpdate(Texture* texture)
{
    // No-op on Diligent
}

void Graphics::SetDefaultTextureFilterMode(TextureFilterMode mode)
{
    if (mode != defaultTextureFilterMode_)
    {
        defaultTextureFilterMode_ = mode;
        SetTextureParametersDirty();
    }
}

void Graphics::SetDefaultTextureAnisotropy(unsigned level)
{
    level = Max(level, 1U);

    if (level != defaultTextureAnisotropy_)
    {
        defaultTextureAnisotropy_ = level;
        SetTextureParametersDirty();
    }
}

void Graphics::Restore()
{
    // No-op on Diligent
}

void Graphics::SetTextureParametersDirty()
{
    MutexLock lock(gpuObjectMutex_);

    for (PODVector<GPUObject*>::Iterator i = gpuObjects_.Begin(); i != gpuObjects_.End(); ++i)
    {
        Texture* texture = dynamic_cast<Texture*>(*i);
        if (texture)
            texture->SetParametersDirty();
    }
}

void Graphics::ResetRenderTargets()
{
    for (unsigned i = 0; i < MAX_RENDERTARGETS; ++i)
        SetRenderTarget(i, (RenderSurface*)nullptr);
    SetDepthStencil((RenderSurface*)nullptr);
    SetViewport(IntRect(0, 0, width_, height_));
}

void Graphics::ResetRenderTarget(unsigned index)
{
    SetRenderTarget(index, (RenderSurface*)nullptr);
}

void Graphics::ResetDepthStencil()
{
    SetDepthStencil((RenderSurface*)nullptr);
}

void Graphics::SetRenderTarget(unsigned index, RenderSurface* renderTarget)
{
    if (index >= MAX_RENDERTARGETS)
        return;

    if (renderTarget != renderTargets_[index])
    {
        renderTargets_[index] = renderTarget;
        impl_->renderTargetsDirty_ = true;

        // If the rendertarget is also bound as a texture, replace with backup texture or null
        if (renderTarget)
        {
            Texture* parentTexture = renderTarget->GetParentTexture();

            for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
            {
                if (textures_[i] == parentTexture)
                    SetTexture(i, textures_[i]->GetBackupTexture());
            }

            // If multisampled, mark the texture & surface needing resolve
            if (parentTexture->GetMultiSample() > 1 && parentTexture->GetAutoResolve())
            {
                parentTexture->SetResolveDirty(true);
                renderTarget->SetResolveDirty(true);
            }

            // If mipmapped, mark the levels needing regeneration
            if (parentTexture->GetLevels() > 1)
                parentTexture->SetLevelsDirty();
        }
    }
}

void Graphics::SetRenderTarget(unsigned index, Texture2D* texture)
{
    RenderSurface* renderTarget = nullptr;
    if (texture)
        renderTarget = texture->GetRenderSurface();

    SetRenderTarget(index, renderTarget);
}

void Graphics::SetDepthStencil(RenderSurface* depthStencil)
{
    if (depthStencil != depthStencil_)
    {
        depthStencil_ = depthStencil;
        impl_->renderTargetsDirty_ = true;
    }
}

void Graphics::SetDepthStencil(Texture2D* texture)
{
    RenderSurface* depthStencil = nullptr;
    if (texture)
        depthStencil = texture->GetRenderSurface();

    SetDepthStencil(depthStencil);
    // Constant depth bias depends on the bitdepth
    impl_->rasterizerStateDirty_ = true;
}

void Graphics::SetViewport(const IntRect& rect)
{
    IntVector2 size = GetRenderTargetDimensions();

    IntRect rectCopy = rect;

    if (rectCopy.right_ <= rectCopy.left_)
        rectCopy.right_ = rectCopy.left_ + 1;
    if (rectCopy.bottom_ <= rectCopy.top_)
        rectCopy.bottom_ = rectCopy.top_ + 1;
    rectCopy.left_ = Clamp(rectCopy.left_, 0, size.x_);
    rectCopy.top_ = Clamp(rectCopy.top_, 0, size.y_);
    rectCopy.right_ = Clamp(rectCopy.right_, 0, size.x_);
    rectCopy.bottom_ = Clamp(rectCopy.bottom_, 0, size.y_);

    static Diligent::Viewport viewport;
    viewport.TopLeftX = (float)rectCopy.left_;
    viewport.TopLeftY = (float)rectCopy.top_;
    viewport.Width = (float)(rectCopy.right_ - rectCopy.left_);
    viewport.Height = (float)(rectCopy.bottom_ - rectCopy.top_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    impl_->deviceContext_->SetViewports(1, &viewport, 0, 0);

    viewport_ = rectCopy;

    // Disable scissor test, needs to be re-enabled by the user
    SetScissorTest(false);
}

void Graphics::SetBlendMode(BlendMode mode, bool alphaToCoverage)
{
    if (mode != blendMode_ || alphaToCoverage != alphaToCoverage_)
    {
        blendMode_ = mode;
        alphaToCoverage_ = alphaToCoverage;
        impl_->blendStateDirty_ = true;
    }
}

void Graphics::SetColorWrite(bool enable)
{
    if (enable != colorWrite_)
    {
        colorWrite_ = enable;
        impl_->blendStateDirty_ = true;
    }
}

void Graphics::SetCullMode(CullMode mode)
{
    if (mode != cullMode_)
    {
        cullMode_ = mode;
        impl_->rasterizerStateDirty_ = true;
    }
}

void Graphics::SetDepthBias(float constantBias, float slopeScaledBias)
{
    if (constantBias != constantDepthBias_ || slopeScaledBias != slopeScaledDepthBias_)
    {
        constantDepthBias_ = constantBias;
        slopeScaledDepthBias_ = slopeScaledBias;
        impl_->rasterizerStateDirty_ = true;
    }
}

void Graphics::SetDepthTest(CompareMode mode)
{
    if (mode != depthTestMode_)
    {
        depthTestMode_ = mode;
        impl_->depthStateDirty_ = true;
    }
}

void Graphics::SetDepthWrite(bool enable)
{
    if (enable != depthWrite_)
    {
        depthWrite_ = enable;
        impl_->depthStateDirty_ = true;
        // Also affects whether a read-only version of depth-stencil should be bound, to allow sampling
        impl_->renderTargetsDirty_ = true;
    }
}

void Graphics::SetFillMode(FillMode mode)
{
    if (mode != fillMode_)
    {
        fillMode_ = mode;
        impl_->rasterizerStateDirty_ = true;
    }
}

void Graphics::SetLineAntiAlias(bool enable)
{
    if (enable != lineAntiAlias_)
    {
        lineAntiAlias_ = enable;
        impl_->rasterizerStateDirty_ = true;
    }
}

void Graphics::SetScissorTest(bool enable, const Rect& rect, bool borderInclusive)
{
    // During some light rendering loops, a full rect is toggled on/off repeatedly.
    // Disable scissor in that case to reduce state changes
    if (rect.min_.x_ <= 0.0f && rect.min_.y_ <= 0.0f && rect.max_.x_ >= 1.0f && rect.max_.y_ >= 1.0f)
        enable = false;

    if (enable)
    {
        IntVector2 rtSize(GetRenderTargetDimensions());
        IntVector2 viewSize(viewport_.Size());
        IntVector2 viewPos(viewport_.left_, viewport_.top_);
        IntRect intRect;
        int expand = borderInclusive ? 1 : 0;

        intRect.left_ = Clamp((int)((rect.min_.x_ + 1.0f) * 0.5f * viewSize.x_) + viewPos.x_, 0, rtSize.x_ - 1);
        intRect.top_ = Clamp((int)((-rect.max_.y_ + 1.0f) * 0.5f * viewSize.y_) + viewPos.y_, 0, rtSize.y_ - 1);
        intRect.right_ = Clamp((int)((rect.max_.x_ + 1.0f) * 0.5f * viewSize.x_) + viewPos.x_ + expand, 0, rtSize.x_);
        intRect.bottom_ = Clamp((int)((-rect.min_.y_ + 1.0f) * 0.5f * viewSize.y_) + viewPos.y_ + expand, 0, rtSize.y_);

        if (intRect.right_ == intRect.left_)
            intRect.right_++;
        if (intRect.bottom_ == intRect.top_)
            intRect.bottom_++;

        if (intRect.right_ < intRect.left_ || intRect.bottom_ < intRect.top_)
            enable = false;

        if (enable && intRect != scissorRect_)
        {
            scissorRect_ = intRect;
            impl_->scissorRectDirty_ = true;
        }
    }

    if (enable != scissorTest_)
    {
        scissorTest_ = enable;
        impl_->rasterizerStateDirty_ = true;
    }
}

void Graphics::SetScissorTest(bool enable, const IntRect& rect)
{
    IntVector2 rtSize(GetRenderTargetDimensions());
    IntVector2 viewPos(viewport_.left_, viewport_.top_);

    if (enable)
    {
        IntRect intRect;
        intRect.left_ = Clamp(rect.left_ + viewPos.x_, 0, rtSize.x_ - 1);
        intRect.top_ = Clamp(rect.top_ + viewPos.y_, 0, rtSize.y_ - 1);
        intRect.right_ = Clamp(rect.right_ + viewPos.x_, 0, rtSize.x_);
        intRect.bottom_ = Clamp(rect.bottom_ + viewPos.y_, 0, rtSize.y_);

        if (intRect.right_ == intRect.left_)
            intRect.right_++;
        if (intRect.bottom_ == intRect.top_)
            intRect.bottom_++;

        if (intRect.right_ < intRect.left_ || intRect.bottom_ < intRect.top_)
            enable = false;

        if (enable && intRect != scissorRect_)
        {
            scissorRect_ = intRect;
            impl_->scissorRectDirty_ = true;
        }
    }

    if (enable != scissorTest_)
    {
        scissorTest_ = enable;
        impl_->rasterizerStateDirty_ = true;
    }
}

void Graphics::SetStencilTest(bool enable, CompareMode mode, StencilOp pass, StencilOp fail, StencilOp zFail, unsigned stencilRef,
    unsigned compareMask, unsigned writeMask)
{
    if (enable != stencilTest_)
    {
        stencilTest_ = enable;
        impl_->depthStateDirty_ = true;
    }

    if (enable)
    {
        if (mode != stencilTestMode_)
        {
            stencilTestMode_ = mode;
            impl_->depthStateDirty_ = true;
        }
        if (pass != stencilPass_)
        {
            stencilPass_ = pass;
            impl_->depthStateDirty_ = true;
        }
        if (fail != stencilFail_)
        {
            stencilFail_ = fail;
            impl_->depthStateDirty_ = true;
        }
        if (zFail != stencilZFail_)
        {
            stencilZFail_ = zFail;
            impl_->depthStateDirty_ = true;
        }
        if (compareMask != stencilCompareMask_)
        {
            stencilCompareMask_ = compareMask;
            impl_->depthStateDirty_ = true;
        }
        if (writeMask != stencilWriteMask_)
        {
            stencilWriteMask_ = writeMask;
            impl_->depthStateDirty_ = true;
        }
        if (stencilRef != stencilRef_)
        {
            stencilRef_ = stencilRef;
            impl_->stencilRefDirty_ = true;
            impl_->depthStateDirty_ = true;
        }
    }
}

void Graphics::SetClipPlane(bool enable, const Plane& clipPlane, const Matrix3x4& view, const Matrix4& projection)
{
    useClipPlane_ = enable;

    if (enable)
    {
        Matrix4 viewProj = projection * view;
        clipPlane_ = clipPlane.Transformed(viewProj).ToVector4();
        SetShaderParameter(VSP_CLIPPLANE, clipPlane_);
    }
}

bool Graphics::IsInitialized() const
{
    return window_ != nullptr && impl_->GetDevice() != nullptr;
}

PODVector<int> Graphics::GetMultiSampleLevels() const
{
#if 0
    PODVector<int> ret;
    ret.Push(1);

    if (impl_->device_)
    {
        for (unsigned i = 2; i <= 16; ++i)
        {
            if (impl_->CheckMultiSampleSupport(sRGB_ ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, i))
                ret.Push(i);
        }
    }

    return ret;
#else
    NOT_IMPLEMENTED();

    return PODVector<int>();
#endif
}

unsigned Graphics::GetFormat(CompressedFormat format) const
{
#if 0
    switch (format)
    {
    case CF_RGBA:
        return DXGI_FORMAT_R8G8B8A8_UNORM;

    case CF_DXT1:
        return DXGI_FORMAT_BC1_UNORM;

    case CF_DXT3:
        return DXGI_FORMAT_BC2_UNORM;

    case CF_DXT5:
        return DXGI_FORMAT_BC3_UNORM;

    default:
        return 0;
    }
#else
    NOT_IMPLEMENTED();

    return 0;
#endif
}

ShaderVariation* Graphics::GetShader(ShaderType type, const String& name, const String& defines) const
{
    return GetShader(type, name.CString(), defines.CString());
}

ShaderVariation* Graphics::GetShader(ShaderType type, const char* name, const char* defines) const
{
    if (lastShaderName_ != name || !lastShader_)
    {
        ResourceCache* cache = GetSubsystem<ResourceCache>();

        String fullShaderName = shaderPath_ + name + shaderExtension_;
        // Try to reduce repeated error log prints because of missing shaders
        if (lastShaderName_ == name && !cache->Exists(fullShaderName))
            return nullptr;

        lastShader_ = cache->GetResource<Shader>(fullShaderName);
        lastShaderName_ = name;
    }

    return lastShader_ ? lastShader_->GetVariation(type, defines) : nullptr;
}

VertexBuffer* Graphics::GetVertexBuffer(unsigned index) const
{
    return index < MAX_VERTEX_STREAMS ? vertexBuffers_[index] : nullptr;
}

ShaderProgram* Graphics::GetShaderProgram() const
{
    return impl_->shaderProgram_;
}

TextureUnit Graphics::GetTextureUnit(const String& name)
{
    HashMap<String, TextureUnit>::Iterator i = textureUnits_.Find(name);
    if (i != textureUnits_.End())
        return i->second_;
    else
        return MAX_TEXTURE_UNITS;
}

const String& Graphics::GetTextureUnitName(TextureUnit unit)
{
#if 0
    for (HashMap<String, TextureUnit>::Iterator i = textureUnits_.Begin(); i != textureUnits_.End(); ++i)
    {
        if (i->second_ == unit)
            return i->first_;
    }
    return String::EMPTY;
#else
    NOT_IMPLEMENTED();

    return String::EMPTY;
#endif
}

Texture* Graphics::GetTexture(unsigned index) const
{
    return index < MAX_TEXTURE_UNITS ? textures_[index] : nullptr;
}

RenderSurface* Graphics::GetRenderTarget(unsigned index) const
{
    return index < MAX_RENDERTARGETS ? renderTargets_[index] : nullptr;
}

IntVector2 Graphics::GetRenderTargetDimensions() const
{
    int width, height;

    if (renderTargets_[0])
    {
        width = renderTargets_[0]->GetWidth();
        height = renderTargets_[0]->GetHeight();
    }
    else if (depthStencil_) // Depth-only rendering
    {
        width = depthStencil_->GetWidth();
        height = depthStencil_->GetHeight();
    }
    else
    {
        width = width_;
        height = height_;
    }

    return IntVector2(width, height);
}

bool Graphics::GetDither() const
{
    return false;
}

bool Graphics::IsDeviceLost() const
{
    // Diligent graphics context is never considered lost
    /// \todo The device could be lost in case of graphics adapters getting disabled during runtime. This is not currently handled
    return false;
}

void Graphics::OnWindowResized()
{
    if (!impl_->device_ || !window_)
        return;

    int newWidth, newHeight;

    SDL_GetWindowSize(window_, &newWidth, &newHeight);
    if (newWidth == width_ && newHeight == height_)
        return;

    UpdateSwapChain(newWidth, newHeight);

    // Reset rendertargets and viewport for the new screen size
    ResetRenderTargets();

    URHO3D_LOGDEBUGF("Window was resized to %dx%d", width_, height_);

    using namespace ScreenMode;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_WIDTH] = width_;
    eventData[P_HEIGHT] = height_;
    eventData[P_FULLSCREEN] = screenParams_.fullscreen_;
    eventData[P_RESIZABLE] = screenParams_.resizable_;
    eventData[P_BORDERLESS] = screenParams_.borderless_;
    eventData[P_HIGHDPI] = screenParams_.highDPI_;
    SendEvent(E_SCREENMODE, eventData);
}

void Graphics::OnWindowMoved()
{
#if 0
    if (!impl_->device_ || !window_ || screenParams_.fullscreen_)
        return;

    int newX, newY;

    SDL_GetWindowPosition(window_, &newX, &newY);
    if (newX == position_.x_ && newY == position_.y_)
        return;

    position_.x_ = newX;
    position_.y_ = newY;

    URHO3D_LOGTRACEF("Window was moved to %d,%d", position_.x_, position_.y_);

    using namespace WindowPos;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_X] = position_.x_;
    eventData[P_Y] = position_.y_;
    SendEvent(E_WINDOWPOS, eventData);
#else
    PARTIALLY_IMPLEMENTED();

    if (!impl_->device_ || !window_ || screenParams_.fullscreen_)
        return;

    int newX, newY;

    SDL_GetWindowPosition(window_, &newX, &newY);
    if (newX == position_.x_ && newY == position_.y_)
        return;

    position_.x_ = newX;
    position_.y_ = newY;

    URHO3D_LOGTRACEF("Window was moved to %d,%d", position_.x_, position_.y_);

    using namespace WindowPos;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_X] = position_.x_;
    eventData[P_Y] = position_.y_;
    SendEvent(E_WINDOWPOS, eventData);
#endif
}

void Graphics::CleanupShaderPrograms(ShaderVariation* variation)
{
#if 0
    for (ShaderProgramMap::Iterator i = impl_->shaderPrograms_.Begin(); i != impl_->shaderPrograms_.End();)
    {
        if (i->first_.first_ == variation || i->first_.second_ == variation)
            i = impl_->shaderPrograms_.Erase(i);
        else
            ++i;
    }

    if (vertexShader_ == variation || pixelShader_ == variation)
        impl_->shaderProgram_ = nullptr;
#else
    NOT_IMPLEMENTED();
#endif
}

void Graphics::CleanupRenderSurface(RenderSurface* surface)
{
    // No-op on Diligent
}

ConstantBuffer* Graphics::GetOrCreateConstantBuffer(ShaderType type, unsigned index, unsigned size)
{
    // Ensure that different shader types and index slots get unique buffers, even if the size is same
    unsigned key = type | (index << 1) | (size << 4);
    ConstantBufferMap::Iterator i = impl_->allConstantBuffers_.Find(key);
    if (i != impl_->allConstantBuffers_.End())
        return i->second_.Get();
    else
    {
        SharedPtr<ConstantBuffer> newConstantBuffer(new ConstantBuffer(context_));
        newConstantBuffer->SetSize(size);
        impl_->allConstantBuffers_[key] = newConstantBuffer;
        return newConstantBuffer.Get();
    }
}

unsigned Graphics::GetAlphaFormat()
{
    return Diligent::TEX_FORMAT_A8_UNORM;
}

unsigned Graphics::GetLuminanceFormat()
{
    return Diligent::TEX_FORMAT_R8_UNORM;
}

unsigned Graphics::GetLuminanceAlphaFormat()
{
    return Diligent::TEX_FORMAT_RG8_UNORM;
}

unsigned Graphics::GetRGBFormat()
{
    return Diligent::TEX_FORMAT_RGBA8_UNORM;
}

unsigned Graphics::GetRGBAFormat()
{
    return Diligent::TEX_FORMAT_RGBA8_UNORM;
}

unsigned Graphics::GetRGBA16Format()
{
    return Diligent::TEX_FORMAT_RGBA16_UNORM;
}

unsigned Graphics::GetRGBAFloat16Format()
{
    return Diligent::TEX_FORMAT_RGBA16_FLOAT;
}

unsigned Graphics::GetRGBAFloat32Format()
{
    return Diligent::TEX_FORMAT_RGBA32_FLOAT;
}

unsigned Graphics::GetRG16Format()
{
    return Diligent::TEX_FORMAT_RG16_UNORM;
}

unsigned Graphics::GetRGFloat16Format()
{
    return Diligent::TEX_FORMAT_RG16_FLOAT;
}

unsigned Graphics::GetRGFloat32Format()
{
    return Diligent::TEX_FORMAT_RG32_FLOAT;
}

unsigned Graphics::GetFloat16Format()
{
    return Diligent::TEX_FORMAT_R16_FLOAT;
}

unsigned Graphics::GetFloat32Format()
{
    return Diligent::TEX_FORMAT_R32_FLOAT;
}

unsigned Graphics::GetLinearDepthFormat()
{
    return Diligent::TEX_FORMAT_R32_FLOAT;
}

unsigned Graphics::GetDepthStencilFormat()
{
    return Diligent::TEX_FORMAT_R24G8_TYPELESS;
}

unsigned Graphics::GetReadableDepthFormat()
{
    return Diligent::TEX_FORMAT_R24G8_TYPELESS;
}

unsigned Graphics::GetFormat(const String& formatName)
{
    String nameLower = formatName.ToLower().Trimmed();

    if (nameLower == "a")
        return GetAlphaFormat();
    if (nameLower == "l")
        return GetLuminanceFormat();
    if (nameLower == "la")
        return GetLuminanceAlphaFormat();
    if (nameLower == "rgb")
        return GetRGBFormat();
    if (nameLower == "rgba")
        return GetRGBAFormat();
    if (nameLower == "rgba16")
        return GetRGBA16Format();
    if (nameLower == "rgba16f")
        return GetRGBAFloat16Format();
    if (nameLower == "rgba32f")
        return GetRGBAFloat32Format();
    if (nameLower == "rg16")
        return GetRG16Format();
    if (nameLower == "rg16f")
        return GetRGFloat16Format();
    if (nameLower == "rg32f")
        return GetRGFloat32Format();
    if (nameLower == "r16f")
        return GetFloat16Format();
    if (nameLower == "r32f" || nameLower == "float")
        return GetFloat32Format();
    if (nameLower == "lineardepth" || nameLower == "depth")
        return GetLinearDepthFormat();
    if (nameLower == "d24s8")
        return GetDepthStencilFormat();
    if (nameLower == "readabledepth" || nameLower == "hwdepth")
        return GetReadableDepthFormat();

    return GetRGBFormat();
}

unsigned Graphics::GetMaxBones()
{
    return 128;
}

bool Graphics::GetGL3Support()
{
#if 0
    return gl3Support;
#else
    NOT_IMPLEMENTED();

    return false;
#endif
}

bool Graphics::OpenWindow(int width, int height, bool resizable, bool borderless)
{
    if (!externalWindow_)
    {
        unsigned flags = 0;
        if (resizable)
            flags |= SDL_WINDOW_RESIZABLE;
        if (borderless)
            flags |= SDL_WINDOW_BORDERLESS;

        window_ = SDL_CreateWindow(windowTitle_.CString(), position_.x_, position_.y_, width, height, flags);
    }
    else
        window_ = SDL_CreateWindowFrom(externalWindow_, 0);

    if (!window_)
    {
        URHO3D_LOGERRORF("Could not create window, root cause: '%s'", SDL_GetError());
        return false;
    }

    SDL_GetWindowPosition(window_, &position_.x_, &position_.y_);

    CreateWindowIcon();

    return true;
}

void Graphics::AdjustWindow(int& newWidth, int& newHeight, bool& newFullscreen, bool& newBorderless, int& monitor)
{
    if (!externalWindow_)
    {
        // Keep current window position because it may change in intermediate callbacks
        const IntVector2 oldPosition = position_;
        bool reposition = false;
        bool resizePostponed = false;
        if (!newWidth || !newHeight)
        {
            SDL_MaximizeWindow(window_);
            SDL_GetWindowSize(window_, &newWidth, &newHeight);
        }
        else
        {
            SDL_Rect display_rect;
            SDL_GetDisplayBounds(monitor, &display_rect);

            reposition = newFullscreen || (newBorderless && newWidth >= display_rect.w && newHeight >= display_rect.h);
            if (reposition)
            {
                // Reposition the window on the specified monitor if it's supposed to cover the entire monitor
                SDL_SetWindowPosition(window_, display_rect.x, display_rect.y);
            }

            // Postpone window resize if exiting fullscreen to avoid redundant resolution change
            if (!newFullscreen && screenParams_.fullscreen_)
                resizePostponed = true;
            else
                SDL_SetWindowSize(window_, newWidth, newHeight);
        }

        // Turn off window fullscreen mode so it gets repositioned to the correct monitor
        SDL_SetWindowFullscreen(window_, SDL_FALSE);
        // Hack fix: on SDL 2.0.4 a fullscreen->windowed transition results in a maximized window when the D3D device is reset, so hide before
        if (!newFullscreen) SDL_HideWindow(window_);
        SDL_SetWindowFullscreen(window_, newFullscreen ? SDL_WINDOW_FULLSCREEN : 0);
        SDL_SetWindowBordered(window_, newBorderless ? SDL_FALSE : SDL_TRUE);
        if (!newFullscreen) SDL_ShowWindow(window_);

        // Resize now if was postponed
        if (resizePostponed)
            SDL_SetWindowSize(window_, newWidth, newHeight);

        // Ensure that window keeps its position
        if (!reposition)
            SDL_SetWindowPosition(window_, oldPosition.x_, oldPosition.y_);
        else
            position_ = oldPosition;
    }
    else
    {
        // If external window, must ask its dimensions instead of trying to set them
        SDL_GetWindowSize(window_, &newWidth, &newHeight);
        newFullscreen = false;
    }
}

bool Graphics::CreateDevice(int width, int height)
{
    auto* pFactoryD3D11 = Diligent::GetEngineFactoryD3D11();

    if (!impl_->device_)
    {
        Diligent::EngineD3D11CreateInfo EngineCI;
        pFactoryD3D11->CreateDeviceAndContextsD3D11(EngineCI, &impl_->device_, &impl_->deviceContext_);

        CheckFeatureSupport();
    }

    Diligent::Win32NativeWindow Window{GetWindowHandle(window_)};
    impl_->swapChainInitDesc_.BufferCount = 16;
    impl_->swapChainInitDesc_.Width = (UINT)width;
    impl_->swapChainInitDesc_.Height = (UINT)height;
    impl_->swapChainInitDesc_.ColorBufferFormat = sRGB_ ? TEX_FORMAT_RGBA8_UNORM_SRGB : TEX_FORMAT_RGBA8_UNORM;
    impl_->swapChainInitDesc_.DepthBufferFormat = TEX_FORMAT_D32_FLOAT;
    impl_->swapChainInitDesc_.DefaultDepthValue = 0.f;

    pFactoryD3D11->CreateSwapChainD3D11(impl_->device_, impl_->deviceContext_, impl_->swapChainInitDesc_,
                                        Diligent::FullScreenModeDesc{}, Window, &impl_->swapChain_);

    impl_->deviceType_ = Diligent::RENDER_DEVICE_TYPE_D3D11;

    return true;
}

bool Graphics::UpdateSwapChain(int width, int height)
{
    bool success = true;

    impl_->deviceContext_->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    if (impl_->defaultRenderTargetView_)
    {
        impl_->defaultRenderTargetView_->Release();
        impl_->defaultRenderTargetView_ = nullptr;
    }
    if (impl_->defaultDepthStencilView_)
    {
        impl_->defaultDepthStencilView_->Release();
        impl_->defaultDepthStencilView_ = nullptr;
    }
    if (impl_->resolveTexture_)
    {
        impl_->resolveTexture_->Release();
        impl_->resolveTexture_ = nullptr;
    }

    impl_->depthStencilView_ = nullptr;
    for (unsigned i = 0; i < MAX_RENDERTARGETS; ++i)
        impl_->renderTargetViews_[i] = nullptr;
    impl_->renderTargetsDirty_ = true;

    impl_->swapChain_->Resize((uint32_t)width, (uint32_t)height);

    impl_->defaultRenderTargetView_ = impl_->swapChain_->GetCurrentBackBufferRTV();
    if (impl_->defaultRenderTargetView_ == nullptr)
    {
        URHO3D_LOGERROR("Failed to get backbuffer rendertarget view");
        success = false;
    }

    impl_->defaultDepthStencilView_ = impl_->swapChain_->GetDepthBufferDSV();
    if (impl_->defaultDepthStencilView_ == nullptr)
    {
        URHO3D_LOGERROR("Failed to get depth-stencil view");
        success = false;
    }

    // Update internally held backbuffer size
    width_ = width;
    height_ = height;

    ResetRenderTargets();

    return true;
}

void Graphics::CheckFeatureSupport()
{
    anisotropySupport_ = true;
    dxtTextureSupport_ = true;
    lightPrepassSupport_ = true;
    deferredSupport_ = true;
    hardwareShadowSupport_ = true;
    instancingSupport_ = true;
    shadowMapFormat_ = TEX_FORMAT_R16_TYPELESS;
    hiresShadowMapFormat_ = TEX_FORMAT_R32_TYPELESS;
    dummyColorFormat_ = TEX_FORMAT_UNKNOWN;
    sRGBSupport_ = true;
    sRGBWriteSupport_ = true;
}

void Graphics::ResetCachedState()
{
    for (unsigned i = 0; i < MAX_VERTEX_STREAMS; ++i)
    {
        vertexBuffers_[i] = nullptr;
        impl_->vertexBuffers_[i] = nullptr;
        impl_->vertexSizes_[i] = 0;
        impl_->vertexOffsets_[i] = 0;
    }

    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
    {
        textures_[i] = nullptr;
        impl_->shaderResourceViews_[i] = nullptr;
        impl_->samplers_[i] = nullptr;
    }

    for (unsigned i = 0; i < MAX_RENDERTARGETS; ++i)
    {
        renderTargets_[i] = nullptr;
        impl_->renderTargetViews_[i] = nullptr;
    }

    for (unsigned i = 0; i < MAX_SHADER_PARAMETER_GROUPS; ++i)
    {
        impl_->constantBuffers_[VS][i] = nullptr;
        impl_->constantBuffers_[PS][i] = nullptr;
    }

    depthStencil_ = nullptr;
    impl_->depthStencilView_ = nullptr;
    viewport_ = IntRect(0, 0, width_, height_);

    indexBuffer_ = nullptr;
    vertexDeclarationHash_ = 0;
    primitiveType_ = 0;
    vertexShader_ = nullptr;
    pixelShader_ = nullptr;
    blendMode_ = BLEND_REPLACE;
    alphaToCoverage_ = false;
    colorWrite_ = true;
    cullMode_ = CULL_CCW;
    constantDepthBias_ = 0.0f;
    slopeScaledDepthBias_ = 0.0f;
    depthTestMode_ = CMP_LESSEQUAL;
    depthWrite_ = true;
    fillMode_ = FILL_SOLID;
    lineAntiAlias_ = false;
    scissorTest_ = false;
    scissorRect_ = IntRect::ZERO;
    stencilTest_ = false;
    stencilTestMode_ = CMP_ALWAYS;
    stencilPass_ = OP_KEEP;
    stencilFail_ = OP_KEEP;
    stencilZFail_ = OP_KEEP;
    stencilRef_ = 0;
    stencilCompareMask_ = M_MAX_UNSIGNED;
    stencilWriteMask_ = M_MAX_UNSIGNED;
    useClipPlane_ = false;
    impl_->shaderProgram_ = nullptr;
    impl_->renderTargetsDirty_ = true;
    impl_->texturesDirty_ = true;
    impl_->vertexDeclarationDirty_ = true;
    impl_->blendStateDirty_ = true;
    impl_->depthStateDirty_ = true;
    impl_->rasterizerStateDirty_ = true;
    impl_->scissorRectDirty_ = true;
    impl_->stencilRefDirty_ = true;
    impl_->blendStateHash_ = M_MAX_UNSIGNED;
    impl_->depthStateHash_ = M_MAX_UNSIGNED;
    impl_->rasterizerStateHash_ = M_MAX_UNSIGNED;
    impl_->firstDirtyTexture_ = impl_->lastDirtyTexture_ = M_MAX_UNSIGNED;
    impl_->firstDirtyVB_ = impl_->lastDirtyVB_ = M_MAX_UNSIGNED;
    impl_->dirtyConstantBuffers_.Clear();
}

void Graphics::PrepareDraw()
{
    bool pipelineStateChanged = false;
    if (impl_->renderTargetsDirty_)
    {
        impl_->depthStencilView_ = (depthStencil_ && depthStencil_->GetUsage() == TEXTURE_DEPTHSTENCIL)
                                       ? (ITextureView*)depthStencil_->GetRenderTargetView()
                                       : impl_->defaultDepthStencilView_;

        // If possible, bind a read-only depth stencil view to allow reading depth in shader
        if (!depthWrite_ && depthStencil_ && depthStencil_->GetReadOnlyView())
            impl_->depthStencilView_ = (ITextureView*)depthStencil_->GetReadOnlyView();

        for (unsigned i = 0; i < MAX_RENDERTARGETS; ++i)
            impl_->renderTargetViews_[i] = (renderTargets_[i] && renderTargets_[i]->GetUsage() == TEXTURE_RENDERTARGET)
                                               ? (ITextureView*)renderTargets_[i]->GetRenderTargetView()
                                               : nullptr;
        // If rendertarget 0 is null and not doing depth-only rendering, render to the backbuffer
        // Special case: if rendertarget 0 is null and depth stencil has same size as backbuffer, assume the intention
        // is to do backbuffer rendering with a custom depth stencil
        if (!renderTargets_[0] && (!depthStencil_ || (depthStencil_ && depthStencil_->GetWidth() == width_ &&
                                                      depthStencil_->GetHeight() == height_)))
            impl_->renderTargetViews_[0] = impl_->defaultRenderTargetView_;

        impl_->deviceContext_->SetRenderTargets(MAX_RENDERTARGETS, &impl_->renderTargetViews_[0],
                                                impl_->depthStencilView_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // TODO: Figure out why is this necessary
        SetViewport(viewport_);
        impl_->renderTargetsDirty_ = false;
    }

    if (vertexShader_ == nullptr || pixelShader_ == nullptr)
    {
        return;
    }

    if (impl_->vertexShaderDirty_ || impl_->pixelShaderDirty_ || impl_->blendStateDirty_ || impl_->depthStateDirty_ ||
        impl_->rasterizerStateDirty_ || impl_->primitiveTypeDirty_ || impl_->vertexDeclarationDirty_)
    {
        bool pipelineStateDirty =
            impl_->vertexShaderDirty_ || impl_->pixelShaderDirty_;

        if (impl_->vertexDeclarationDirty_ && vertexShader_)
        {
            if (impl_->firstDirtyVB_ < M_MAX_UNSIGNED)
            {
                impl_->deviceContext_->SetVertexBuffers(
                    impl_->firstDirtyVB_, impl_->lastDirtyVB_ - impl_->firstDirtyVB_ + 1,
                    &impl_->vertexBuffers_[impl_->firstDirtyVB_], &impl_->vertexOffsets_[impl_->firstDirtyVB_],
                    RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_NONE);

                impl_->firstDirtyVB_ = impl_->lastDirtyVB_ = M_MAX_UNSIGNED;
            }

            unsigned long long newVertexDeclarationHash = 0;
            for (unsigned i = 0; i < MAX_VERTEX_STREAMS; ++i)
            {
                if (vertexBuffers_[i])
                    newVertexDeclarationHash |= vertexBuffers_[i]->GetBufferHash(i);
            }

            if (newVertexDeclarationHash)
            {
                /// \todo Using a 64bit total hash for vertex shader and vertex buffer elements hash may not guarantee
                /// uniqueness
                // TODO: Check if vertex shader element hash is needed
//                newVertexDeclarationHash += vertexShader_->GetElementHash();
                if (newVertexDeclarationHash != vertexDeclarationHash_)
                {
                    pipelineStateDirty = true;
                    vertexDeclarationHash_ = newVertexDeclarationHash;
                }
            }
        }

        if (impl_->primitiveTypeDirty_)
        {
            pipelineStateDirty = true;
            impl_->primitiveTypeDirty_ = false;
        }

        if (impl_->blendStateDirty_)
        {
            unsigned newBlendStateHash =
                (unsigned)((colorWrite_ ? 1 : 0) | (alphaToCoverage_ ? 2 : 0) | (blendMode_ << 2));
            if (newBlendStateHash != impl_->blendStateHash_)
            {
                pipelineStateDirty = true;
                impl_->blendStateHash_ = newBlendStateHash;
                const float blendFactors[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                impl_->deviceContext_->SetBlendFactors(blendFactors);
            }

            impl_->blendStateDirty_ = false;
        }

        if (impl_->depthStateDirty_)
        {
            unsigned newDepthStateHash = (depthWrite_ ? 1 : 0) | (stencilTest_ ? 2 : 0) | (depthTestMode_ << 2) |
                                         ((stencilCompareMask_ & 0xff) << 5) | ((stencilWriteMask_ & 0xff) << 13) |
                                         (stencilTestMode_ << 21) |
                                         ((stencilFail_ + stencilZFail_ * 5 + stencilPass_ * 25) << 24);
            if (newDepthStateHash != impl_->depthStateHash_ || impl_->stencilRefDirty_)
            {
                pipelineStateDirty = true;
                impl_->depthStateHash_ = newDepthStateHash;
            }

            impl_->depthStateDirty_ = false;
        }

        unsigned depthBits = 24;
        if (depthStencil_ && depthStencil_->GetParentTexture()->GetFormat() == DXGI_FORMAT_R16_TYPELESS)
            depthBits = 16;
        int scaledDepthBias = (int)(constantDepthBias_ * (1 << depthBits));

        if (impl_->rasterizerStateDirty_)
        {
            unsigned newRasterizerStateHash = (scissorTest_ ? 1 : 0) | (lineAntiAlias_ ? 2 : 0) | (fillMode_ << 2) |
                                              (cullMode_ << 4) | ((scaledDepthBias & 0x1fff) << 6) |
                                              (((int)(slopeScaledDepthBias_ * 100.0f) & 0x1fff) << 19);
            if (newRasterizerStateHash != impl_->rasterizerStateHash_)
            {
                pipelineStateDirty = true;
                impl_->rasterizerStateHash_ = newRasterizerStateHash;
            }

            impl_->rasterizerStateDirty_ = false;
        }

        impl_->vertexShaderDirty_ = false;
        impl_->pixelShaderDirty_ = false;

        if (pipelineStateDirty)
        {
            RefCntAutoPtr<IPipelineState> pipelineState;
            RefCntAutoPtr<IShaderResourceBinding> shaderResourceBinding;
            std::shared_ptr<GraphicsImpl::PipelineState::TextureMap> textureMap;

            auto pipelineKey = std::make_tuple(vertexShader_, pixelShader_, impl_->blendStateHash_,
                                               impl_->depthStateHash_, impl_->rasterizerStateHash_,
                                               vertexDeclarationHash_, impl_->primitiveType_);
            auto iterator = impl_->pipelineStates_.find(pipelineKey);
            if (iterator == impl_->pipelineStates_.end())
            {
                GraphicsPipelineStateCreateInfo pipelineStateCreateInfo;
                // TODO: Remove
#if 1
                static int count = 0;
                String psoName = String().AppendWithFormat("%d", count++);
                pipelineStateCreateInfo.PSODesc.Name = psoName.CString();
#else
                pipelineStateCreateInfo.PSODesc.Name = "";
#endif
                pipelineStateCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
                pipelineStateCreateInfo.GraphicsPipeline.NumRenderTargets = 1;
                pipelineStateCreateInfo.GraphicsPipeline.RTVFormats[0] = impl_->swapChain_->GetDesc().ColorBufferFormat;
                pipelineStateCreateInfo.GraphicsPipeline.DSVFormat = impl_->swapChain_->GetDesc().DepthBufferFormat;
                pipelineStateCreateInfo.GraphicsPipeline.PrimitiveTopology = impl_->GetPrimitiveTopology();

                IShader* vertexShader = (IShader*)vertexShader_->GetGPUObject();
                IShader* pixelShader = (IShader*)pixelShader_->GetGPUObject();
                pipelineStateCreateInfo.pVS = vertexShader;
                pipelineStateCreateInfo.pPS = pixelShader;

                pipelineStateCreateInfo.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

                PODVector<ShaderResourceVariableDesc> variableDescriptors;
                for (uint32_t i = 0; i < vertexShader->GetResourceCount(); i++)
                {
                    ShaderResourceDesc srd;
                    vertexShader->GetResourceDesc(i, srd);
                    if (srd.Type == SHADER_RESOURCE_TYPE_TEXTURE_SRV)
                    {
                        variableDescriptors.Push(ShaderResourceVariableDesc(SHADER_TYPE_VERTEX, srd.Name,
                                                                            SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC));
                    }
                }

                for (uint32_t i = 0; i < pixelShader->GetResourceCount(); i++)
                {
                    ShaderResourceDesc srd;
                    pixelShader->GetResourceDesc(i, srd);
                    if (srd.Type == SHADER_RESOURCE_TYPE_TEXTURE_SRV)
                    {
                        variableDescriptors.Push(ShaderResourceVariableDesc(SHADER_TYPE_PIXEL, srd.Name,
                                                                            SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC));
                    }
                }
                pipelineStateCreateInfo.PSODesc.ResourceLayout.Variables = variableDescriptors.Size() > 0 ? &variableDescriptors[0] : nullptr;
                pipelineStateCreateInfo.PSODesc.ResourceLayout.NumVariables = variableDescriptors.Size();

                pipelineStateCreateInfo.PSODesc.ResourceLayout.ImmutableSamplers = nullptr;
                pipelineStateCreateInfo.PSODesc.ResourceLayout.NumImmutableSamplers = 0;

                PODVector<LayoutElement> layoutElements;
                unsigned prevLayoutElementsCount = 0;

                layoutElements.Reserve(MAX_VERTEX_STREAMS);
                for (unsigned i = 0; i < MAX_VERTEX_STREAMS; ++i)
                {
                    if (!vertexBuffers_[i])
                        continue;

                    const PODVector<VertexElement>& srcElements = vertexBuffers_[i]->GetElements();
                    bool isExisting = false;

                    for (unsigned j = 0; j < srcElements.Size(); ++j)
                    {
                        const VertexElement& srcElement = srcElements[j];
                        const char* semanticName = ShaderVariation::elementSemanticNames[srcElement.semantic_];

                        // Override existing element if necessary
                        for (unsigned k = 0; k < prevLayoutElementsCount; ++k)
                        {
                            if (layoutElements[k].HLSLSemantic == semanticName &&
                                layoutElements[k].InputIndex == srcElement.index_)
                            {
                                isExisting = true;
                                layoutElements[k].BufferSlot = i;
                                layoutElements[k].RelativeOffset = srcElement.offset_;
                                layoutElements[k].Frequency = srcElement.perInstance_ ? INPUT_ELEMENT_FREQUENCY_PER_INSTANCE
                                                                                      : INPUT_ELEMENT_FREQUENCY_PER_VERTEX;
                                layoutElements[k].InstanceDataStepRate = srcElement.perInstance_ ? 1 : 0;
                                break;
                            }
                        }

                        if (isExisting)
                            continue;

                        LayoutElement newLayoutElement;
                        newLayoutElement.HLSLSemantic = semanticName;
                        newLayoutElement.InputIndex = srcElement.index_;
                        newLayoutElement.ValueType = diligentValueType[srcElement.type_];
                        newLayoutElement.NumComponents = diligentNumComponents[srcElement.type_];
                        newLayoutElement.IsNormalized = diligentIsNormalized[srcElement.type_];
                        newLayoutElement.BufferSlot = (Uint32)i;
                        newLayoutElement.RelativeOffset = srcElement.offset_;
                        newLayoutElement.Frequency = srcElement.perInstance_ ? INPUT_ELEMENT_FREQUENCY_PER_INSTANCE
                                                                             : INPUT_ELEMENT_FREQUENCY_PER_VERTEX;
                        newLayoutElement.InstanceDataStepRate = srcElement.perInstance_ ? 1 : 0;
                        layoutElements.Push(newLayoutElement);
                    }

                    prevLayoutElementsCount = layoutElements.Size();
                }

                pipelineStateCreateInfo.GraphicsPipeline.InputLayout.LayoutElements = layoutElements.Size() > 0 ? &layoutElements[0] : nullptr;
                pipelineStateCreateInfo.GraphicsPipeline.InputLayout.NumElements = layoutElements.Size();

                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.AlphaToCoverageEnable = alphaToCoverage_ ? true : false;
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.IndependentBlendEnable = false;
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].BlendEnable = diligentBlendEnable[blendMode_];
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].SrcBlend = diligentSrcBlend[blendMode_];
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].DestBlend = diligentDestBlend[blendMode_];
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].BlendOp = diligentBlendOp[blendMode_];
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].SrcBlendAlpha = diligentSrcBlend[blendMode_];
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].DestBlendAlpha = diligentDestBlend[blendMode_];
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].BlendOpAlpha = diligentBlendOp[blendMode_];
                pipelineStateCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0].RenderTargetWriteMask = colorWrite_ ? COLOR_MASK_ALL : COLOR_MASK_NONE;

                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = depthWrite_;
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthFunc = diligentCmpFunc[depthTestMode_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.StencilEnable = stencilTest_;
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.StencilReadMask = (Uint8)stencilCompareMask_;
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.StencilWriteMask = (Uint8)stencilWriteMask_;
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilFailOp = diligentStencilOp[stencilFail_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilDepthFailOp = diligentStencilOp[stencilZFail_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilPassOp = diligentStencilOp[stencilPass_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilFunc = diligentCmpFunc[stencilTestMode_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.BackFace.StencilFailOp = diligentStencilOp[stencilFail_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.BackFace.StencilDepthFailOp = diligentStencilOp[stencilZFail_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.BackFace.StencilPassOp = diligentStencilOp[stencilPass_];
                pipelineStateCreateInfo.GraphicsPipeline.DepthStencilDesc.BackFace.StencilFunc = diligentCmpFunc[stencilTestMode_];

                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.FillMode = diligentFillMode[fillMode_];
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.CullMode = diligentCullMode[cullMode_];
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.FrontCounterClockwise = false;
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.DepthBias = scaledDepthBias;
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.DepthBiasClamp = M_INFINITY;
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.SlopeScaledDepthBias = slopeScaledDepthBias_;
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.DepthClipEnable = true;
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.ScissorEnable = scissorTest_;
                // TODO: Chack if MultisampleEnable is needed; in Diligent code MultisampleEnable is set to AntialiasedLineEnable
                //pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.MultisampleEnable = !lineAntiAlias_;
                pipelineStateCreateInfo.GraphicsPipeline.RasterizerDesc.AntialiasedLineEnable = lineAntiAlias_;

                impl_->device_->CreateGraphicsPipelineState(pipelineStateCreateInfo, &pipelineState);
                assert(pipelineState != nullptr);

                const unsigned* vsBufferSizes = vertexShader_->GetConstantBufferSizes();
                const String* vsBufferNames = vertexShader_->GetConstantBufferNames();
                for (unsigned i = 0; i < MAX_SHADER_PARAMETER_GROUPS; ++i)
                {
                    if (vsBufferSizes[i] > 0 && !vsBufferNames[i].Empty())
                    {
                        pipelineState->GetStaticVariableByName(SHADER_TYPE_VERTEX, vsBufferNames[i].CString())
                            ->Set((IBuffer*)impl_->shaderProgram_->vsConstantBuffers_[i]->GetGPUObject());
                    }
                }

                const unsigned* psBufferSizes = pixelShader_->GetConstantBufferSizes();
                const String* psBufferNames = pixelShader_->GetConstantBufferNames();
                for (unsigned i = 0; i < MAX_SHADER_PARAMETER_GROUPS; ++i)
                {
                    if (psBufferSizes[i] > 0 && !psBufferNames[i].Empty())
                    {
                        pipelineState->GetStaticVariableByName(SHADER_TYPE_PIXEL, psBufferNames[i].CString())
                            ->Set((IBuffer*)impl_->shaderProgram_->psConstantBuffers_[i]->GetGPUObject());
                    }
                }

                pipelineState->CreateShaderResourceBinding(&shaderResourceBinding, true);
                assert(shaderResourceBinding != nullptr);

                const Uint32 vertexShaderVariableCount = shaderResourceBinding->GetVariableCount(SHADER_TYPE_VERTEX);
                const Uint32 pixelShaderVariableCount = shaderResourceBinding->GetVariableCount(SHADER_TYPE_PIXEL);
                textureMap = std::make_shared<GraphicsImpl::PipelineState::TextureMap>();
                textureMap->reserve(vertexShaderVariableCount + pixelShaderVariableCount);

                auto numberPostfix = [](const char* str)
                {
                    for (unsigned i = 0; str[i] != '\0'; ++i)
                    {
                        if (IsDigit(str[i]))
                            return ToUInt(&str[i]);
                    }

                    return M_MAX_UNSIGNED;
                };

                auto getTextureUnitFromVariable = [&](IShaderResourceVariable* variable)
                {
                    ShaderResourceDesc shaderResourceDesc;
                    variable->GetResourceDesc(shaderResourceDesc);

                    const char* variableName =
                        shaderResourceDesc.Name[0] == 't' ? &shaderResourceDesc.Name[1] : shaderResourceDesc.Name;

                    unsigned textureUnit = GetTextureUnit(variableName);
                    if (textureUnit >= MAX_TEXTURE_UNITS)
                    {
                        textureUnit = numberPostfix(variableName);
                    }

                    return textureUnit;
                };

                for (Uint32 i = 0; i < vertexShaderVariableCount; i++)
                {
                    IShaderResourceVariable* variable = shaderResourceBinding->GetVariableByIndex(SHADER_TYPE_VERTEX, i);
                    unsigned textureUnit = getTextureUnitFromVariable(variable);

                    if (textureUnit < MAX_TEXTURE_UNITS)
                    {
                        textureMap->push_back(GraphicsImpl::PipelineState::TextureMapEntry{textureUnit, variable});
                    }
                }

                for (Uint32 i = 0; i < pixelShaderVariableCount; i++)
                {
                    IShaderResourceVariable* variable = shaderResourceBinding->GetVariableByIndex(SHADER_TYPE_PIXEL, i);
                    unsigned textureUnit = getTextureUnitFromVariable(variable);

                    if (textureUnit < MAX_TEXTURE_UNITS)
                    {
                        textureMap->push_back(GraphicsImpl::PipelineState::TextureMapEntry{textureUnit, variable});
                    }
                }

                impl_->pipelineStates_[pipelineKey] = GraphicsImpl::PipelineState{
                    pipelineState, shaderResourceBinding, textureMap};
            }
            else
            {
                pipelineState = iterator->second.pipelineState_;
                shaderResourceBinding = iterator->second.shaderResourceBinding_;
                textureMap = iterator->second.textureMap_;
            }

            if (impl_->currentPipelineState_ != pipelineState)
            {
                pipelineStateChanged = true;
                impl_->currentPipelineState_ = pipelineState;
                impl_->currentShaderResourceBinding_ = shaderResourceBinding;
                impl_->currentTextureMap_ = textureMap;
            }
        }
    }

    assert(impl_->currentPipelineState_ != nullptr);
    impl_->deviceContext_->SetPipelineState(impl_->currentPipelineState_);

    const auto& desc =
        impl_->currentPipelineState_->GetDesc();

    if (pipelineStateChanged || (impl_->texturesDirty_ && impl_->firstDirtyTexture_ < M_MAX_UNSIGNED))
    {
        if (impl_->currentTextureMap_)
        {
            const auto& textureMap = *impl_->currentTextureMap_.get();

            for (const auto& textureMapEntry : textureMap)
            {
                if ((pipelineStateChanged || (textureMapEntry.textureUnit >= impl_->firstDirtyTexture_ &&
                     textureMapEntry.textureUnit <= impl_->lastDirtyTexture_)) &&
                    impl_->shaderResourceViews_[textureMapEntry.textureUnit])
                {
                    impl_->shaderResourceViews_[textureMapEntry.textureUnit]->SetSampler(
                        impl_->samplers_[textureMapEntry.textureUnit]);
                    textureMapEntry.variable->Set(impl_->shaderResourceViews_[textureMapEntry.textureUnit]);
                }
            }
        }

        impl_->firstDirtyTexture_ = impl_->lastDirtyTexture_ = M_MAX_UNSIGNED;
        impl_->texturesDirty_ = false;
    }

    assert(impl_->currentShaderResourceBinding_);
    impl_->deviceContext_->CommitShaderResources(impl_->currentShaderResourceBinding_,
                                                 RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    if (impl_->scissorRectDirty_)
    {
        Diligent::Rect rect;
        rect.left = scissorRect_.left_;
        rect.top = scissorRect_.top_;
        rect.right = scissorRect_.right_;
        rect.bottom = scissorRect_.bottom_;
        impl_->deviceContext_->SetScissorRects(1, &rect, 0, 0);
        impl_->scissorRectDirty_ = false;
    }

    for (unsigned i = 0; i < impl_->dirtyConstantBuffers_.Size(); ++i)
        impl_->dirtyConstantBuffers_[i]->Apply();
    impl_->dirtyConstantBuffers_.Clear();
}

void Graphics::CreateResolveTexture()
{
    if (impl_->resolveTexture_)
        return;

    TextureDesc textureDesc;
    textureDesc.Type = RESOURCE_DIM_TEX_2D;
    textureDesc.Width = (UINT)width_;
    textureDesc.Height = (UINT)height_;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = TEX_FORMAT_RGBA8_UNORM;
    textureDesc.SampleCount = 1;
    // TODO: Is sample quality needed?
 //   textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = USAGE_DEFAULT;
    textureDesc.CPUAccessFlags = CPU_ACCESS_NONE;

    impl_->device_->CreateTexture(textureDesc, nullptr, &impl_->resolveTexture_);
    if (impl_->resolveTexture_ == nullptr)
    {
        URHO3D_LOGERROR("Could not create resolve texture");
    }
}

void Graphics::SetTextureUnitMappings()
{
    textureUnits_["DiffMap"] = TU_DIFFUSE;
    textureUnits_["DiffCubeMap"] = TU_DIFFUSE;
    textureUnits_["NormalMap"] = TU_NORMAL;
    textureUnits_["SpecMap"] = TU_SPECULAR;
    textureUnits_["EmissiveMap"] = TU_EMISSIVE;
    textureUnits_["EnvMap"] = TU_ENVIRONMENT;
    textureUnits_["EnvCubeMap"] = TU_ENVIRONMENT;
    textureUnits_["LightRampMap"] = TU_LIGHTRAMP;
    textureUnits_["LightSpotMap"] = TU_LIGHTSHAPE;
    textureUnits_["LightCubeMap"] = TU_LIGHTSHAPE;
    textureUnits_["ShadowMap"] = TU_SHADOWMAP;
    textureUnits_["FaceSelectCubeMap"] = TU_FACESELECT;
    textureUnits_["IndirectionCubeMap"] = TU_INDIRECTION;
    textureUnits_["VolumeMap"] = TU_VOLUMEMAP;
    textureUnits_["ZoneCubeMap"] = TU_ZONE;
    textureUnits_["ZoneVolumeMap"] = TU_ZONE;

    textureUnits_["AlbedoBuffer"] = TU_ALBEDOBUFFER;
    textureUnits_["NormalBuffer"] = TU_NORMALBUFFER;
    textureUnits_["RoughMetalFresnel"] = TU_SPECULAR;
    textureUnits_["DepthBuffer"] = TU_DEPTHBUFFER;
    textureUnits_["LightBuffer"] = TU_LIGHTBUFFER;
}

}
