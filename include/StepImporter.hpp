#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal, dependency-free ISO 10303-21 (STEP) importer.
//
// The importer reads the exchange structure, resolves the B-rep topology and
// tessellates the faces it understands:
//   * faceted B-rep (POLY_LOOP) and polygonal shells,
//   * faces on analytic surfaces: plane, cylinder, cone, sphere, torus,
//   * boundary curves: line, circle, ellipse, polyline, composite and
//     (rational) B-spline curves, trimmed curves.
//
// Faces on B-spline / swept surfaces are reported as skipped instead of
// aborting the whole import, so a part with a few fillets still loads.
namespace Step {

struct Mesh {
    // Flat-shaded triangle soup: every triangle owns three vertices, so hard
    // CAD edges stay crisp. Positions are converted to the engine's Y-up
    // world; normals match that frame.
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    // CAD boundary edges as an xyz line list: exact face-boundary curves, not
    // tessellation edges. Lets the renderer draw the circles/corners that
    // SolidWorks shows even where the surface is closed by coincident faces.
    std::vector<float> edges;

    int faces = 0;         // faces tessellated
    int skippedFaces = 0;  // faces with an unsupported surface/loop
    double unitScale = 1.0;  // STEP length unit -> metres
    std::string warning;     // non-fatal note (skipped faces, ignored units)
};

// Returns false with a human-readable `error` when the file cannot be parsed
// or contains no usable geometry.
bool Tessellate(const std::string& text, Mesh& mesh, std::string& error);

}  // namespace Step
