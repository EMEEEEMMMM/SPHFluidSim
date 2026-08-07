#version 330 core
in vec3 vNormal;
in vec3 vWorldPos;
out vec4 FragColor;

uniform vec3 viewPos;

void main(){
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;               // double-sided
    vec3 V = normalize(viewPos - vWorldPos);

    // --- Fresnel (Schlick-like): grazing angles reflect the sky more ---
    float fres = pow(1.0 - max(dot(N, V), 0.0), 4.0);

    // --- Reflected environment: procedural sky (horizon-white -> zenith blue) ---
    vec3 R = reflect(-V, N);
    float skyMix = clamp(R.y * 0.6 + 0.5, 0.0, 1.0);
    vec3 sky = mix(vec3(0.72, 0.80, 0.90), vec3(0.35, 0.55, 0.85), skyMix);
    // sun glint in the reflection (fixed key-light direction)
    vec3 L = normalize(vec3(0.45, 0.75, 0.4));
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 96.0);
    sky += vec3(1.0, 0.98, 0.92) * spec * 1.1;

    // --- Transmitted water colour (opaque approximation of the volume) ---
    float diff = max(dot(N, L), 0.0);
    vec3 water = vec3(0.015, 0.10, 0.20);
    water += vec3(0.06, 0.28, 0.45) * (0.25 + 0.75 * diff);

    vec3 col = mix(water, sky, clamp(fres, 0.0, 0.95));
    // slight white rim where the surface is seen head-on against the light
    col += vec3(0.5, 0.65, 0.8) * pow(max(dot(N, V), 0.0), 8.0) * 0.08;

    FragColor = vec4(col, 1.0);                // OPAQUE (no blend artifacts)
}
