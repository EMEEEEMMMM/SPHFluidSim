#version 330 core
in vec2 vCenterPx;
in vec3 vCovInv;
in vec3 vColor;
in float vAlpha;
out vec4 FragColor;

void main() {
    vec2 d = gl_FragCoord.xy - vCenterPx;
    float m2 = d.x * d.x * vCovInv.x + 2.0 * d.x * d.y * vCovInv.y + d.y * d.y * vCovInv.z;
    // Gaussian falloff (demo_v2 style). Moderate steepness keeps the surface
    // smooth; combined with the flat colour+highlight shading this reads as
    // clean water rather than haze.
    float w = exp(-m2);
    float a = vAlpha * w;
    if (a < 0.001) discard;
    FragColor = vec4(vColor * a, a);
}
