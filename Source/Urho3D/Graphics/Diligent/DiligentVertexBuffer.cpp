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

#include "../../Graphics/Graphics.h"
#include "../../Graphics/GraphicsImpl.h"
#include "../../Graphics/VertexBuffer.h"
#include "../../IO/Log.h"

#include "../../DebugNew.h"

using namespace Diligent;

namespace Urho3D
{

void VertexBuffer::OnDeviceLost()
{
    // No-op on Diligent
}

void VertexBuffer::OnDeviceReset()
{
    // No-op on Diligent
}

void VertexBuffer::Release()
{
    Unlock();

    if (graphics_)
    {
        for (unsigned i = 0; i < MAX_VERTEX_STREAMS; ++i)
        {
            if (graphics_->GetVertexBuffer(i) == this)
                graphics_->SetVertexBuffer(nullptr);
        }
    }

    URHO3D_SAFE_RELEASE(object_.ptr_);
}

bool VertexBuffer::SetData(const void* data)
{
    if (!data)
    {
        URHO3D_LOGERROR("Null pointer for vertex buffer data");
        return false;
    }

    if (!vertexSize_)
    {
        URHO3D_LOGERROR("Vertex elements not defined, can not set vertex buffer data");
        return false;
    }

    if (shadowData_ && data != shadowData_.Get())
        memcpy(shadowData_.Get(), data, vertexCount_ * vertexSize_);

    if (object_.ptr_)
    {
        if (dynamic_)
        {
            void* hwData = MapBuffer(0, vertexCount_, true);
            if (hwData)
            {
                memcpy(hwData, data, vertexCount_ * vertexSize_);
                UnmapBuffer();
            }
            else
                return false;
        }
        else
        {
            // TODO: Make sure transition mode is correct
            graphics_->GetImpl()->GetDeviceContext()->UpdateBuffer(
                (IBuffer*)object_.ptr_, 0, vertexCount_ * vertexSize_,
                data, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    return true;
}

bool VertexBuffer::SetDataRange(const void* data, unsigned start, unsigned count, bool discard)
{
    if (start == 0 && count == vertexCount_)
        return SetData(data);

    if (!data)
    {
        URHO3D_LOGERROR("Null pointer for vertex buffer data");
        return false;
    }

    if (!vertexSize_)
    {
        URHO3D_LOGERROR("Vertex elements not defined, can not set vertex buffer data");
        return false;
    }

    if (start + count > vertexCount_)
    {
        URHO3D_LOGERROR("Illegal range for setting new vertex buffer data");
        return false;
    }

    if (!count)
        return true;

    if (shadowData_ && shadowData_.Get() + start * vertexSize_ != data)
        memcpy(shadowData_.Get() + start * vertexSize_, data, count * vertexSize_);

    if (object_.ptr_)
    {
        if (dynamic_)
        {
            void* hwData = MapBuffer(start, count, discard);
            if (hwData)
            {
                memcpy(hwData, data, count * vertexSize_);
                UnmapBuffer();
            }
            else
                return false;
        }
        else
        {
            graphics_->GetImpl()->GetDeviceContext()->UpdateBuffer((IBuffer*)object_.ptr_, start * vertexSize_,
                                                                   count * vertexSize_, data,
                                                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    return true;
}

void* VertexBuffer::Lock(unsigned start, unsigned count, bool discard)
{
    if (lockState_ != LOCK_NONE)
    {
        URHO3D_LOGERROR("Vertex buffer already locked");
        return nullptr;
    }

    if (!vertexSize_)
    {
        URHO3D_LOGERROR("Vertex elements not defined, can not lock vertex buffer");
        return nullptr;
    }

    if (start + count > vertexCount_)
    {
        URHO3D_LOGERROR("Illegal range for locking vertex buffer");
        return nullptr;
    }

    if (!count)
        return nullptr;

    lockStart_ = start;
    lockCount_ = count;

    // Because shadow data must be kept in sync, can only lock hardware buffer if not shadowed
    if (object_.ptr_ && !shadowData_ && dynamic_)
        return MapBuffer(start, count, discard);
    else if (shadowData_)
    {
        lockState_ = LOCK_SHADOW;
        return shadowData_.Get() + start * vertexSize_;
    }
    else if (graphics_)
    {
        lockState_ = LOCK_SCRATCH;
        lockScratchData_ = graphics_->ReserveScratchBuffer(count * vertexSize_);
        return lockScratchData_;
    }
    else
        return nullptr;
}

void VertexBuffer::Unlock()
{
    switch (lockState_)
    {
    case LOCK_HARDWARE:
        UnmapBuffer();
        break;

    case LOCK_SHADOW:
        SetDataRange(shadowData_.Get() + lockStart_ * vertexSize_, lockStart_, lockCount_);
        lockState_ = LOCK_NONE;
        break;

    case LOCK_SCRATCH:
        SetDataRange(lockScratchData_, lockStart_, lockCount_);
        if (graphics_)
            graphics_->FreeScratchBuffer(lockScratchData_);
        lockScratchData_ = nullptr;
        lockState_ = LOCK_NONE;
        break;

    default:
        break;
    }
}

bool VertexBuffer::Create()
{
    Release();

    if (!vertexCount_ || !elementMask_)
        return true;

    if (graphics_)
    {
        BufferDesc bufferDesc;

        bufferDesc.BindFlags = BIND_VERTEX_BUFFER;
        bufferDesc.CPUAccessFlags = dynamic_ ? CPU_ACCESS_WRITE : CPU_ACCESS_NONE;
        bufferDesc.Usage = dynamic_ ? USAGE_DYNAMIC : USAGE_DEFAULT;
        bufferDesc.Size = (UINT)(vertexCount_ * vertexSize_);

        graphics_->GetImpl()->GetDevice()->CreateBuffer(bufferDesc, nullptr, (IBuffer**)&object_.ptr_);
        if (object_.ptr_ == nullptr)
        {
            URHO3D_LOGERROR("Failed to create vertex buffer");
            return false;
        }
    }

    return true;
}

bool VertexBuffer::UpdateToGPU()
{
#if 0
    if (object_.ptr_ && shadowData_)
        return SetData(shadowData_.Get());
    else
        return false;
#else
    NOT_IMPLEMENTED();

    return true;
#endif
}

void* VertexBuffer::MapBuffer(unsigned start, unsigned count, bool discard)
{
    void* hwData = nullptr;

    if (object_.ptr_)
    {
        PVoid mappedData = nullptr;

        graphics_->GetImpl()->GetDeviceContext()->MapBuffer(
            (IBuffer*)object_.ptr_, MAP_WRITE, discard ? MAP_FLAG_DISCARD : MAP_FLAG_NONE, mappedData);
        if (mappedData == nullptr)
            URHO3D_LOGERROR("Failed to map vertex buffer");
        else
        {
            hwData = mappedData;
            lockState_ = LOCK_HARDWARE;
        }
    }

    return hwData;
}

void VertexBuffer::UnmapBuffer()
{
    if (object_.ptr_ && lockState_ == LOCK_HARDWARE)
    {
        graphics_->GetImpl()->GetDeviceContext()->UnmapBuffer((IBuffer*)object_.ptr_, MAP_WRITE);
        lockState_ = LOCK_NONE;
    }
}

}
