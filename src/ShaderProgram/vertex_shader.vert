#version 330 core
layout (location = 2) in vec3 aPosInst;   // per-instance particle position
layout (location = 3) in vec3 aColorInst; // per-instance particle color

out vec3 vColor;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float particleScale;

void main(){
    vec4 world = model * vec4(aPosInst, 1.0);
    gl_Position = projection * view * world;
    gl_PointSize = particleScale / gl_Position.w * 800.0; // radius in screen px
    vColor = aColorInst;
}
