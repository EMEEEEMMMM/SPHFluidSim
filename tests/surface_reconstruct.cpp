// headless check of SurfaceReconstructor at dam-break scale.
// Optional argv[1] = grid scale (1.0 = full quality, >1 = faster/coarser).
#include "FluidSolver.hpp"
#include "SurfaceReconstructor.hpp"
#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    FluidSolver s;
    s.Init(2.0f, 1.2f, 1.0f, 0.8f);
    for (int k = 0; k < 50; k++) s.Step();   // let it start collapsing

    SurfaceReconstructor rec;
    if (argc > 1) rec.SetGridScale((float)atof(argv[1]));
    auto t0 = std::chrono::steady_clock::now();
    bool ok = rec.Reconstruct(s.Positions(), s.Spacing());
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("particles=%d  ok=%d  verts=%d  tris=%d  time=%.1f ms\n",
           s.FluidCount(), ok, rec.VertexCount(),
           ok ? rec.IndexCount() / 3 : 0, ms);

    // normal sanity: surface normals should point OUTWARD (away from the fluid
    // center of mass): n . (p - cm) > 0 for most vertices
    int outward = 0, total = 0;
    if (ok) {
        Math::Vector3 cm;
        for (const auto& p : s.Positions()) cm = cm + p;
        cm = cm * (1.0f / (float)s.Positions().size());
        const auto& v = rec.Vertices();
        for (size_t i = 0; i + 5 < v.size(); i += 6) {
            Math::Vector3 p(v[i], v[i+1], v[i+2]);
            Math::Vector3 n(v[i+3], v[i+4], v[i+5]);
            if ((p - cm).dot(n) > 0.0f) outward++;
            total++;
        }
    }
    printf("normals outward: %d/%d (%.1f%%)\n", outward, total,
           total ? 100.0f * outward / total : 0.0f);

    bool pass = ok && rec.IndexCount() > 1000 && ms < 2000.0 && outward > total / 2;
    printf(pass ? "PASS\n" : "FAIL\n");
    return pass ? 0 : 1;
}
