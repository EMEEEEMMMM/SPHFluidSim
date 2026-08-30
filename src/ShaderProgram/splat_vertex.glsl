#version 330 core
// Per-splat instanced quad (4 vertices per splat, GL_TRIANGLE_STRIP).
// Attributes (per instance, divisor 1):
//   location 4: aCenter   (vec3)  splat center (world)
//   location 5: aCovDiag  (vec3)  symmetric covariance diagonal  (xx, yy, zz)
//   location 6: aCovOff   (vec3)  symmetric covariance off-diag  (xy, xz, yz)
//   location 7: aColor    (vec3)  render color
//   location 8: aAlpha    (float) opacity
layout(location = 4) in vec3 aCenter;
layout(location = 5) in vec3 aCovDiag;
layout(location = 6) in vec3 aCovOff;
layout(location = 7) in vec3 aColor;
layout(location = 8) in float aAlpha;

out vec2 vCenterPx;   // ellipse centre in pixels (gl_FragCoord space)
out vec3 vCovInv;     // inverse 2x2 screen covariance (a, b, c) for [a b; b c]
out vec3 vColor;
out float vAlpha;

uniform mat4 view;
uniform float fovY;      // vertical FOV in radians
uniform vec2 viewport;   // framebuffer size in pixels

void main() {
    vec4 cam = view * vec4(aCenter, 1.0);
    float zc = -cam.z;                       // depth in front of the camera
    if (zc < 0.05) {                         // behind / too close: cull this quad
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        vCenterPx = vec2(0.0);
        vCovInv = vec3(0.0);
        vColor = aColor;
        vAlpha = 0.0;
        return;
    }

    // pinhole projection (theory.md Sec. 15): focal length in pixels
    float f = viewport.y * 0.5 / tan(fovY * 0.5);
    float px = f * cam.x / zc + viewport.x * 0.5;
    float py = viewport.y * 0.5 - f * cam.y / zc;   // flip: gl_FragCoord y up

    // camera-space covariance: Sigma_c = R Sigma R^T, R = mat3(view) (Sec. 14)
    mat3 Cov = mat3(aCovDiag.x, aCovOff.x, aCovOff.y,
                    aCovOff.x, aCovDiag.y, aCovOff.z,
                    aCovOff.y, aCovOff.z, aCovDiag.z);
    mat3 R = mat3(view);
    mat3 CovCam = R * Cov * transpose(R);

    // linearized perspective projection (Sec. 15):
    //   J = f/zc * [[1, 0, -x/zc], [0, 1, -y/zc]]
    //   Sigma' = J Sigma_c J^T   (2x2, in pixels^2)
    float xz = cam.x / zc, yz = cam.y / zc;
    float s = f / zc;
    float c00 = CovCam[0][0], c01 = CovCam[0][1], c02 = CovCam[0][2];
    float c11 = CovCam[1][1], c12 = CovCam[1][2], c22 = CovCam[2][2];
    float a = s * s * (c00 - 2.0 * xz * c02 + xz * xz * c22);
    float b = s * s * (c01 - xz * c12 - yz * c02 + xz * yz * c22);
    float c = s * s * (c11 - 2.0 * yz * c12 + yz * yz * c22);

    // 2x2 eigendecomposition -> ellipse axes (3 sigma bounding box)
    float tr = a + c;
    float det = a * c - b * b;
    float disc = sqrt(max(tr * tr * 0.25 - det, 0.0));
    float l1 = max(tr * 0.5 + disc, 0.05);
    float l2 = max(tr * 0.5 - disc, 0.05);
    vec2 e1;
    if (abs(b) > 1e-6) {
        e1 = normalize(vec2(b, l1 - a));
    } else {
        e1 = vec2(1.0, 0.0);
    }
    vec2 e2 = vec2(-e1.y, e1.x);
    float r1 = 3.0 * sqrt(l1);
    float r2 = 3.0 * sqrt(l2);

    int corner = gl_VertexID & 3;            // 4 quad corners
    float sx = (corner & 1) == 0 ? -1.0 : 1.0;
    float sy = (corner & 2) == 0 ? -1.0 : 1.0;
    vec2 p = vec2(px, py) + sx * r1 * e1 + sy * r2 * e2;

    // to NDC (depth irrelevant: sorted alpha blending, depth test disabled)
    vec2 ndc = vec2(p.x / viewport.x * 2.0 - 1.0, 1.0 - p.y / viewport.y * 2.0);
    gl_Position = vec4(ndc, 0.5, 1.0);

    // pass the inverse 2x2 covariance (Sec. 16): inv = 1/det [[c, -b], [-b, a]]
    // Clamp invDet: very elongated ellipses (thin sheets/splashes) have det~0
    // and invDet -> inf, which makes every off-centre pixel discard. A floor
    // of 1e-4 px^4 keeps them visible (the 3-sigma quad already bounds them).
    float invDet = 1.0 / max(det, 1e-4);
    vCovInv = vec3(c * invDet, -b * invDet, a * invDet);
    // gl_FragCoord origin is bottom-left (NDC y up); our py is top-down, so
    // flip it here — otherwise the Mahalanobis distance in the fragment
    // shader is wrong everywhere except at the screen-centre row.
    vCenterPx = vec2(px, viewport.y - py);
    vColor = aColor;
    vAlpha = aAlpha;
}
