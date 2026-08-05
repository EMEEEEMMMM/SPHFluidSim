#include <cstdint>
#pragma once

class FrameBuffer {

public:
    FrameBuffer(uint32_t width, uint32_t height);
    ~FrameBuffer();
    void Invalidate();
    void Resize(uint32_t width, uint32_t height);
    inline uint32_t GetWidth() const { return m_Width; }
    inline uint32_t GetHeight() const { return m_Height; }
    inline uint32_t GetColorAttachment() const { return m_ColorAttachment; }

    void Bind();
    void UBind();

private:
    uint32_t m_FrameBufferID, m_ColorAttachment, m_DepthAttachment;
    uint32_t m_Width, m_Height;
};