"""Emit small AP214 STEP fixtures used by tests/test_model_import.cpp.

Geometry is kept analytic on purpose: planar faces bounded by lines for the
box, a cylindrical face plus circular caps for the cylinder, and a plate whose
planar faces carry an inner circular hole.  All lengths are millimetres.
"""
import math
import os

OUT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(OUT)


class Step:
    def __init__(self):
        self.records = []

    def add(self, text):
        self.records.append(text)
        return len(self.records)

    # -- primitives ---------------------------------------------------------
    def point(self, p):
        return self.add("CARTESIAN_POINT('',(%.6f,%.6f,%.6f));" % tuple(p))

    def direction(self, v):
        return self.add("DIRECTION('',(%.6f,%.6f,%.6f));" % tuple(v))

    def placement(self, origin, axis, ref):
        return self.add("AXIS2_PLACEMENT_3D('',#%d,#%d,#%d);" % (
            self.point(origin), self.direction(axis), self.direction(ref)))

    def vertex(self, point_id):
        return self.add("VERTEX_POINT('',#%d);" % point_id)

    def line_edge(self, v_from, v_to, p_from, p_to):
        d = [p_to[i] - p_from[i] for i in range(3)]
        length = math.sqrt(sum(c * c for c in d))
        vec = self.add("VECTOR('',#%d,%.6f);" % (self.direction(d), length))
        line = self.add("LINE('',#%d,#%d);" % (self.point(p_from), vec))
        return self.add("EDGE_CURVE('',#%d,#%d,#%d,.T.);" % (v_from, v_to, line))

    def circle_edge(self, vertex_id, center, radius):
        place = self.placement(center, (0, 0, 1), (1, 0, 0))
        circle = self.add("CIRCLE('',#%d,%.6f);" % (place, radius))
        return self.add("EDGE_CURVE('',#%d,#%d,#%d,.T.);" % (vertex_id, vertex_id, circle))

    def loop(self, oriented_edges):
        return self.add("EDGE_LOOP('',(%s));" % ",".join("#%d" % e for e in oriented_edges))

    def oriented(self, edge_id, forward=True):
        return self.add("ORIENTED_EDGE('',*,*,#%d,%s);" % (edge_id, ".T." if forward else ".F."))

    def plane_face(self, loop_id, origin, normal, ref, same_sense=True):
        plane = self.add("PLANE('',#%d);" % self.placement(origin, normal, ref))
        bound = self.add("FACE_OUTER_BOUND('',#%d,.T.);" % loop_id)
        return self.add("ADVANCED_FACE('',(#%d),#%d,%s);" % (
            bound, plane, ".T." if same_sense else ".F."))

    def face_with_hole(self, outer_loop, inner_loop, origin, normal, ref):
        plane = self.add("PLANE('',#%d);" % self.placement(origin, normal, ref))
        b1 = self.add("FACE_OUTER_BOUND('',#%d,.T.);" % outer_loop)
        b2 = self.add("FACE_BOUND('',#%d,.T.);" % inner_loop)
        return self.add("ADVANCED_FACE('',(#%d,#%d),#%d,.T.);" % (b1, b2, plane))

    def cylinder_face(self, loop_id, origin, axis, ref, radius, same_sense=True):
        surface = self.add("CYLINDRICAL_SURFACE('',#%d,%.6f);" % (
            self.placement(origin, axis, ref), radius))
        bound = self.add("FACE_OUTER_BOUND('',#%d,.T.);" % loop_id)
        return self.add("ADVANCED_FACE('',(#%d),#%d,%s);" % (
            bound, surface, ".T." if same_sense else ".F."))

    # -- document -----------------------------------------------------------
    def write(self, path, name, solid_id):
        length_unit = ("(LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.))")
        angle_unit = "(NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.))"
        solid_angle = "(NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT())"
        u1 = self.add(length_unit + ";")
        u2 = self.add(angle_unit + ";")
        u3 = self.add(solid_angle + ";")
        unc = self.add("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-07),#%d,"
                       "'distance_accuracy_value','');" % u1)
        ctx = self.add("(GEOMETRIC_REPRESENTATION_CONTEXT(3) "
                       "GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((#%d)) "
                       "GLOBAL_UNIT_ASSIGNED_CONTEXT((#%d,#%d,#%d)) "
                       "REPRESENTATION_CONTEXT('',''));" % (unc, u1, u2, u3))
        app = self.add("APPLICATION_CONTEXT('core data for automotive mechanical design processes');")
        self.add("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',"
                 "2000,#%d);" % app)
        product_context = self.add("PRODUCT_CONTEXT('',#%d,'mechanical');" % app)
        product = self.add("PRODUCT('%s','%s','',(#%d));" % (name, name, product_context))
        formation = self.add("PRODUCT_DEFINITION_FORMATION('','',#%d);" % product)
        definition_context = self.add("PRODUCT_DEFINITION_CONTEXT('part definition',#%d,'design');" % app)
        definition = self.add("PRODUCT_DEFINITION('design','',#%d,#%d);" % (
            formation, definition_context))
        representation = self.add(
            "ADVANCED_BREP_SHAPE_REPRESENTATION('%s',(#%d),#%d);" % (name, solid_id, ctx))
        self.add("SHAPE_DEFINITION_REPRESENTATION(#%d,#%d);" % (definition, representation))

        header = (
            "ISO-10303-21;\n"
            "HEADER;\n"
            "FILE_DESCRIPTION((''),'2;1');\n"
            "FILE_NAME('%s.step','2026-01-01T00:00:00',(''),(''),'','','');\n"
            "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\n"
            "ENDSEC;\n"
            "DATA;\n" % name.lower())
        body = "".join("#%d = %s\n" % (i + 1, r) for i, r in enumerate(self.records))
        with open(path, "w", encoding="ascii", newline="\n") as handle:
            handle.write(header + body + "ENDSEC;\nEND-ISO-10303-21;\n")


def box():
    s = Step()
    pts = [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0),
           (0, 0, 10), (10, 0, 10), (10, 10, 10), (0, 10, 10)]
    pids = [s.point(p) for p in pts]
    vids = [s.vertex(p) for p in pids]

    pairs = [(0, 1), (1, 2), (2, 3), (3, 0),
             (4, 5), (5, 6), (6, 7), (7, 4),
             (0, 4), (1, 5), (2, 6), (3, 7)]
    edges = {}
    for a, b in pairs:
        edges[(a, b)] = s.line_edge(vids[a], vids[b], pts[a], pts[b])
        edges[(b, a)] = (edges[(a, b)], False)

    def ring_loop(ring):
        oriented = []
        for i in range(len(ring)):
            a, b = ring[i], ring[(i + 1) % len(ring)]
            entry = edges[(a, b)]
            if isinstance(entry, tuple):
                eid, fwd = entry
            else:
                eid, fwd = entry, True
            oriented.append(s.oriented(eid, fwd))
        return s.loop(oriented)

    faces = []
    faces.append(s.plane_face(ring_loop([0, 1, 2, 3]), (0, 0, 0), (0, 0, -1), (1, 0, 0)))
    faces.append(s.plane_face(ring_loop([4, 5, 6, 7]), (0, 0, 10), (0, 0, 1), (1, 0, 0)))
    faces.append(s.plane_face(ring_loop([0, 1, 5, 4]), (0, 0, 0), (0, -1, 0), (1, 0, 0)))
    faces.append(s.plane_face(ring_loop([1, 2, 6, 5]), (10, 0, 0), (1, 0, 0), (0, 1, 0)))
    faces.append(s.plane_face(ring_loop([2, 3, 7, 6]), (0, 10, 0), (0, 1, 0), (1, 0, 0)))
    faces.append(s.plane_face(ring_loop([3, 0, 4, 7]), (0, 0, 0), (-1, 0, 0), (0, 1, 0)))

    shell = s.add("CLOSED_SHELL('',(%s));" % ",".join("#%d" % f for f in faces))
    solid = s.add("MANIFOLD_SOLID_BREP('Box',#%d);" % shell)
    s.write(os.path.join(OUT, "box.step"), "Box", solid)


def cylinder():
    s = Step()
    radius, height = 5.0, 10.0
    bottom = s.point((radius, 0, 0))
    top = s.point((radius, 0, height))
    v_bottom = s.vertex(bottom)
    v_top = s.vertex(top)

    bottom_circle = s.circle_edge(v_bottom, (0, 0, 0), radius)
    top_circle = s.circle_edge(v_top, (0, 0, height), radius)
    seam = s.line_edge(v_bottom, v_top, (radius, 0, 0), (radius, 0, height))

    bottom_loop = s.loop([s.oriented(bottom_circle, True)])
    bottom_face = s.plane_face(bottom_loop, (0, 0, 0), (0, 0, -1), (1, 0, 0))

    top_loop = s.loop([s.oriented(top_circle, True)])
    top_face = s.plane_face(top_loop, (0, 0, height), (0, 0, 1), (1, 0, 0))

    side_loop = s.loop([
        s.oriented(bottom_circle, True),
        s.oriented(seam, True),
        s.oriented(top_circle, False),
        s.oriented(seam, False),
    ])
    side_face = s.cylinder_face(side_loop, (0, 0, 0), (0, 0, 1), (1, 0, 0), radius)

    shell = s.add("CLOSED_SHELL('',(#%d,#%d,#%d));" % (bottom_face, top_face, side_face))
    solid = s.add("MANIFOLD_SOLID_BREP('Cylinder',#%d);" % shell)
    s.write(os.path.join(OUT, "cylinder.step"), "Cylinder", solid)


def plate():
    s = Step()
    size, thickness, hole = 20.0, 2.0, 4.0
    pts = [(0, 0, 0), (size, 0, 0), (size, size, 0), (0, size, 0),
           (0, 0, thickness), (size, 0, thickness), (size, size, thickness), (0, size, thickness)]
    pids = [s.point(p) for p in pts]
    vids = [s.vertex(p) for p in pids]

    pairs = [(0, 1), (1, 2), (2, 3), (3, 0),
             (4, 5), (5, 6), (6, 7), (7, 4),
             (0, 4), (1, 5), (2, 6), (3, 7)]
    edges = {}
    for a, b in pairs:
        edges[(a, b)] = s.line_edge(vids[a], vids[b], pts[a], pts[b])
        edges[(b, a)] = (edges[(a, b)], False)

    def ring_loop(ring):
        oriented = []
        for i in range(len(ring)):
            a, b = ring[i], ring[(i + 1) % len(ring)]
            entry = edges[(a, b)]
            eid, fwd = entry if isinstance(entry, tuple) else (entry, True)
            oriented.append(s.oriented(eid, fwd))
        return s.loop(oriented)

    # Hole ring: a closed circle reused by both planar faces.
    hole_center_bottom = (size / 2.0, size / 2.0, 0.0)
    hole_center_top = (size / 2.0, size / 2.0, thickness)
    hole_v_bottom = s.vertex(s.point((size / 2.0 + hole, size / 2.0, 0.0)))
    hole_v_top = s.vertex(s.point((size / 2.0 + hole, size / 2.0, thickness)))
    hole_bottom = s.circle_edge(hole_v_bottom, hole_center_bottom, hole)
    hole_top = s.circle_edge(hole_v_top, hole_center_top, hole)

    hole_loop_bottom = s.loop([s.oriented(hole_bottom, True)])
    hole_loop_top = s.loop([s.oriented(hole_top, True)])

    faces = []
    faces.append(s.face_with_hole(ring_loop([0, 1, 2, 3]), hole_loop_bottom,
                                  (0, 0, 0), (0, 0, -1), (1, 0, 0)))
    faces.append(s.face_with_hole(ring_loop([4, 5, 6, 7]), hole_loop_top,
                                  (0, 0, thickness), (0, 0, 1), (1, 0, 0)))
    faces.append(s.plane_face(ring_loop([0, 1, 5, 4]), (0, 0, 0), (0, -1, 0), (1, 0, 0)))
    faces.append(s.plane_face(ring_loop([1, 2, 6, 5]), (size, 0, 0), (1, 0, 0), (0, 1, 0)))
    faces.append(s.plane_face(ring_loop([2, 3, 7, 6]), (0, size, 0), (0, 1, 0), (1, 0, 0)))
    faces.append(s.plane_face(ring_loop([3, 0, 4, 7]), (0, 0, 0), (-1, 0, 0), (0, 1, 0)))

    # Inner cylindrical wall of the hole: outward normal points to the axis,
    # hence same_sense = .F. on the surface.
    wall_loop = s.loop([
        s.oriented(hole_bottom, False),
        s.oriented(hole_top, True),
    ])
    surface = s.add("CYLINDRICAL_SURFACE('',#%d,%.6f);" % (
        s.placement(hole_center_bottom, (0, 0, 1), (1, 0, 0)), hole))
    bound = s.add("FACE_OUTER_BOUND('',#%d,.T.);" % wall_loop)
    faces.append(s.add("ADVANCED_FACE('',(#%d),#%d,.F.);" % (bound, surface)))

    shell = s.add("CLOSED_SHELL('',(%s));" % ",".join("#%d" % f for f in faces))
    solid = s.add("MANIFOLD_SOLID_BREP('Plate',#%d);" % shell)
    s.write(os.path.join(OUT, "plate.step"), "Plate", solid)



def multi_hole_plate():
    """5 x 4 through-holes: exercises hole bridging and hole-wall tessellation."""
    s = Step()
    size_x, size_y, thickness = 100.0, 60.0, 6.0
    hole_radius, spacing_x, spacing_y = 3.0, 18.0, 13.0
    columns, rows = 5, 4

    pts = [(0, 0, 0), (size_x, 0, 0), (size_x, size_y, 0), (0, size_y, 0),
           (0, 0, thickness), (size_x, 0, thickness),
           (size_x, size_y, thickness), (0, size_y, thickness)]
    pids = [s.point(p) for p in pts]
    vids = [s.vertex(p) for p in pids]

    pairs = [(0, 1), (1, 2), (2, 3), (3, 0),
             (4, 5), (5, 6), (6, 7), (7, 4),
             (0, 4), (1, 5), (2, 6), (3, 7)]
    edges = {}
    for a, b in pairs:
        edges[(a, b)] = s.line_edge(vids[a], vids[b], pts[a], pts[b])
        edges[(b, a)] = (edges[(a, b)], False)

    def ring_loop(ring):
        oriented = []
        for i in range(len(ring)):
            a, b = ring[i], ring[(i + 1) % len(ring)]
            entry = edges[(a, b)]
            eid, fwd = entry if isinstance(entry, tuple) else (entry, True)
            oriented.append(s.oriented(eid, fwd))
        return s.loop(oriented)

    margin_x = (size_x - (columns - 1) * spacing_x) / 2.0
    margin_y = (size_y - (rows - 1) * spacing_y) / 2.0
    centers = [(margin_x + c * spacing_x, margin_y + r * spacing_y)
               for r in range(rows) for c in range(columns)]

    bottom_holes = []
    top_holes = []
    walls = []
    for (cx, cy) in centers:
        v_bottom = s.vertex(s.point((cx + hole_radius, cy, 0.0)))
        v_top = s.vertex(s.point((cx + hole_radius, cy, thickness)))
        circle_bottom = s.circle_edge(v_bottom, (cx, cy, 0.0), hole_radius)
        circle_top = s.circle_edge(v_top, (cx, cy, thickness), hole_radius)
        seam = s.line_edge(v_bottom, v_top, (cx + hole_radius, cy, 0.0),
                           (cx + hole_radius, cy, thickness))
        bottom_holes.append(s.loop([s.oriented(circle_bottom, True)]))
        top_holes.append(s.loop([s.oriented(circle_top, True)]))
        wall_loop = s.loop([
            s.oriented(circle_bottom, True),
            s.oriented(seam, True),
            s.oriented(circle_top, False),
            s.oriented(seam, False),
        ])
        # A bore wall faces the hole axis, i.e. against the surface normal.
        walls.append(s.cylinder_face(wall_loop, (cx, cy, 0.0), (0, 0, 1),
                                     (1, 0, 0), hole_radius, same_sense=False))

    def face_with_holes(outer_loop, inner_loops, origin, normal, ref):
        plane = s.add("PLANE('',#%d);" % s.placement(origin, normal, ref))
        outer_bound = s.add("FACE_OUTER_BOUND('',#%d,.T.);" % outer_loop)
        bounds = [outer_bound]
        for loop in inner_loops:
            bounds.append(s.add("FACE_BOUND('',#%d,.T.);" % loop))
        return s.add("ADVANCED_FACE('',(%s),#%d,.T.);" %
                     (",".join("#%d" % b for b in bounds), plane))

    faces = [face_with_holes(ring_loop([0, 1, 2, 3]), bottom_holes,
                             (0, 0, 0), (0, 0, -1), (1, 0, 0)),
             face_with_holes(ring_loop([4, 5, 6, 7]), top_holes,
                             (0, 0, thickness), (0, 0, 1), (1, 0, 0)),
             s.plane_face(ring_loop([0, 1, 5, 4]), (0, 0, 0), (0, -1, 0), (1, 0, 0)),
             s.plane_face(ring_loop([1, 2, 6, 5]), (size_x, 0, 0), (1, 0, 0), (0, 1, 0)),
             s.plane_face(ring_loop([2, 3, 7, 6]), (0, size_y, 0), (0, 1, 0), (1, 0, 0)),
             s.plane_face(ring_loop([3, 0, 4, 7]), (0, 0, 0), (-1, 0, 0), (0, 1, 0))]
    faces.extend(walls)
    shell = s.add("CLOSED_SHELL('',(%s));" % ",".join("#%d" % f for f in faces))
    solid = s.add("MANIFOLD_SOLID_BREP('MultiHolePlate',#%d);" % shell)
    s.write(os.path.join(OUT, "multi_hole_plate.step"), "MultiHolePlate", solid)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    box()
    cylinder()
    plate()
    multi_hole_plate()
    for name in ("box.step", "cylinder.step", "plate.step", "multi_hole_plate.step"):
        path = os.path.join(OUT, name)
        print("%-14s %6d bytes" % (name, os.path.getsize(path)))
