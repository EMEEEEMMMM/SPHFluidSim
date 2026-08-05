#pragma once
#include <string>
#include <glad/glad.h>

class Shader {
public:
    Shader(const std::string& vertexPath, const std::string& fragmentPath);
    ~Shader();
    
    uint32_t m_RendererID;
    
    void Bind() const;
    void Unbind() const;

private:
    std::string ParseShader(const std::string& filepath);
    uint32_t CompileShader(unsigned int type, const std::string& source);
    uint32_t CreateShader(const std::string& vertexShader, const std::string& fragmentShader);
};