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

#include "../../Core/Context.h"
#include "../../Graphics/Graphics.h"
#include "../../Graphics/GraphicsImpl.h"
#include "../../Graphics/IndexBuffer.h"
#include "../../IO/Log.h"

#include "../../DebugNew.h"

using namespace Diligent;

namespace Urho3D
{

void IndexBuffer::OnDeviceLost()
{
    // No-op on Diligent
}

void IndexBuffer::OnDeviceReset()
{
    // No-op on Diligent
}

void IndexBuffer::Release()
{
    Unlock();

    if (graphics_ && graphics_->GetIndexBuffer() == this)
        graphics_->SetIndexBuffer(nullptr);

    URHO3D_SAFE_RELEASE(object_.ptr_);
}

bool IndexBuffer::SetData(const void* data)
{
    if (!data)
    {
        URHO3D_LOGERROR("Null pointer for index buffer data");
        return false;
    }

    if (!indexSize_)
    {
        URHO3D_LOGERROR("Index size not defined, can not set index buffer data");
        return false;
    }

    if (shadowData_ && data != shadowData_.Get())
        memcpy(shadowData_.Get(), data, indexCount_ * indexSize_);

    if (object_.ptr_)
    {
        if (dynamic_)
        {
            void* hwData = MapBuffer(0, indexCount_, true);
            if (hwData)
            {
                memcpy(hwData, data, indexCount_ * indexSize_);
                UnmapBuffer();
            }
            else
                return false;
        }
        else
        {
            // TODO: Make sure transition mode is correct
            graphics_->GetImpl()->GetDeviceContext()->UpdateBuffer(
                (IBuffer*)object_.ptr_, 0, indexCount_ * indexSize_,
                data, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    return true;
}

bool IndexBuffer::SetDataRange(const void* data, unsigned start, unsigned count, bool discard)
{
    if (start == 0 && count == indexCount_)
        return SetData(data);

    if (!data)
    {
        URHO3D_LOGERROR("Null pointer for index buffer data");
        return false;
    }

    if (!indexSize_)
    {
        URHO3D_LOGERROR("Index size not defined, can not set index buffer data");
        return false;
    }

    if (start + count > indexCount_)
    {
        URHO3D_LOGERROR("Illegal range for setting new index buffer data");
        return false;
    }

    if (!count)
        return true;

    if (shadowData_ && shadowData_.Get() + start * indexSize_ != data)
        memcpy(shadowData_.Get() + start * indexSize_, data, count * indexSize_);

    if (object_.ptr_)
    {
        if (dynamic_)
        {
            void* hwData = MapBuffer(start, count, discard);
            if (hwData)
            {
                memcpy(hwData, data, count * indexSize_);
                UnmapBuffer();
            }
            else
                return false;
        }
        else
        {
            graphics_->GetImpl()->GetDeviceContext()->UpdateBuffer((IBuffer*)object_.ptr_, start * indexSize_,
                                                                   count * indexSize_, data,
                                                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    return true;
}

void* IndexBuffer::Lock(unsigned start, unsigned count, bool discard)
{
    if (lockState_ != LOCK_NONE)
    {
        URHO3D_LOGERROR("Index buffer already locked");
        return nullptr;
    }

    if (!indexSize_)
    {
        URHO3D_LOGERROR("Index size not defined, can not lock index buffer");
        return nullptr;
    }

    if (start + count > indexCount_)
    {
        URHO3D_LOGERROR("Illegal range for locking index buffer");
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
        return shadowData_.Get() + start * indexSize_;
    }
    else if (graphics_)
    {
        lockState_ = LOCK_SCRATCH;
        lockScratchData_ = graphics_->ReserveScratchBuffer(count * indexSize_);
        return lockScratchData_;
    }
    else
        return nullptr;
}

void IndexBuffer::Unlock()
{
    switch (lockState_)
    {
    case LOCK_HARDWARE:
        UnmapBuffer();
        break;

    case LOCK_SHADOW:
        SetDataRange(shadowData_.Get() + lockStart_ * indexSize_, lockStart_, lockCount_);
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

bool IndexBuffer::Create()
{
    Release();

    if (!indexCount_)
        return true;

    if (graphics_)
    {
        BufferDesc bufferDesc;

        bufferDesc.BindFlags = BIND_INDEX_BUFFER;
        bufferDesc.CPUAccessFlags = dynamic_ ? CPU_ACCESS_WRITE : CPU_ACCESS_NONE;
        bufferDesc.Usage = dynamic_ ? USAGE_DYNAMIC : USAGE_DEFAULT;
        bufferDesc.Size = (UINT)(indexCount_ * indexSize_);

        graphics_->GetImpl()->GetDevice()->CreateBuffer(bufferDesc, nullptr, (IBuffer**)&object_.ptr_);
        if (object_.ptr_ == nullptr)
        {
            URHO3D_LOGERROR("Failed to create index buffer");
            return false;
        }
    }

    return true;
}

bool IndexBuffer::UpdateToGPU()
{
#if 0
    if (object_.ptr_ && shadowData_)
        return SetData(shadowData_.Get());
    else
        return false;
#else
    NOT_IMPLEMENTED();

    return false;
#endif
}

void* IndexBuffer::MapBuffer(unsigned start, unsigned count, bool discard)
{
    void* hwData = nullptr;

    if (object_.ptr_)
    {
        PVoid mappedData = nullptr;

        graphics_->GetImpl()->GetDeviceContext()->MapBuffer((IBuffer*)object_.ptr_, MAP_WRITE,
                                                            discard ? MAP_FLAG_DISCARD : MAP_FLAG_NONE, mappedData);
        if (mappedData == nullptr)
            URHO3D_LOGERROR("Failed to map index buffer");
        else
        {
            hwData = mappedData;
            lockState_ = LOCK_HARDWARE;
        }
    }

    return hwData;
}

void IndexBuffer::UnmapBuffer()
{
    if (object_.ptr_ && lockState_ == LOCK_HARDWARE)
    {
        graphics_->GetImpl()->GetDeviceContext()->UnmapBuffer((IBuffer*)object_.ptr_, MAP_WRITE);
        lockState_ = LOCK_NONE;
    }
}

}
