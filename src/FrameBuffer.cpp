#include<glad/glad.h>
#include<iostream>
#include"FrameBuffer.hpp"
 
FrameBuffer::FrameBuffer(uint32_t width, uint32_t height):m_Width(width),m_Height(height),
m_FrameBufferID(0), m_ColorAttachment(0), m_DepthAttachment(0) {
	Invalidate();
}
 
FrameBuffer::~FrameBuffer() {
	if (m_FrameBufferID != 0) 
	{
		glDeleteFramebuffers(1, &m_FrameBufferID);
		m_FrameBufferID = 0;
	}
	if (m_ColorAttachment != 0) 
	{
		glDeleteTextures(1, &m_ColorAttachment);
		m_ColorAttachment = 0;
	}
	if (m_DepthAttachment != 0) 
	{
		glDeleteTextures(1, &m_DepthAttachment);
		m_DepthAttachment = 0;
	}
}
 
void FrameBuffer::Invalidate(){
	if (m_FrameBufferID != 0) 
	{
		glDeleteFramebuffers(1, &m_FrameBufferID);
		glDeleteTextures(1, &m_ColorAttachment);
		glDeleteTextures(1, &m_DepthAttachment);
	}
	glGenFramebuffers(1, &m_FrameBufferID);
	glBindFramebuffer(GL_FRAMEBUFFER, m_FrameBufferID);

	glGenTextures(1, &m_ColorAttachment);
	glBindTexture(GL_TEXTURE_2D, m_ColorAttachment);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_Width, m_Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ColorAttachment, 0);

	glGenTextures(1, &m_DepthAttachment);
	glBindTexture(GL_TEXTURE_2D, m_DepthAttachment);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, m_Width, m_Height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_DepthAttachment, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		std::cout << "帧缓存区编译失败" << std::endl;
 
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}
 
void FrameBuffer::Resize(uint32_t width, uint32_t height){
	m_Width = width;
	m_Height = height;
 
	Invalidate();
}
 
void FrameBuffer::Bind(){
	glBindFramebuffer(GL_FRAMEBUFFER, m_FrameBufferID);
}
 
void FrameBuffer::UBind() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}