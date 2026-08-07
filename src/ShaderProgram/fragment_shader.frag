#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main(){
    // turn the square point into a circle
    vec2 c = gl_PointCoord - vec2(0.5);
    if (dot(c, c) > 0.25) discard;
    FragColor = vec4(vColor, 1.0);
}
