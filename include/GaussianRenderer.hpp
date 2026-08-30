#pragma once
#include "Math/Vector3.hpp"
#include <vector>

// ---------------------------------------------------------------------------
// GaussianRenderer â€?per-particle 3D Gaussian splatting surface rendering,
// exactly as derived in docs/theory.md (replaces the Marching-Cubes path):
//
//   color field C_i  ->  normal n_i (= r3)
//   velocity gradient -> strain-rate D_i -> eigendecomposition -> t_max (= r1)
//   Gram-Schmidt      -> rotation R = [r1 r2 r3]
//   curvature kappa_i -> squeeze scale s3, strain -> s1, volume -> s2
//   covariance Sigma_i = R diag(s)^2 R^T
//   render color c_i  (speed foam + thickness + Blinn-Phong) and opacity alpha
//
// Only surface particles (C_i below a threshold) are emitted as splats; the
// rasterizer (OpenGLLayer) sorts them far-to-near and alpha-composites.
// ---------------------------------------------------------------------------

struct Splat {
    Math::Vector3 center;   // particle position (world)
    float cov[6];           // symmetric 3x3 covariance: xx, yy, zz, xy, xz, yz
    float color[3];         // render color RGB (diffuse + specular), ~[0,1]
    float alpha;            // effective opacity (theory.md Sec. 12)
};

class GaussianRenderer {
public:
    struct Params {
        // Sec. 7.2 stretch: s1 = r (1 + alpha*max(0,lambda1)*dt), capped at rMax*r
        // Weak amplification keeps stretched sheets from reading as hard-edged
        // rectangular shards; splash droplets are handled by isoNbThreshold.
        float stretchAlpha   = 1.2f;
        float rMaxFactor     = 2.0f;
        // Sec. 7.3 curvature-adaptive flattening: eps = clamp(epsMin + kappa/kappaMax * (epsMax-epsMin))
        // epsMin must be large enough that the pancake keeps a visible screen
        // footprint at grazing angles: with epsMin=0.05-0.15 the splat is a
        // 1px line when viewed obliquely (side walls vanish, top surface shows
        // stripe gaps between particle rows). epsMin=0.4 keeps ~3px across.
        float epsMin         = 0.40f;
        float epsMax         = 0.80f;
        float kappaMaxFactor = 2.0f;      // kappaMax = factor / r  (curvature of the smallest droplet)
        // Sec. 7.3: minimum squeeze thickness. Without a floor, edge/corner
        // splats (normal along the view direction) project as hair-thin lines
        // and the cube's edges read as dark empty seams between bright faces.
        float s3MinFactor    = 0.45f;     // s3 = max(r*eps, s3MinFactor*r)
        // Overall splat size multiplier. Surface particles are spaced one
        // particle apart; scaling the ellipsoid up overlaps the neighbours so
        // the surface reads continuous (no per-particle speckle). Opacity is
        // computed from the UNSCALED volume, so alpha stays ~alphaBase.
        float splatScale     = 1.0f;
        // Sec. 7.4 volume-conservation cap: s2 = r^3/(s1*s3) explodes when the
        // pancake is very flat (epsMin=0.05 -> s2 = 20r -> a disc bigger than
        // the tank). Cap it so the screen footprint stays local, and floor it
        // high enough that stretched splats stay plump instead of needle-thin.
        float s2MaxFactor    = 2.5f;
        float s2MinFactor    = 0.35f;
        // Sec. 7.5 isotropic fallback band (C_i in [isoBlendLo, isoBlendHi])
        float isoBlendLo     = 0.85f;
        float isoBlendHi     = 0.95f;
        // Sec. 4.3 surface test: particles with C_i < surfaceThreshold are
        // surface splats. With a finite-resolution SPH kernel the outermost
        // layer measures C ~ 0.93 (not the continuum 0.5), so the threshold
        // must sit between that and the interior ~1.0 â€?0.95 keeps the whole
        // outermost layer (top face, walls) on the bright surface pass.
        float surfaceThreshold = 0.95f;
        // Sec. 7.5: particles with very few neighbours (splashes, cube corners)
        // fall back to isotropic spheres â€?otherwise the strain-rate stretch
        // produces elongated rectangular shards instead of round droplets.
        int   isoNbThreshold = 12;
        // Sec. 12 opacity calibration: high enough that the surface reads
        // solid (like the old opaque MC surface) instead of a haze of stacked
        // translucent shells
        float alphaBase      = 0.95f;
        // Sec. 7.5 interior fill: isotropic spheres with low opacity so the
        // water body reads as a volume, not an empty shell
        float interiorAlpha   = 0.25f;
        // Sec. 10 diffuse colours: a clean water blue (user request: flat
        // colour + highlight, no reflection tricks)
        Math::Vector3 deepColor    = {0.05f, 0.32f, 0.62f};
        Math::Vector3 shallowColor = {0.40f, 0.65f, 0.95f};
        // visual enhancement: ambient fill + fresnel rim so the surface reads
        // as glossy water instead of flat dark blue
        float ambient = 0.15f;              // ambient = deepColor * ambient
        float fresnelRim = 0.20f;           // rim = (1,0.97,0.92) * pow(1-|n.v|, 3) * fresnelRim
        // fresnel sky reflection: grazing angles blend toward a sky tint.
        // Keep it moderate and blue-ish ¡ª too strong/white washes the whole
        // surface toward white (the "haze" the user saw).
        float fresnelReflect = 0.50f;
        Math::Vector3 skyColor = {0.30f, 0.55f, 0.95f};
        // Sec. 10 speed->foam normalization: only fast splashes should foam.
        // vmax=1.5 made every dam-break wave (1-2 m/s) pure white; 4.0 keeps
        // the body blue and only the fastest droplets whitish.
        float foamVMax       = 4.0f;
        // Sec. 11 specular: moderate strength and a high exponent so the
        // highlight stays a compact glint â€?a strong broad specular turns
        // surface-normal noise into bright white stripes on flat water
        float specGamma      = 96.0f;
        float specStrength   = 1.0f;   // multiplier on the Blinn-Phong term
        // key light direction (world), matches the old surface shader
        Math::Vector3 lightDir = {0.45f, 0.75f, 0.40f};
    };

    GaussianRenderer() = default;

    // Compute per-particle Gaussian parameters from the solver state.
    //   positions/velocities: fluid particles only (no boundary particles)
    //   densities: NORMALIZED rho/rho0 (particle volume V_j = spacing^3 / rhoN_j)
    //   dt: simulation timestep (used by the Sec. 7.2 strain-rate stretch)
    //   cameraPos: used for the Blinn-Phong view vector (Sec. 11)
    void Compute(const std::vector<Math::Vector3>& positions,
                 const std::vector<Math::Vector3>& velocities,
                 const std::vector<float>& densities,
                 float spacing, float dt,
                 const Math::Vector3& cameraPos);

    const std::vector<Splat>& Splats() const { return m_splats; }
    int  SplatCount() const { return (int)m_splats.size(); }
    int  SurfaceCount() const { return (int)m_surface.size(); }  // surface particles found
    // color field C_i per particle (volume occupancy, theory.md Sec. 4.1)
    const std::vector<float>& ColorField() const { return m_color; }

    Params& GetParams() { return m_params; }
    const Params& GetParams() const { return m_params; }

    // Debug stats
    float AvgColorField() const { return m_avgC; }
    int   MaxNeighbors() const { return m_maxNb; }

private:
    void BuildGrid(const std::vector<Math::Vector3>& X, float h);
    void ComputeColorField(const std::vector<Math::Vector3>& X,
                           const std::vector<float>& rhoN, float spacing);
    void ComputeSurfaceAniso(const std::vector<Math::Vector3>& X,
                             const std::vector<Math::Vector3>& V,
                             const std::vector<float>& rhoN,
                             float spacing, float dt);
    void BuildSplats(const std::vector<Math::Vector3>& X,
                     const std::vector<Math::Vector3>& V,
                     float spacing, float dt, const Math::Vector3& cameraPos);

    // Poly6 kernel (theory.md Appendix B) and its gradient
    static float WPoly6(float r, float h);
    static Math::Vector3 gradWPoly6(const Math::Vector3& rvec, float h);
    // Jacobi eigendecomposition of a symmetric 3x3 (row-major a[9]); returns
    // eigenvalues sorted descending in eval[3] and eigenvectors as the columns
    // of evec[9] (evec[3k+c] = component k of eigenvector c).
    static void SymEigen3(const float a[9], float eval[3], float evec[9]);

    Params m_params;
    float m_spacing = 0.04f;
    float m_h = 0.08f;                 // kernel support = 2 * spacing

    // uniform grid (flat SoA buckets, cell = h)
    int m_gw = 0, m_gh = 0, m_gd = 0;
    Math::Vector3 m_origin;
    std::vector<int> m_cellStart;
    std::vector<int> m_cellParticles;

    // per-particle intermediates
    std::vector<float> m_color;        // color field C_i (volume occupancy)
    std::vector<int>   m_nbCount;      // neighbours within h, per particle
    std::vector<Math::Vector3> m_normal;  // outward surface normal (surface particles)
    std::vector<Math::Vector3> m_tmax;    // principal stretch direction (surface particles)
    std::vector<float> m_lambda1;      // largest strain-rate eigenvalue
    std::vector<float> m_kappa;        // curvature measure div n
    std::vector<int> m_surface;        // indices of surface particles

    // output
    std::vector<Splat> m_splats;
    float m_avgC = 0.0f;
    int   m_maxNb = 0;
};



