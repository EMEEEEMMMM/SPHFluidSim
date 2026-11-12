#include "StepImporter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kEps = 1.0e-9;
constexpr int kMaxDepth = 8;

// ---------------------------------------------------------------------------
// Small vector / matrix helpers (double precision; converted on output)
// ---------------------------------------------------------------------------

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
struct Vec2 {
    double x = 0.0, y = 0.0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline bool IsFinite(const Vec3& a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
inline Vec3 Normalized(const Vec3& a, const Vec3& fallback = {0, 0, 1}) {
    const double len = Length(a);
    if (!(len > 1.0e-12) || !std::isfinite(len)) return fallback;
    return a * (1.0 / len);
}

inline double Cross2(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
inline Vec2 operator-(const Vec2& a, const Vec2& b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator+(const Vec2& a, const Vec2& b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator*(const Vec2& a, double s) { return {a.x * s, a.y * s}; }

// ---------------------------------------------------------------------------
// ISO 10303-21 exchange structure
// ---------------------------------------------------------------------------

struct Value {
    enum Kind { Nil, Ref, Number, Text, Enum, List, Typed } kind = Nil;
    int ref = 0;
    double number = 0.0;
    std::string text;          // string body, enum name or typed-name
    std::vector<Value> items;  // list elements / typed arguments
};

struct Entity {
    int id = 0;
    std::vector<std::string> types;
    std::map<std::string, std::vector<Value>> args;

    bool Has(const char* type) const { return args.find(type) != args.end(); }
    const std::vector<Value>* Get(const char* type) const {
        auto it = args.find(type);
        return it == args.end() ? nullptr : &it->second;
    }
    const std::vector<Value>* Any() const {
        if (types.empty()) return nullptr;
        return Get(types.front().c_str());
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : m_text(text) {}

    bool Parse(std::unordered_map<int, Entity>& out, std::string& error) {
        bool sawData = false;
        size_t cursor = 0;
        while (true) {
            const size_t data = FindDataSection(cursor);
            if (data == std::string::npos) break;
            sawData = true;
            cursor = data + 5;  // past "DATA;"
            if (!ParseRecords(cursor, out, error)) return false;
        }
        if (!sawData) {
            error = "Not a STEP exchange file: no DATA section was found.";
            return false;
        }
        if (out.empty()) {
            error = "The STEP file has an empty DATA section.";
            return false;
        }
        return true;
    }

private:
    const std::string& m_text;

    size_t FindDataSection(size_t from) const {
        const size_t upper = m_text.find("DATA;", from);
        const size_t lower = m_text.find("data;", from);
        if (upper == std::string::npos) return lower;
        if (lower == std::string::npos) return upper;
        return std::min(upper, lower);
    }

    void SkipTrivia(size_t& pos) const {
        const size_t n = m_text.size();
        while (pos < n) {
            const char c = m_text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                ++pos;
            } else if (c == '/' && pos + 1 < n && m_text[pos + 1] == '*') {
                const size_t end = m_text.find("*/", pos + 2);
                pos = (end == std::string::npos) ? n : end + 2;
            } else {
                break;
            }
        }
    }

    bool ParseRecords(size_t& pos, std::unordered_map<int, Entity>& out, std::string& error) const {
        const size_t n = m_text.size();
        while (true) {
            SkipTrivia(pos);
            if (pos >= n) break;
            if (m_text.compare(pos, 6, "ENDSEC") == 0) {
                pos += 6;
                return true;
            }
            if (m_text[pos] != '#') {
                // Unknown statement in the data section: skip to its semicolon.
                const size_t semi = m_text.find(';', pos);
                if (semi == std::string::npos) {
                    error = "Malformed STEP data section (missing ';').";
                    return false;
                }
                pos = semi + 1;
                continue;
            }
            if (!ParseRecord(pos, out, error)) return false;
        }
        return true;
    }

    bool ParseRecord(size_t& pos, std::unordered_map<int, Entity>& out, std::string& error) const {
        const size_t n = m_text.size();
        Entity entity;
        ++pos;  // '#'
        if (!ReadInteger(pos, entity.id)) {
            error = "Malformed STEP entity id near offset " + std::to_string(pos) + ".";
            return false;
        }
        SkipTrivia(pos);
        if (pos >= n || m_text[pos] != '=') {
            error = "Malformed STEP entity #" + std::to_string(entity.id) + " (missing '=').";
            return false;
        }
        ++pos;
        SkipTrivia(pos);

        if (pos < n && m_text[pos] == '(') {
            // Complex entity instance: (TYPE1(...) TYPE2(...) ...);
            ++pos;
            while (true) {
                SkipTrivia(pos);
                if (pos >= n) {
                    error = "Unterminated complex STEP entity #" + std::to_string(entity.id) + ".";
                    return false;
                }
                if (m_text[pos] == ')') {
                    ++pos;
                    break;
                }
                std::string type;
                if (!ReadIdentifier(pos, type)) {
                    error = "Malformed complex STEP entity #" + std::to_string(entity.id) + ".";
                    return false;
                }
                SkipTrivia(pos);
                std::vector<Value> args;
                if (pos >= n || m_text[pos] != '(') {
                    error = "Missing argument list in STEP entity #" + std::to_string(entity.id) + ".";
                    return false;
                }
                if (!ParseArguments(pos, args, error)) return false;
                entity.types.push_back(type);
                entity.args[type] = std::move(args);
                SkipTrivia(pos);
                if (pos < n && m_text[pos] == ',') ++pos;  // tolerated, not standard
            }
        } else {
            std::string type;
            if (!ReadIdentifier(pos, type)) {
                error = "Malformed STEP entity #" + std::to_string(entity.id) + ".";
                return false;
            }
            SkipTrivia(pos);
            std::vector<Value> args;
            if (pos >= n || m_text[pos] != '(') {
                error = "Missing argument list in STEP entity #" + std::to_string(entity.id) + ".";
                return false;
            }
            if (!ParseArguments(pos, args, error)) return false;
            entity.types.push_back(type);
            entity.args[type] = std::move(args);
        }

        SkipTrivia(pos);
        if (pos < n && m_text[pos] == ';') ++pos;
        out[entity.id] = std::move(entity);
        return true;
    }

    bool ParseArguments(size_t& pos, std::vector<Value>& out, std::string& error) const {
        const size_t n = m_text.size();
        ++pos;  // '('
        SkipTrivia(pos);
        if (pos < n && m_text[pos] == ')') {
            ++pos;
            return true;
        }
        while (true) {
            Value value;
            if (!ParseValue(pos, value, error)) return false;
            out.push_back(std::move(value));
            SkipTrivia(pos);
            if (pos >= n) {
                error = "Unterminated STEP argument list.";
                return false;
            }
            if (m_text[pos] == ',') {
                ++pos;
                continue;
            }
            if (m_text[pos] == ')') {
                ++pos;
                return true;
            }
            error = "Unexpected character in STEP argument list near offset " + std::to_string(pos) + ".";
            return false;
        }
    }

    bool ParseValue(size_t& pos, Value& value, std::string& error) const {
        const size_t n = m_text.size();
        SkipTrivia(pos);
        if (pos >= n) {
            error = "Unexpected end of STEP file.";
            return false;
        }
        const char c = m_text[pos];

        if (c == '#') {
            ++pos;
            value.kind = Value::Ref;
            if (!ReadInteger(pos, value.ref)) {
                error = "Malformed STEP reference near offset " + std::to_string(pos) + ".";
                return false;
            }
            return true;
        }
        if (c == '\'') {
            ++pos;
            value.kind = Value::Text;
            value.text.clear();
            while (pos < n) {
                const char ch = m_text[pos++];
                if (ch == '\'') {
                    if (pos < n && m_text[pos] == '\'') {
                        value.text.push_back('\'');
                        ++pos;
                        continue;
                    }
                    return true;
                }
                value.text.push_back(ch);
            }
            error = "Unterminated STEP string literal.";
            return false;
        }
        if (c == '.') {
            ++pos;
            value.kind = Value::Enum;
            value.text.clear();
            while (pos < n && m_text[pos] != '.') {
                value.text.push_back(static_cast<char>(std::toupper(
                    static_cast<unsigned char>(m_text[pos]))));
                ++pos;
            }
            if (pos < n) ++pos;
            return true;
        }
        if (c == '$' || c == '*') {
            ++pos;
            value.kind = Value::Nil;
            return true;
        }
        if (c == '(') {
            ++pos;
            value.kind = Value::List;
            SkipTrivia(pos);
            if (pos < n && m_text[pos] == ')') {
                ++pos;
                return true;
            }
            while (true) {
                SkipTrivia(pos);
                Value item;
                if (!ParseValue(pos, item, error)) return false;
                value.items.push_back(std::move(item));
                SkipTrivia(pos);
                if (pos >= n) {
                    error = "Unterminated STEP list literal.";
                    return false;
                }
                if (m_text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (m_text[pos] == ')') {
                    ++pos;
                    return true;
                }
                error = "Malformed STEP list near offset " + std::to_string(pos) + ".";
                return false;
            }
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string type;
            if (!ReadIdentifier(pos, type)) {
                error = "Malformed STEP typed parameter near offset " + std::to_string(pos) + ".";
                return false;
            }
            SkipTrivia(pos);
            value.kind = Value::Typed;
            value.text = type;
            if (pos < n && m_text[pos] == '(') {
                if (!ParseArguments(pos, value.items, error)) return false;
            }
            return true;
        }
        if (c == '"') {
            // Binary literal: not used by any geometry we read.
            ++pos;
            while (pos < n && m_text[pos] != '"') ++pos;
            if (pos < n) ++pos;
            value.kind = Value::Text;
            return true;
        }

        // Number (integer or real, optional exponent).
        const char* start = m_text.c_str() + pos;
        char* end = nullptr;
        const double parsed = std::strtod(start, &end);
        if (end == start) {
            error = "Unsupported STEP token near offset " + std::to_string(pos) + ".";
            return false;
        }
        pos += static_cast<size_t>(end - start);
        value.kind = Value::Number;
        value.number = parsed;
        return true;
    }

    bool ReadInteger(size_t& pos, int& out) const {
        const size_t n = m_text.size();
        while (pos < n && (m_text[pos] == ' ' || m_text[pos] == '\t')) ++pos;
        long long value = 0;
        bool any = false;
        while (pos < n && m_text[pos] >= '0' && m_text[pos] <= '9') {
            value = value * 10 + (m_text[pos] - '0');
            if (value > std::numeric_limits<int>::max()) return false;
            ++pos;
            any = true;
        }
        if (!any) return false;
        out = static_cast<int>(value);
        return true;
    }

    bool ReadIdentifier(size_t& pos, std::string& out) const {
        const size_t n = m_text.size();
        out.clear();
        while (pos < n) {
            const char c = m_text[pos];
            const bool valid = std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                               c == '-' || c == '!';
            if (!valid) break;
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            ++pos;
        }
        return !out.empty();
    }
};

// ---------------------------------------------------------------------------
// Entity access helpers
// ---------------------------------------------------------------------------

struct Doc {
    std::unordered_map<int, Entity> entities;

    const Entity* Find(int id) const {
        auto it = entities.find(id);
        return it == entities.end() ? nullptr : &it->second;
    }
    const std::vector<Value>* Args(int id, const char* type) const {
        const Entity* entity = Find(id);
        return entity ? entity->Get(type) : nullptr;
    }
    const std::vector<Value>* ArgsAny(int id) const {
        const Entity* entity = Find(id);
        return entity ? entity->Any() : nullptr;
    }
    const std::vector<Value>* ArgsOfRef(const Value& value, const char* type) const {
        if (value.kind != Value::Ref) return nullptr;
        const std::vector<Value>* args = Args(value.ref, type);
        if (args) return args;
        // Subtypes expose the same argument list under their own name; fall
        // back to the first record for complex instances.
        const Entity* entity = Find(value.ref);
        if (!entity) return nullptr;
        for (const std::string& name : entity->types) {
            if (name == type) return entity->Get(name.c_str());
        }
        return entity->Any();
    }
};

const Value* At(const std::vector<Value>* args, size_t index) {
    if (!args || index >= args->size()) return nullptr;
    return &(*args)[index];
}

double Number(const std::vector<Value>* args, size_t index, double fallback = 0.0) {
    const Value* value = At(args, index);
    if (!value) return fallback;
    if (value->kind == Value::Number) return value->number;
    if (value->kind == Value::Typed && !value->items.empty()) {
        const Value& inner = value->items.front();
        if (inner.kind == Value::Number) return inner.number;
    }
    return fallback;
}

int RefOf(const std::vector<Value>* args, size_t index) {
    const Value* value = At(args, index);
    return (value && value->kind == Value::Ref) ? value->ref : 0;
}

bool BoolOf(const std::vector<Value>* args, size_t index, bool fallback = true) {
    const Value* value = At(args, index);
    if (!value || value->kind != Value::Enum) return fallback;
    if (value->text == "T" || value->text == "TRUE") return true;
    if (value->text == "F" || value->text == "FALSE") return false;
    return fallback;
}

std::string TextOf(const std::vector<Value>* args, size_t index) {
    const Value* value = At(args, index);
    return (value && value->kind == Value::Text) ? value->text : std::string();
}

Vec3 ReadVector(const Doc& doc, const Value& value) {
    if (value.kind != Value::Ref) return {};
    const std::vector<Value>* args = doc.ArgsAny(value.ref);
    const std::vector<Value>* coords = nullptr;
    if (const Value* list = At(args, 1)) {
        if (list->kind == Value::List) coords = &list->items;
    }
    if (!coords) return {};
    const Value* x = At(coords, 0);
    const Value* y = At(coords, 1);
    const Value* z = At(coords, 2);
    if (!x || !y || !z) return {};
    return {x->number, y->number, z->number};
}

Vec3 ReadPoint(const Doc& doc, const std::vector<Value>* args, size_t index) {
    return ReadVector(doc, At(args, index) ? *At(args, index) : Value{});
}

Vec3 ReadDirection(const Doc& doc, const std::vector<Value>* args, size_t index) {
    return ReadVector(doc, At(args, index) ? *At(args, index) : Value{});
}

Vec3 ReadCartesianRef(const Doc& doc, const Value& value) {
    return ReadVector(doc, value);
}

struct Frame {
    Vec3 origin;
    Vec3 x{1, 0, 0};
    Vec3 y{0, 1, 0};
    Vec3 z{0, 0, 1};
};

bool ReadFrame(const Doc& doc, const Value& value, Frame& frame) {
    if (value.kind != Value::Ref) return false;
    const std::vector<Value>* args = doc.ArgsOfRef(value, "AXIS2_PLACEMENT_3D");
    if (!args) return false;

    frame.origin = ReadPoint(doc, args, 1);
    if (!IsFinite(frame.origin)) return false;

    Vec3 axis = ReadDirection(doc, args, 2);
    if (Length(axis) < 1.0e-12) axis = Vec3{0, 0, 1};
    frame.z = Normalized(axis);

    Vec3 refDir = ReadDirection(doc, args, 3);
    // Remove the component along the axis; fall back to any perpendicular.
    refDir = refDir - frame.z * Dot(refDir, frame.z);
    if (Length(refDir) < 1.0e-9) {
        refDir = std::fabs(frame.z.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        refDir = refDir - frame.z * Dot(refDir, frame.z);
    }
    frame.x = Normalized(refDir);
    frame.y = Normalized(Cross(frame.z, frame.x));
    return true;
}

// ---------------------------------------------------------------------------
// Analytic geometry
// ---------------------------------------------------------------------------

struct Surface {
    enum Kind { Unsupported, Plane, Cylinder, Cone, Sphere, Torus } kind = Unsupported;
    Frame frame;
    double a = 0.0;  // radius / major radius / cone radius at the origin
    double b = 0.0;  // minor radius / cone semi-angle

    Vec3 Point(double u, double v) const {
        switch (kind) {
            case Plane:
                return frame.origin + frame.x * u + frame.y * v;
            case Cylinder:
                return frame.origin + frame.x * (a * std::cos(u)) +
                       frame.y * (a * std::sin(u)) + frame.z * v;
            case Cone: {
                const double radius = a + v * std::tan(b);
                return frame.origin + frame.x * (radius * std::cos(u)) +
                       frame.y * (radius * std::sin(u)) + frame.z * v;
            }
            case Sphere:
                return frame.origin +
                       frame.x * (a * std::cos(v) * std::cos(u)) +
                       frame.y * (a * std::cos(v) * std::sin(u)) +
                       frame.z * (a * std::sin(v));
            case Torus: {
                const double ring = a + b * std::cos(v);
                return frame.origin + frame.x * (ring * std::cos(u)) +
                       frame.y * (ring * std::sin(u)) + frame.z * (b * std::sin(v));
            }
            default:
                return frame.origin;
        }
    }

    Vec3 Normal(double u, double v) const {
        switch (kind) {
            case Plane:
                return frame.z;
            case Cylinder:
                return frame.x * std::cos(u) + frame.y * std::sin(u);
            case Cone: {
                const Vec3 radial = frame.x * std::cos(u) + frame.y * std::sin(u);
                return Normalized(radial - frame.z * std::tan(b));
            }
            case Sphere:
                return frame.x * (std::cos(v) * std::cos(u)) +
                       frame.y * (std::cos(v) * std::sin(u)) + frame.z * std::sin(v);
            case Torus:
                return frame.x * (std::cos(v) * std::cos(u)) +
                       frame.y * (std::cos(v) * std::sin(u)) + frame.z * std::sin(v);
            default:
                return frame.z;
        }
    }

    bool Inverse(const Vec3& point, double& u, double& v) const {
        const Vec3 d = point - frame.origin;
        switch (kind) {
            case Plane:
                u = Dot(d, frame.x);
                v = Dot(d, frame.y);
                return true;
            case Cylinder:
                u = std::atan2(Dot(d, frame.y), Dot(d, frame.x));
                v = Dot(d, frame.z);
                return true;
            case Cone:
                u = std::atan2(Dot(d, frame.y), Dot(d, frame.x));
                v = Dot(d, frame.z);
                return true;
            case Sphere: {
                const double dz = Dot(d, frame.z);
                u = std::atan2(Dot(d, frame.y), Dot(d, frame.x));
                const double ratio = a > kEps ? dz / a : 0.0;
                v = std::asin(std::max(-1.0, std::min(1.0, ratio)));
                return true;
            }
            case Torus: {
                const double dz = Dot(d, frame.z);
                const double radial = std::sqrt(Dot(d, frame.x) * Dot(d, frame.x) +
                                                Dot(d, frame.y) * Dot(d, frame.y));
                u = std::atan2(Dot(d, frame.y), Dot(d, frame.x));
                v = std::atan2(dz, radial - a);
                return true;
            }
            default:
                return false;
        }
    }

    bool PeriodicU() const { return kind == Cylinder || kind == Cone || kind == Sphere || kind == Torus; }
};

bool ReadFrameArg(const Doc& doc, const std::vector<Value>* args, size_t index, Frame& frame) {
    const Value* value = At(args, index);
    return value && ReadFrame(doc, *value, frame);
}

bool BuildSurface(const Doc& doc, int id, Surface& surface, int depth = 0) {
    if (depth > kMaxDepth) return false;
    const Entity* entity = doc.Find(id);
    if (!entity) return false;

    if (entity->Has("PLANE")) {
        const std::vector<Value>* args = entity->Get("PLANE");
        if (!ReadFrameArg(doc, args, 1, surface.frame)) return false;
        surface.kind = Surface::Plane;
        return true;
    }
    if (entity->Has("CYLINDRICAL_SURFACE")) {
        const std::vector<Value>* args = entity->Get("CYLINDRICAL_SURFACE");
        if (!ReadFrameArg(doc, args, 1, surface.frame)) return false;
        surface.a = Number(args, 2);
        if (!(surface.a > 0.0)) return false;
        surface.kind = Surface::Cylinder;
        return true;
    }
    if (entity->Has("CONICAL_SURFACE")) {
        const std::vector<Value>* args = entity->Get("CONICAL_SURFACE");
        if (!ReadFrameArg(doc, args, 1, surface.frame)) return false;
        surface.a = Number(args, 2);
        surface.b = Number(args, 3);
        surface.kind = Surface::Cone;
        return true;
    }
    if (entity->Has("SPHERICAL_SURFACE")) {
        const std::vector<Value>* args = entity->Get("SPHERICAL_SURFACE");
        if (!ReadFrameArg(doc, args, 1, surface.frame)) return false;
        surface.a = Number(args, 2);
        if (!(surface.a > 0.0)) return false;
        surface.kind = Surface::Sphere;
        return true;
    }
    if (entity->Has("TOROIDAL_SURFACE")) {
        const std::vector<Value>* args = entity->Get("TOROIDAL_SURFACE");
        if (!ReadFrameArg(doc, args, 1, surface.frame)) return false;
        surface.a = Number(args, 2);
        surface.b = Number(args, 3);
        if (!(surface.b > 0.0)) return false;
        surface.kind = Surface::Torus;
        return true;
    }
    return false;
}

struct Curve {
    enum Kind { Unsupported, Line, Circle, Ellipse, Polyline, BSpline } kind = Unsupported;
    Frame frame;
    Vec3 origin;   // line point
    Vec3 direction;  // line direction (scaled by the vector magnitude)
    double a = 0.0;
    double b = 0.0;
    std::vector<Vec3> poly;
    int degree = 0;
    std::vector<Vec3> control;
    std::vector<double> knots;    // expanded
    std::vector<double> weights;  // empty = non-rational

    double DomainStart() const {
        if (knots.empty()) return 0.0;
        return knots[std::min<size_t>(degree, knots.size() - 1)];
    }
    double DomainEnd() const {
        if (knots.empty()) return 1.0;
        const size_t last = knots.size() - 1;
        return knots[last - std::min<size_t>(degree, last)];
    }

    Vec3 Point(double t) const {
        switch (kind) {
            case Line:
                return origin + direction * t;
            case Circle:
                return frame.origin + frame.x * (a * std::cos(t)) + frame.y * (a * std::sin(t));
            case Ellipse:
                return frame.origin + frame.x * (a * std::cos(t)) + frame.y * (b * std::sin(t));
            case Polyline: {
                if (poly.empty()) return {};
                if (poly.size() == 1) return poly.front();
                const double clamped = std::max(0.0, std::min<double>(t, poly.size() - 1.0));
                const size_t index = static_cast<size_t>(std::min(std::floor(clamped), poly.size() - 2.0));
                const double frac = clamped - static_cast<double>(index);
                return poly[index] * (1.0 - frac) + poly[index + 1] * frac;
            }
            case BSpline:
                return EvaluateBSpline(t);
            default:
                return {};
        }
    }

    Vec3 EvaluateBSpline(double t) const {
        const size_t count = control.size();
        if (count == 0) return {};
        if (count == 1) return control.front();
        const size_t p = static_cast<size_t>(std::max(0, degree));
        if (knots.size() != count + p + 1) return control.front();

        const double lo = DomainStart();
        const double hi = DomainEnd();
        t = std::max(lo, std::min(t, hi));
        // Locate the knot span.
        size_t span = p;
        for (size_t i = p; i < count; ++i) {
            if (t >= knots[i] && t <= knots[i + 1]) {
                span = i;
                break;
            }
            if (i + 1 == count) span = count - 1;
        }
        std::vector<Vec3> d(p + 1);
        std::vector<double> dw(p + 1, 1.0);
        const bool rational = weights.size() == count;
        for (size_t j = 0; j <= p; ++j) {
            const size_t index = span - p + j;
            d[j] = control[std::min(index, count - 1)];
            dw[j] = rational ? weights[std::min(index, count - 1)] : 1.0;
            d[j] = d[j] * dw[j];
        }
        for (size_t r = 1; r <= p; ++r) {
            for (size_t j = p + 1; j-- > r;) {
                const size_t i = span - p + j;
                const double denom = knots[i + p + 1 - r] - knots[i];
                const double alpha = denom > kEps ? (t - knots[i]) / denom : 0.0;
                d[j] = d[j] * (1.0 - alpha) + d[j - 1] * alpha;
                dw[j] = dw[j] * (1.0 - alpha) + dw[j - 1] * alpha;
            }
        }
        const double w = dw[p];
        if (std::fabs(w) > kEps) return d[p] * (1.0 / w);
        return d[p];
    }

    bool Param(const Vec3& point, double& t) const {
        const Vec3 d = point - frame.origin;
        switch (kind) {
            case Line: {
                const double denom = Dot(direction, direction);
                if (denom < kEps) return false;
                t = Dot(point - origin, direction) / denom;
                return true;
            }
            case Circle:
                t = std::atan2(Dot(d, frame.y), Dot(d, frame.x));
                return true;
            case Ellipse:
                t = std::atan2(std::fabs(b) > kEps ? Dot(d, frame.y) / b : 0.0,
                               std::fabs(a) > kEps ? Dot(d, frame.x) / a : 0.0);
                return true;
            default:
                return false;
        }
    }
};

bool ExpandKnots(const std::vector<Value>& multiplicities, const std::vector<Value>& knots,
                 std::vector<double>& out, int& total) {
    out.clear();
    total = 0;
    const size_t count = std::min(multiplicities.size(), knots.size());
    for (size_t i = 0; i < count; ++i) {
        const int mult = static_cast<int>(multiplicities[i].number);
        if (mult < 1 || mult > 64) return false;
        for (int m = 0; m < mult; ++m) out.push_back(knots[i].number);
        total += mult;
    }
    return !out.empty();
}

bool BuildCurve(const Doc& doc, int id, Curve& curve, double& trimStart, double& trimEnd,
                bool& hasTrim, int depth = 0) {
    if (depth > kMaxDepth) return false;
    const Entity* entity = doc.Find(id);
    if (!entity) return false;
    hasTrim = false;

    if (entity->Has("SURFACE_CURVE") || entity->Has("SEAM_CURVE")) {
        const std::vector<Value>* args = entity->Get(entity->Has("SURFACE_CURVE")
                                                         ? "SURFACE_CURVE"
                                                         : "SEAM_CURVE");
        const Value* basis = At(args, 1);
        if (!basis || basis->kind != Value::Ref) return false;
        return BuildCurve(doc, basis->ref, curve, trimStart, trimEnd, hasTrim, depth + 1);
    }
    if (entity->Has("TRIMMED_CURVE")) {
        const std::vector<Value>* args = entity->Get("TRIMMED_CURVE");
        const Value* basis = At(args, 1);
        if (!basis || basis->kind != Value::Ref) return false;
        double ignoredStart = 0.0, ignoredEnd = 0.0;
        bool ignoredTrim = false;
        if (!BuildCurve(doc, basis->ref, curve, ignoredStart, ignoredEnd, ignoredTrim, depth + 1)) {
            return false;
        }
        const Value* trim1 = At(args, 2);
        const Value* trim2 = At(args, 3);
        auto readTrim = [&](const Value* value, double& out) {
            if (!value) return false;
            if (value->kind == Value::Number) {
                out = value->number;
                return true;
            }
            if (value->kind == Value::Typed) {
                const Value* inner = At(&value->items, 0);
                if (inner && inner->kind == Value::Number) {
                    out = inner->number;
                    return true;
                }
                return false;
            }
            if (value->kind == Value::Ref) {
                const Vec3 point = ReadCartesianRef(doc, *value);
                return curve.Param(point, out);
            }
            return false;
        };
        double start = 0.0, end = 0.0;
        if (readTrim(trim1, start) && readTrim(trim2, end)) {
            trimStart = start;
            trimEnd = end;
            hasTrim = true;
            return true;
        }
        return true;
    }

    if (entity->Has("LINE")) {
        const std::vector<Value>* args = entity->Get("LINE");
        if (!At(args, 1) || !At(args, 2)) return false;
        curve.origin = ReadPoint(doc, args, 1);
        // LINE(name, point, VECTOR(direction, magnitude)); the vector entity
        // stores its direction in slot 1 and the magnitude in slot 2.
        const std::vector<Value>* vector = doc.ArgsOfRef(*At(args, 2), "VECTOR");
        if (!vector || !At(vector, 1)) return false;
        const Vec3 direction = ReadVector(doc, *At(vector, 1));
        const double magnitude = Number(vector, 2, 1.0);
        curve.direction = direction * magnitude;
        curve.kind = Curve::Line;
        return Length(curve.direction) > 1.0e-12;
    }
    if (entity->Has("CIRCLE")) {
        const std::vector<Value>* args = entity->Get("CIRCLE");
        if (!ReadFrameArg(doc, args, 1, curve.frame)) return false;
        curve.a = Number(args, 2);
        curve.kind = Curve::Circle;
        return curve.a > 0.0;
    }
    if (entity->Has("ELLIPSE")) {
        const std::vector<Value>* args = entity->Get("ELLIPSE");
        if (!ReadFrameArg(doc, args, 1, curve.frame)) return false;
        curve.a = Number(args, 2);
        curve.b = Number(args, 3);
        curve.kind = Curve::Ellipse;
        return curve.a > 0.0 && curve.b > 0.0;
    }
    if (entity->Has("POLYLINE")) {
        const std::vector<Value>* args = entity->Get("POLYLINE");
        const Value* points = At(args, 1);
        if (!points || points->kind != Value::List) return false;
        for (const Value& ref : points->items) curve.poly.push_back(ReadCartesianRef(doc, ref));
        curve.kind = Curve::Polyline;
        return curve.poly.size() >= 2;
    }
    if (entity->Has("B_SPLINE_CURVE_WITH_KNOTS")) {
        const std::vector<Value>* args = entity->Get("B_SPLINE_CURVE_WITH_KNOTS");
        curve.kind = Curve::BSpline;
        curve.degree = static_cast<int>(Number(args, 1));
        const Value* controlPoints = At(args, 2);
        if (!controlPoints || controlPoints->kind != Value::List) return false;
        for (const Value& ref : controlPoints->items) {
            curve.control.push_back(ReadCartesianRef(doc, ref));
        }
        const Value* multiplicities = At(args, 6);
        const Value* knots = At(args, 7);
        if (!multiplicities || !knots || multiplicities->kind != Value::List ||
            knots->kind != Value::List) {
            return false;
        }
        int total = 0;
        if (!ExpandKnots(multiplicities->items, knots->items, curve.knots, total)) return false;
        if (static_cast<size_t>(total) != curve.control.size() + static_cast<size_t>(curve.degree) + 1) {
            return false;
        }
        if (const std::vector<Value>* weights = entity->Get("RATIONAL_B_SPLINE_CURVE")) {
            const Value* list = At(weights, 0);
            if (list && list->kind == Value::List) {
                for (const Value& w : list->items) curve.weights.push_back(w.number);
            }
        }
        return curve.control.size() > static_cast<size_t>(curve.degree) && curve.degree >= 1;
    }
    if (entity->Has("COMPOSITE_CURVE")) {
        const std::vector<Value>* args = entity->Get("COMPOSITE_CURVE");
        const Value* segments = At(args, 1);
        if (!segments || segments->kind != Value::List) return false;
        curve.kind = Curve::Polyline;
        for (const Value& segmentRef : segments->items) {
            const std::vector<Value>* segment =
                doc.ArgsOfRef(segmentRef, "COMPOSITE_CURVE_SEGMENT");
            if (!segment) continue;
            const int parentId = RefOf(segment, 2);
            if (parentId == 0) continue;
            Curve child;
            double childStart = 0.0, childEnd = 0.0;
            bool childTrim = false;
            if (!BuildCurve(doc, parentId, child, childStart, childEnd, childTrim, depth + 1)) continue;
            std::vector<Vec3> points;
            const double lo = childTrim ? childStart : child.DomainStart();
            const double hi = childTrim ? childEnd : child.DomainEnd();
            if (child.kind == Curve::Polyline) {
                points = child.poly;
            } else {
                const int steps = child.kind == Curve::Line ? 2 : 24;
                for (int i = 0; i <= steps; ++i) {
                    points.push_back(child.Point(lo + (hi - lo) * (double(i) / steps)));
                }
            }
            if (!BoolOf(segment, 1, true)) std::reverse(points.begin(), points.end());
            if (!curve.poly.empty() && !points.empty()) points.erase(points.begin());
            curve.poly.insert(curve.poly.end(), points.begin(), points.end());
        }
        return curve.poly.size() >= 2;
    }
    return false;
}

// ---------------------------------------------------------------------------
// B-rep traversal
// ---------------------------------------------------------------------------

struct Ctx {
    const Doc& doc;
    Step::Mesh& mesh;
    int faceBudget = 400000;
    int faceTriangleBudget = 6000;
};

bool ReadVertex(const Doc& doc, int vertexId, Vec3& point) {
    const std::vector<Value>* args = doc.Args(vertexId, "VERTEX_POINT");
    if (!args) return false;
    const Value* ref = At(args, 1);
    if (!ref || ref->kind != Value::Ref) return false;
    point = ReadCartesianRef(doc, *ref);
    return IsFinite(point);
}

void AppendUnique(std::vector<Vec3>& out, const Vec3& point, double eps) {
    if (!out.empty()) {
        const Vec3 delta = out.back() - point;
        if (Dot(delta, delta) <= eps * eps) return;
    }
    out.push_back(point);
}

void SampleCurveSegment(const Curve& curve, double from, double to, bool forward,
                        std::vector<Vec3>& out) {
    const bool periodic = curve.kind == Curve::Circle || curve.kind == Curve::Ellipse;
    double start = from;
    double delta = 0.0;

    if (periodic) {
        delta = std::fmod(to - from, kTwoPi);
        if (delta < 0.0) delta += kTwoPi;
        if (!forward) delta -= kTwoPi;
        if (std::fabs(delta) < 1.0e-9) delta = forward ? kTwoPi : -kTwoPi;
    } else {
        // Reversing an open curve reverses the point order; it must not merely
        // reparameterise the same interval, or the loop would fold back.
        start = forward ? from : to;
        const double stop = forward ? to : from;
        delta = stop - start;
    }

    int steps = 1;
    switch (curve.kind) {
        case Curve::Circle:
        case Curve::Ellipse:
            steps = std::max(2, static_cast<int>(std::ceil(std::fabs(delta) / (kPi / 36.0))));
            steps = std::min(steps, 512);
            break;
        case Curve::BSpline:
            steps = 48;
            break;
        case Curve::Polyline:
            steps = std::max(1, static_cast<int>(std::ceil(std::fabs(delta))));
            steps = std::min(steps, 1024);
            break;
        default:
            steps = 1;
            break;
    }

    for (int i = 0; i <= steps; ++i) {
        const double t = start + delta * (static_cast<double>(i) / static_cast<double>(steps));
        if (i == 0) {
            AppendUnique(out, curve.Point(start), 1.0e-9);
        } else {
            out.push_back(curve.Point(t));
        }
    }
}

bool SampleEdge(const Doc& doc, int edgeId, bool orientation, std::vector<Vec3>& out, int depth) {
    if (depth > kMaxDepth) return false;
    const Entity* entity = doc.Find(edgeId);
    if (!entity) return false;

    if (entity->Has("ORIENTED_EDGE")) {
        const std::vector<Value>* args = entity->Get("ORIENTED_EDGE");
        const bool flip = !BoolOf(args, 4, true);
        const int child = RefOf(args, 3);
        return SampleEdge(doc, child, orientation != flip, out, depth + 1);
    }
    if (!entity->Has("EDGE_CURVE")) return false;

    const std::vector<Value>* args = entity->Get("EDGE_CURVE");
    const int curveId = RefOf(args, 3);
    const bool sameSense = BoolOf(args, 4, true);
    const bool forward = sameSense == orientation;

    Vec3 start, end;
    const bool hasStart = ReadVertex(doc, RefOf(args, 1), start);
    const bool hasEnd = ReadVertex(doc, RefOf(args, 2), end);
    if (!hasStart || !hasEnd) return false;

    Curve curve;
    double trimStart = 0.0, trimEnd = 0.0;
    bool hasTrim = false;
    if (!BuildCurve(doc, curveId, curve, trimStart, trimEnd, hasTrim)) {
        // Best effort: straight chord between the edge vertices.
        AppendUnique(out, start, 1.0e-9);
        AppendUnique(out, end, 1.0e-9);
        return true;
    }

    double from = 0.0, to = 1.0;
    if (hasTrim) {
        from = trimStart;
        to = trimEnd;
    } else if (curve.kind == Curve::Line) {
        if (!curve.Param(start, from) || !curve.Param(end, to)) return false;
    } else if (curve.kind == Curve::Circle || curve.kind == Curve::Ellipse) {
        if (!curve.Param(start, from) || !curve.Param(end, to)) return false;
    } else if (curve.kind == Curve::Polyline) {
        auto nearest = [&](const Vec3& point) {
            size_t best = 0;
            double bestDistance = std::numeric_limits<double>::max();
            for (size_t i = 0; i < curve.poly.size(); ++i) {
                const Vec3 delta = curve.poly[i] - point;
                const double distance = Dot(delta, delta);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = i;
                }
            }
            return static_cast<double>(best);
        };
        from = nearest(start);
        to = nearest(end);
    } else {
        from = curve.DomainStart();
        to = curve.DomainEnd();
    }

    if (!(std::isfinite(from) && std::isfinite(to))) return false;
    SampleCurveSegment(curve, from, to, forward, out);
    return true;
}

bool SampleLoop(const Doc& doc, int loopId, std::vector<Vec3>& out, int depth = 0) {
    const Entity* entity = doc.Find(loopId);
    if (!entity) return false;

    if (entity->Has("POLY_LOOP")) {
        const std::vector<Value>* args = entity->Get("POLY_LOOP");
        const Value* points = At(args, 1);
        if (!points || points->kind != Value::List) return false;
        for (const Value& ref : points->items) {
            AppendUnique(out, ReadCartesianRef(doc, ref), 1.0e-9);
        }
    } else if (entity->Has("EDGE_LOOP")) {
        const std::vector<Value>* args = entity->Get("EDGE_LOOP");
        const Value* edges = At(args, 1);
        if (!edges || edges->kind != Value::List) return false;
        for (const Value& ref : edges->items) {
            if (ref.kind != Value::Ref) continue;
            std::vector<Vec3> segment;
            if (!SampleEdge(doc, ref.ref, true, segment, depth)) continue;
            for (const Vec3& point : segment) AppendUnique(out, point, 1.0e-9);
        }
    } else if (entity->Has("VERTEX_LOOP")) {
        return false;
    } else if (entity->Has("PATH")) {
        return false;
    } else {
        return false;
    }

    if (out.size() > 1) {
        const Vec3 delta = out.front() - out.back();
        if (Dot(delta, delta) <= 1.0e-18) out.pop_back();
    }
    return out.size() >= 3;
}

// ---------------------------------------------------------------------------
// 2D polygon triangulation (UV space)
// ---------------------------------------------------------------------------

double SignedArea(const std::vector<Vec2>& polygon) {
    double sum = 0.0;
    const size_t n = polygon.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[(i + 1) % n];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum * 0.5;
}

void CleanupPolygon(std::vector<Vec2>& polygon, double eps, bool removeCollinear) {
    if (polygon.size() < 3) return;
    std::vector<Vec2> out;
    out.reserve(polygon.size());
    for (const Vec2& point : polygon) {
        if (!out.empty()) {
            const double dx = point.x - out.back().x;
            const double dy = point.y - out.back().y;
            if (dx * dx + dy * dy <= eps * eps) continue;
        }
        out.push_back(point);
    }
    while (out.size() > 2) {
        const double dx = out.front().x - out.back().x;
        const double dy = out.front().y - out.back().y;
        if (dx * dx + dy * dy > eps * eps) break;
        out.pop_back();
    }
    // Drop exactly collinear vertices; sampled arcs keep their curvature.
    if (removeCollinear && out.size() >= 3) {
        std::vector<Vec2> cleaned;
        cleaned.reserve(out.size());
        for (size_t i = 0; i < out.size(); ++i) {
            const Vec2& prev = out[(i + out.size() - 1) % out.size()];
            const Vec2& cur = out[i];
            const Vec2& next = out[(i + 1) % out.size()];
            const Vec2 a = cur - prev;
            const Vec2 b = next - cur;
            const double area = Cross2(a, b);
            const double scale = std::sqrt((a.x * a.x + a.y * a.y) * (b.x * b.x + b.y * b.y));
            if (scale > 0.0 && std::fabs(area) <= 1.0e-10 * scale) continue;
            cleaned.push_back(cur);
        }
        if (cleaned.size() >= 3) out.swap(cleaned);
    }
    if (out.size() >= 3) polygon.swap(out);
}

bool PointInTriangle(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
    const double d1 = Cross2(b - a, p - a);
    const double d2 = Cross2(c - b, p - b);
    const double d3 = Cross2(a - c, p - c);
    const double eps = 1.0e-12;
    const bool positive = d1 > eps && d2 > eps && d3 > eps;
    const bool negative = d1 < -eps && d2 < -eps && d3 < -eps;
    return positive || negative;
}

// Ear clipping variant: a vertex that sits exactly on an ear's edge still makes
// that ear invalid. With the strict test a hole boundary can sneak through the
// ear (its own vertices are outside, but an edge crosses the triangle), and the
// clipper then fills the hole with a wedge. Treating the boundary as blocking
// is conservative and keeps the ring simple.
bool VertexBlocksEar(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
    const double eps = 1.0e-12;
    return Cross2(b - a, p - a) >= -eps && Cross2(c - b, p - b) >= -eps &&
           Cross2(a - c, p - c) >= -eps;
}

bool PointInPolygon(const Vec2& p, const std::vector<Vec2>& polygon) {
    bool inside = false;
    const size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

bool SegmentHitsSegment(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
    const double d1 = Cross2(b - a, c - a);
    const double d2 = Cross2(b - a, d - a);
    const double d3 = Cross2(d - c, a - c);
    const double d4 = Cross2(d - c, b - c);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
           ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

// X-monotone domains (a cylindrical wrap, a trimmed rectangle) can be split
// into two chains at the extreme-u vertices and stitched into a triangle
// strip. That keeps the diagonals short, which matters because a UV triangle
// is a flat chord through a curved surface. Returns false when the polygon is
// not monotone, in which case the caller falls back to ear clipping.
bool StripTriangulate(const std::vector<Vec2>& ring, const std::vector<int>& indexMap,
                      std::vector<std::array<int, 3>>& out) {
    const size_t n = ring.size();
    if (n < 4) return false;

    size_t left = 0;
    size_t right = 0;
    for (size_t i = 1; i < n; ++i) {
        if (ring[i].x < ring[left].x - 1.0e-12 ||
            (std::fabs(ring[i].x - ring[left].x) <= 1.0e-12 && ring[i].y < ring[left].y)) {
            left = i;
        }
        if (ring[i].x > ring[right].x + 1.0e-12 ||
            (std::fabs(ring[i].x - ring[right].x) <= 1.0e-12 && ring[i].y > ring[right].y)) {
            right = i;
        }
    }
    if (left == right) return false;

    std::vector<size_t> chainA;
    std::vector<size_t> chainB;
    for (size_t i = left;; i = (i + 1) % n) {
        chainA.push_back(i);
        if (i == right) break;
        if (chainA.size() > n) return false;
    }
    for (size_t i = right;; i = (i + 1) % n) {
        chainB.push_back(i);
        if (i == left) break;
        if (chainB.size() > n) return false;
    }
    std::reverse(chainB.begin(), chainB.end());

    for (size_t i = 1; i < chainA.size(); ++i) {
        if (ring[chainA[i]].x < ring[chainA[i - 1]].x - 1.0e-9) return false;
    }
    for (size_t i = 1; i < chainB.size(); ++i) {
        if (ring[chainB[i]].x < ring[chainB[i - 1]].x - 1.0e-9) return false;
    }

    size_t i = 0;
    size_t j = 0;
    while (i + 1 < chainA.size() || j + 1 < chainB.size()) {
        const bool advanceA = (j + 1 >= chainB.size()) ||
                              (i + 1 < chainA.size() &&
                               ring[chainA[i + 1]].x <= ring[chainB[j + 1]].x);
        if (advanceA) {
            out.push_back({indexMap[chainA[i]], indexMap[chainA[i + 1]], indexMap[chainB[j]]});
            ++i;
        } else {
            out.push_back({indexMap[chainB[j]], indexMap[chainB[j + 1]], indexMap[chainA[i]]});
            ++j;
        }
    }
    return !out.empty();
}

bool EarClip(std::vector<Vec2>& polygon, std::vector<std::array<int, 3>>& triangles,
             const std::vector<int>& globalIndex) {
    const size_t n = polygon.size();
    if (n < 3) return false;
    std::vector<int> remaining(n);
    for (size_t i = 0; i < n; ++i) remaining[i] = static_cast<int>(i);

    int guard = static_cast<int>(n) * static_cast<int>(n) + 16;
    while (remaining.size() > 3 && guard-- > 0) {
        bool clipped = false;
        const size_t count = remaining.size();
        for (size_t i = 0; i < count; ++i) {
            const size_t prevIndex = remaining[(i + count - 1) % count];
            const size_t curIndex = remaining[i];
            const size_t nextIndex = remaining[(i + 1) % count];
            const Vec2& a = polygon[prevIndex];
            const Vec2& b = polygon[curIndex];
            const Vec2& c = polygon[nextIndex];
            const double corner = Cross2(b - a, c - b);
            if (corner < -1.0e-14) continue;  // reflex
            // Collinear corners appear where a hole bridge doubles back on
            // itself. Dropping them without a triangle keeps the ring valid.
            const bool flat = corner <= 1.0e-14;

            bool blocked = false;
            if (!flat) {
                for (size_t j = 0; j < count; ++j) {
                    const size_t testIndex = remaining[j];
                    if (testIndex == prevIndex || testIndex == curIndex || testIndex == nextIndex) {
                        continue;
                    }
                    // A duplicate of the ear's own corner (the two sides of a
                    // hole bridge) does not block the ear.
                    const Vec2& q = polygon[testIndex];
                    if ((std::fabs(q.x - a.x) <= 1.0e-9 && std::fabs(q.y - a.y) <= 1.0e-9) ||
                        (std::fabs(q.x - b.x) <= 1.0e-9 && std::fabs(q.y - b.y) <= 1.0e-9) ||
                        (std::fabs(q.x - c.x) <= 1.0e-9 && std::fabs(q.y - c.y) <= 1.0e-9)) {
                        continue;
                    }
                    if (VertexBlocksEar(q, a, b, c)) {
                        blocked = true;
                        break;
                    }
                }
            }
            if (blocked) continue;

            if (!flat) {
                triangles.push_back(
                    {globalIndex[prevIndex], globalIndex[curIndex], globalIndex[nextIndex]});
            }
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            // Degenerate input: drop the flattest vertex so the loop terminates.
            double smallest = std::numeric_limits<double>::max();
            size_t victim = 0;
            for (size_t i = 0; i < count; ++i) {
                const Vec2& a = polygon[remaining[(i + count - 1) % count]];
                const Vec2& b = polygon[remaining[i]];
                const Vec2& c = polygon[remaining[(i + 1) % count]];
                const double area = std::fabs(Cross2(b - a, c - b));
                if (area < smallest) {
                    smallest = area;
                    victim = i;
                }
            }
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(victim));
        }
    }
    if (remaining.size() == 3) {
        const Vec2& a = polygon[remaining[0]];
        const Vec2& b = polygon[remaining[1]];
        const Vec2& c = polygon[remaining[2]];
        if (std::fabs(Cross2(b - a, c - a)) > 1.0e-16) {
            triangles.push_back({globalIndex[remaining[0]], globalIndex[remaining[1]],
                                 globalIndex[remaining[2]]});
        }
    }
    return !triangles.empty();
}

void RemoveClosingDuplicates(std::vector<Vec2>& polygon, double eps) {
    if (polygon.size() > 1) {
        const double dx = polygon.front().x - polygon.back().x;
        const double dy = polygon.front().y - polygon.back().y;
        if (dx * dx + dy * dy <= eps * eps) polygon.pop_back();
    }
}

// ---------------------------------------------------------------------------
// Face tessellation
// ---------------------------------------------------------------------------

bool ToParametric(const Surface& surface, const std::vector<Vec3>& points,
                  std::vector<Vec2>& out) {
    out.clear();
    out.reserve(points.size());
    bool havePrevious = false;
    double previousU = 0.0;
    for (const Vec3& point : points) {
        double u = 0.0, v = 0.0;
        if (!surface.Inverse(point, u, v)) return false;
        if (!std::isfinite(u) || !std::isfinite(v)) return false;
        if (surface.PeriodicU() && havePrevious) {
            while (u - previousU > kPi) u -= kTwoPi;
            while (u - previousU < -kPi) u += kTwoPi;
        }
        previousU = u;
        havePrevious = true;
        out.push_back({u, v});
    }
    return out.size() >= 3;
}

void EmitTriangle(Ctx& ctx, const Surface& surface, bool sameSense, const Vec2& a,
                  const Vec2& b, const Vec2& c) {
    if (ctx.faceBudget-- <= 0) return;
    const Vec3 pa = surface.Point(a.x, a.y);
    Vec3 pb = surface.Point(b.x, b.y);
    Vec3 pc = surface.Point(c.x, c.y);
    if (!IsFinite(pa) || !IsFinite(pb) || !IsFinite(pc)) return;

    Vec3 normal = Cross(pb - pa, pc - pa);
    const double length = Length(normal);
    if (!(length > 1.0e-14)) return;
    normal = normal * (1.0 / length);

    // The UV loop is normalised to positive area, so the winding normal agrees
    // with the surface normal; same_sense flips the face's outward direction.
    const Vec2 centre{(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0};
    const Vec3 outward = surface.Normal(centre.x, centre.y);
    if (Dot(normal, outward) * (sameSense ? 1.0 : -1.0) < 0.0) {
        std::swap(pb, pc);
        normal = -normal;
    }

    // Flat per-triangle normals: a UV triangle is a flat chord, so mixing the
    // analytic surface normal into it gives wrong lighting wherever the chord
    // spans a wide arc.
    const uint32_t base = static_cast<uint32_t>(ctx.mesh.positions.size() / 3);
    const Vec3 positions[3] = {pa, pb, pc};
    for (int i = 0; i < 3; ++i) {
        ctx.mesh.positions.push_back(static_cast<float>(positions[i].x));
        ctx.mesh.positions.push_back(static_cast<float>(positions[i].y));
        ctx.mesh.positions.push_back(static_cast<float>(positions[i].z));
        ctx.mesh.normals.push_back(static_cast<float>(normal.x));
        ctx.mesh.normals.push_back(static_cast<float>(normal.y));
        ctx.mesh.normals.push_back(static_cast<float>(normal.z));
    }
    ctx.mesh.indices.push_back(base);
    ctx.mesh.indices.push_back(base + 1);
    ctx.mesh.indices.push_back(base + 2);
}

// Characteristic radius of a face on a curved surface, in model units.
double CurvatureRadius(const Surface& surface, const std::vector<Vec2>& outer) {
    switch (surface.kind) {
        case Surface::Sphere:
            return surface.a;
        case Surface::Torus:
            return surface.a + surface.b;
        case Surface::Cylinder:
            return surface.a;
        case Surface::Cone: {
            double radius = std::fabs(surface.a);
            for (const Vec2& p : outer) {
                const Vec3 point = surface.Point(p.x, p.y);
                const Vec3 d = point - surface.frame.origin;
                radius = std::max(radius, std::sqrt(Dot(d, surface.frame.x) * Dot(d, surface.frame.x) +
                                                    Dot(d, surface.frame.y) * Dot(d, surface.frame.y)));
            }
            return radius;
        }
        default:
            return 0.0;
    }
}

// Inserts a regular grid of interior points into the (u,v) triangulation. This
// is what keeps a full cylindrical wrap from being meshed by a handful of long
// chords: the strip/ear triangulation fixes the boundary, the grid fixes the
// interior. Points are only accepted strictly inside the trimmed region.
void DensifyGrid(const Surface& surface, const std::vector<Vec2>& ring,
                 const std::vector<std::vector<Vec2>>& holes, double radius,
                 std::vector<Vec2>& combined, std::vector<std::array<int, 3>>& triangles) {
    if (radius <= 0.0 || triangles.empty() || ring.empty()) return;

    double lo[2] = {ring[0].x, ring[0].y};
    double hi[2] = {ring[0].x, ring[0].y};
    for (const Vec2& p : ring) {
        lo[0] = std::min(lo[0], p.x);
        lo[1] = std::min(lo[1], p.y);
        hi[0] = std::max(hi[0], p.x);
        hi[1] = std::max(hi[1], p.y);
    }

    const double span[2] = {hi[0] - lo[0], hi[1] - lo[1]};
    const double spacing = radius * 0.1;  // ~5.7 degrees of arc
    double du = 0.0;
    double dv = 0.0;
    switch (surface.kind) {
        case Surface::Sphere:
            du = spacing / radius;
            dv = spacing / radius;
            break;
        case Surface::Torus:
            du = spacing / std::max(radius, 1.0e-9);
            dv = spacing / std::max(surface.b, 1.0e-9);
            break;
        default:
            // Cylinders and cones are ruled along v: one interior row is
            // enough, and it keeps a long barrel from becoming a dense grid.
            du = spacing / radius;
            dv = std::max(span[1], spacing);
            break;
    }
    if (!(du > 0.0) || !(dv > 0.0)) return;

    const double columns = span[0] / du;
    const double rows = span[1] / dv;
    if (!(columns > 0.0) || !(rows > 0.0)) return;
    if (columns > 4000.0 || rows > 4000.0) return;
    if (columns * rows > 40000.0) return;

    for (double v = lo[1] + dv * 0.5; v < hi[1]; v += dv) {
        for (double u = lo[0] + du * 0.5; u < hi[0]; u += du) {
            const Vec2 p{u, v};
            if (!PointInPolygon(p, ring)) continue;
            bool inHole = false;
            for (const std::vector<Vec2>& hole : holes) {
                if (hole.size() >= 3 && PointInPolygon(p, hole)) {
                    inHole = true;
                    break;
                }
            }
            if (inHole) continue;

            for (size_t t = 0; t < triangles.size(); ++t) {
                const std::array<int, 3> tri = triangles[t];  // copy: push_back reallocates
                const Vec2& a = combined[tri[0]];
                const Vec2& b = combined[tri[1]];
                const Vec2& c = combined[tri[2]];
                if (!PointInTriangle(p, a, b, c)) continue;
                combined.push_back(p);
                const int index = static_cast<int>(combined.size() - 1);
                triangles[t] = {tri[0], tri[1], index};
                triangles.push_back({tri[1], tri[2], index});
                triangles.push_back({tri[2], tri[0], index});
                break;
            }
        }
    }
}

// Curved faces are ear-clipped in (u,v), which turns a full cylindrical wrap
// into long chords. Bisecting the edge with the largest sagitta (in UV, so the
// new vertex stays exactly on the surface) adapts the mesh to the curvature
// without refining the straight direction.
// Distance between the surface point at the UV midpoint of an edge and the
// midpoint of its 3D chord: the sagitta of that edge. Zero for straight
// generatrices, maximal across a wide arc.
double EdgeSagitta(const Surface& surface, const Vec2& a, const Vec2& b) {
    const Vec2 mid{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
    const Vec3 pa = surface.Point(a.x, a.y);
    const Vec3 pb = surface.Point(b.x, b.y);
    const Vec3 pm = surface.Point(mid.x, mid.y);
    return Length(pm - (pa + pb) * 0.5);
}

void EmitRefined(Ctx& ctx, const Surface& surface, bool sameSense, const Vec2& a,
                 const Vec2& b, const Vec2& c, double tolerance, int depth) {
    if (depth >= 16 || ctx.faceTriangleBudget <= 0) {
        EmitTriangle(ctx, surface, sameSense, a, b, c);
        return;
    }
    const double dab = EdgeSagitta(surface, a, b);
    const double dbc = EdgeSagitta(surface, b, c);
    const double dca = EdgeSagitta(surface, c, a);
    const double worst = std::max(dab, std::max(dbc, dca));
    if (!(worst > tolerance)) {
        --ctx.faceTriangleBudget;
        EmitTriangle(ctx, surface, sameSense, a, b, c);
        return;
    }
    if (dab >= dbc && dab >= dca) {
        const Vec2 mid{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
        EmitRefined(ctx, surface, sameSense, a, mid, c, tolerance, depth + 1);
        EmitRefined(ctx, surface, sameSense, mid, b, c, tolerance, depth + 1);
    } else if (dbc >= dca) {
        const Vec2 mid{(b.x + c.x) * 0.5, (b.y + c.y) * 0.5};
        EmitRefined(ctx, surface, sameSense, a, b, mid, tolerance, depth + 1);
        EmitRefined(ctx, surface, sameSense, a, mid, c, tolerance, depth + 1);
    } else {
        const Vec2 mid{(c.x + a.x) * 0.5, (c.y + a.y) * 0.5};
        EmitRefined(ctx, surface, sameSense, a, b, mid, tolerance, depth + 1);
        EmitRefined(ctx, surface, sameSense, mid, b, c, tolerance, depth + 1);
    }
}

bool TessellateFace(Ctx& ctx, int faceId) {
    const Doc& doc = ctx.doc;
    const Entity* entity = doc.Find(faceId);
    if (!entity) return false;
    const char* faceType = entity->Has("ADVANCED_FACE")
                               ? "ADVANCED_FACE"
                               : (entity->Has("FACE_SURFACE") ? "FACE_SURFACE" : nullptr);
    if (!faceType) return false;
    const std::vector<Value>* args = entity->Get(faceType);

    const int surfaceId = RefOf(args, 2);
    const bool sameSense = BoolOf(args, 3, true);

    Surface surface;
    if (!BuildSurface(doc, surfaceId, surface)) return false;

    const Value* bounds = At(args, 1);
    if (!bounds || bounds->kind != Value::List || bounds->items.empty()) return false;

    struct Loop {
        std::vector<Vec2> points;
        bool outer = false;
        double area = 0.0;
    };
    std::vector<Loop> loops;

    bool boundsComplete = true;
    for (const Value& boundRef : bounds->items) {
        if (boundRef.kind != Value::Ref) continue;
        const Entity* bound = doc.Find(boundRef.ref);
        if (!bound) continue;
        const bool isOuter = bound->Has("FACE_OUTER_BOUND");
        const std::vector<Value>* boundArgs = isOuter ? bound->Get("FACE_OUTER_BOUND")
                                                      : bound->Get("FACE_BOUND");
        if (!boundArgs) continue;
        const int loopId = RefOf(boundArgs, 1);
        if (loopId == 0) continue;

        std::vector<Vec3> points3d;
        if (!SampleLoop(doc, loopId, points3d)) {
            boundsComplete = false;
            continue;
        }
        std::vector<Vec2> points2d;
        if (!ToParametric(surface, points3d, points2d)) {
            boundsComplete = false;
            continue;
        }
        // Keep the exact CAD boundary as a line list. Coincident faces (a bore
        // lid, a perforated plate patch) are invisible in a shaded render but
        // SolidWorks still shows their circular edges.
        if (points3d.size() >= 3) {
            for (size_t i = 0; i < points3d.size(); ++i) {
                const Vec3& p0 = points3d[i];
                const Vec3& p1 = points3d[(i + 1) % points3d.size()];
                const Vec3 pair[2] = {p0, p1};
                for (const Vec3& p : pair) {
                    ctx.mesh.edges.push_back(static_cast<float>(p.x));
                    ctx.mesh.edges.push_back(static_cast<float>(p.y));
                    ctx.mesh.edges.push_back(static_cast<float>(p.z));
                }
            }
        }

        double extent = 0.0;
        for (const Vec2& p : points2d) extent = std::max(extent, std::max(std::fabs(p.x), std::fabs(p.y)));
        const double cleanupEps = std::max(1.0e-9, extent * 1.0e-7);
        RemoveClosingDuplicates(points2d, cleanupEps);
        // On a curved surface the UV image of a circle is a straight line:
        // those collinear samples are the only thing that follows the surface
        // when the (u,v) domain is triangulated, so they must be kept.
        CleanupPolygon(points2d, cleanupEps, surface.kind == Surface::Plane);
        if (points2d.size() < 3) {
            boundsComplete = false;
            continue;
        }

        Loop loop;
        loop.points = std::move(points2d);
        loop.outer = isOuter;
        loop.area = SignedArea(loop.points);
        if (std::fabs(loop.area) < 1.0e-14) {
            boundsComplete = false;
            continue;
        }
        loops.push_back(std::move(loop));
    }

    if (loops.empty() || !boundsComplete) return false;

    size_t outerIndex = 0;
    bool haveOuter = false;
    for (size_t i = 0; i < loops.size(); ++i) {
        if (!loops[i].outer) continue;
        outerIndex = i;
        haveOuter = true;
        break;
    }
    if (!haveOuter) {
        double largest = 0.0;
        for (size_t i = 0; i < loops.size(); ++i) {
            const double area = std::fabs(loops[i].area);
            if (area > largest) {
                largest = area;
                outerIndex = i;
            }
        }
    }

    // Outer ring counter-clockwise, holes clockwise: the ear clipper expects
    // positive orientation for the boundary it consumes.
    if (loops[outerIndex].area < 0.0) {
        std::reverse(loops[outerIndex].points.begin(), loops[outerIndex].points.end());
        loops[outerIndex].area = -loops[outerIndex].area;
    }
    std::vector<std::vector<Vec2>> holes;
    for (size_t i = 0; i < loops.size(); ++i) {
        if (i == outerIndex) continue;
        if (loops[i].area > 0.0) {
            std::reverse(loops[i].points.begin(), loops[i].points.end());
            loops[i].area = -loops[i].area;
        }
        holes.push_back(std::move(loops[i].points));
    }

    const std::vector<Vec2>& outerPoints = loops[outerIndex].points;
    std::vector<std::array<int, 3>> local;
    // Triangulate in a self-contained point array: the helper returns indices
    // into a combined list of outer + hole points.
    std::vector<Vec2> combined = outerPoints;
    std::vector<int> map(outerPoints.size());
    for (size_t i = 0; i < outerPoints.size(); ++i) map[i] = static_cast<int>(i);

    // Bridge holes manually while keeping a single point array.
    std::vector<Vec2> ring = outerPoints;
    std::vector<int> ringMap = map;
    bool ok = false;
    {
        // Bounding boxes let the bridge test skip far-away holes cheaply.
        std::vector<std::array<double, 4>> holeBoxes;
        holeBoxes.reserve(holes.size());
        for (const std::vector<Vec2>& hole : holes) {
            if (hole.empty()) {
                holeBoxes.push_back({0, 0, 0, 0});
                continue;
            }
            double x0 = hole[0].x, x1 = hole[0].x, y0 = hole[0].y, y1 = hole[0].y;
            for (const Vec2& p : hole) {
                x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
                y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
            }
            holeBoxes.push_back({x0, y0, x1, y1});
        }

        for (size_t holeIndex = 0; holeIndex < holes.size(); ++holeIndex) {
            const std::vector<Vec2>& hole = holes[holeIndex];
            if (hole.size() < 3) continue;
            // The bridge only needs the closest *visible* vertex pair. Ranking
            // every pair by distance and testing just the closest handful keeps
            // a 48-hole plate linear instead of quadratic in ring x hole.
            struct Candidate {
                size_t outerIndex = 0;
                size_t holeIndex = 0;
                double distance = 0.0;
            };
            std::vector<Candidate> candidates;
            candidates.reserve(ring.size() * hole.size());
            for (size_t i = 0; i < ring.size(); ++i) {
                for (size_t j = 0; j < hole.size(); ++j) {
                    const Vec2 delta = hole[j] - ring[i];
                    candidates.push_back({i, j, delta.x * delta.x + delta.y * delta.y});
                }
            }
            const size_t probeCount = std::min<size_t>(48, candidates.size());
            std::partial_sort(candidates.begin(), candidates.begin() + probeCount,
                              candidates.end(),
                              [](const Candidate& a, const Candidate& b) {
                                  return a.distance < b.distance;
                              });

            struct Bridge {
                size_t outerIndex = 0;
                size_t holeIndex = 0;
                bool valid = false;
            } best;
            for (size_t c = 0; c < probeCount && !best.valid; ++c) {
                const size_t i = candidates[c].outerIndex;
                const size_t j = candidates[c].holeIndex;
                const Vec2 mid = (hole[j] + ring[i]) * 0.5;
                if (!PointInPolygon(mid, ring)) continue;
                bool blocked = false;
                for (size_t k = 0; k < ring.size() && !blocked; ++k) {
                    const Vec2& a = ring[k];
                    const Vec2& b = ring[(k + 1) % ring.size()];
                    if (k == i || (k + 1) % ring.size() == i) continue;
                    if (SegmentHitsSegment(ring[i], hole[j], a, b)) blocked = true;
                }
                // A bridge may not cut through another hole: that would make
                // the merged ring self-intersecting and the ear clipper would
                // then cover the very holes it is supposed to keep open.
                if (!blocked) {
                    const double sx0 = std::min(ring[i].x, hole[j].x);
                    const double sx1 = std::max(ring[i].x, hole[j].x);
                    const double sy0 = std::min(ring[i].y, hole[j].y);
                    const double sy1 = std::max(ring[i].y, hole[j].y);
                    for (size_t h = 0; h < holes.size() && !blocked; ++h) {
                        if (h == holeIndex || holes[h].size() < 3) continue;
                        const auto& box = holeBoxes[h];
                        if (box[2] < sx0 || box[0] > sx1 || box[3] < sy0 || box[1] > sy1) continue;
                        for (size_t k = 0; k < holes[h].size() && !blocked; ++k) {
                            const Vec2& a = holes[h][k];
                            const Vec2& b = holes[h][(k + 1) % holes[h].size()];
                            if (SegmentHitsSegment(ring[i], hole[j], a, b)) blocked = true;
                        }
                    }
                }
                if (blocked) continue;
                best.outerIndex = i;
                best.holeIndex = j;
                best.valid = true;
            }
            if (!best.valid) continue;
            std::vector<Vec2> merged;
            std::vector<int> mergedMap;
            for (size_t i = 0; i <= best.outerIndex; ++i) {
                merged.push_back(ring[i]);
                mergedMap.push_back(ringMap[i]);
            }
            for (size_t step = 0; step < hole.size(); ++step) {
                const size_t j = (best.holeIndex + step) % hole.size();
                combined.push_back(hole[j]);
                merged.push_back(hole[j]);
                mergedMap.push_back(static_cast<int>(combined.size() - 1));
            }
            combined.push_back(hole[best.holeIndex]);
            merged.push_back(hole[best.holeIndex]);
            mergedMap.push_back(static_cast<int>(combined.size() - 1));
            combined.push_back(ring[best.outerIndex]);
            merged.push_back(ring[best.outerIndex]);
            mergedMap.push_back(static_cast<int>(combined.size() - 1));
            for (size_t i = best.outerIndex + 1; i < ring.size(); ++i) {
                merged.push_back(ring[i]);
                mergedMap.push_back(ringMap[i]);
            }
            ring.swap(merged);
            ringMap.swap(mergedMap);
        }
        std::vector<std::array<int, 3>> strip;
        if (StripTriangulate(ring, ringMap, strip)) {
            double stripArea = 0.0;
            for (const std::array<int, 3>& t : strip) {
                const Vec2& p0 = combined[t[0]];
                const Vec2& p1 = combined[t[1]];
                const Vec2& p2 = combined[t[2]];
                stripArea += std::fabs(Cross2(p1 - p0, p2 - p0)) * 0.5;
            }
            const double ringArea = std::fabs(SignedArea(ring));
            if (std::fabs(stripArea - ringArea) <= std::max(1.0e-12, ringArea * 1.0e-3)) {
                local.swap(strip);
                ok = true;
            }
        }
        if (!ok) {
            std::vector<Vec2> clipRing = ring;
            ok = EarClip(clipRing, local, ringMap);
        }
    }

    if (!ok || local.empty()) return false;

    // A planar face has to be covered exactly once. A self-intersecting ring
    // can make the clipper cover a hole with a wedge; drawing that silently
    // corrupts the model, so the face is skipped and reported instead.
    if (surface.kind == Surface::Plane) {
        double covered = 0.0;
        for (const std::array<int, 3>& triangle : local) {
            if (triangle[0] < 0 || triangle[1] < 0 || triangle[2] < 0) continue;
            if (static_cast<size_t>(triangle[0]) >= combined.size() ||
                static_cast<size_t>(triangle[1]) >= combined.size() ||
                static_cast<size_t>(triangle[2]) >= combined.size()) {
                continue;
            }
            const Vec2& p0 = combined[triangle[0]];
            const Vec2& p1 = combined[triangle[1]];
            const Vec2& p2 = combined[triangle[2]];
            covered += std::fabs(Cross2(p1 - p0, p2 - p0)) * 0.5;
        }
        const double region = std::fabs(SignedArea(ring));
        if (std::fabs(covered - region) > std::max(1.0e-9, region * 0.02)) return false;
    }

    ctx.faceTriangleBudget = 6000;
    const double radius = CurvatureRadius(surface, outerPoints);
    if (radius > 0.0) {
        DensifyGrid(surface, ring, holes, radius, combined, local);
    }
    const double tolerance = std::max(1.0e-9, radius * 1.5e-3);

    const size_t before = ctx.mesh.indices.size();
    for (const std::array<int, 3>& triangle : local) {
        if (triangle[0] < 0 || triangle[1] < 0 || triangle[2] < 0) continue;
        if (static_cast<size_t>(triangle[0]) >= combined.size()) continue;
        if (static_cast<size_t>(triangle[1]) >= combined.size()) continue;
        if (static_cast<size_t>(triangle[2]) >= combined.size()) continue;
        EmitRefined(ctx, surface, sameSense, combined[triangle[0]], combined[triangle[1]],
                    combined[triangle[2]], tolerance, 0);
    }
    return ctx.mesh.indices.size() > before;
}

void CollectShellFaces(Ctx& ctx, int shellId, std::unordered_set<int>& seenFaces,
                       std::unordered_set<int>& seenShells, int depth) {
    if (depth > kMaxDepth || shellId <= 0 || !seenShells.insert(shellId).second) return;
    const Doc& doc = ctx.doc;
    const Entity* entity = doc.Find(shellId);
    if (!entity) return;

    int target = shellId;
    bool orientation = true;
    if (entity->Has("ORIENTED_CLOSED_SHELL")) {
        const std::vector<Value>* args = entity->Get("ORIENTED_CLOSED_SHELL");
        target = RefOf(args, 1);
        orientation = BoolOf(args, 2, true);
        const Entity* inner = doc.Find(target);
        if (!inner) return;
        entity = inner;
        if (!orientation) {
            // Reversed shells are rare; faces keep their own orientation.
        }
    }

    const std::vector<Value>* faces = nullptr;
    if (entity->Has("CLOSED_SHELL")) faces = entity->Get("CLOSED_SHELL");
    else if (entity->Has("OPEN_SHELL")) faces = entity->Get("OPEN_SHELL");
    else if (entity->Has("CONNECTED_FACE_SET")) faces = entity->Get("CONNECTED_FACE_SET");
    if (!faces) return;
    const Value* list = At(faces, 1);
    if (!list || list->kind != Value::List) return;
    for (const Value& ref : list->items) {
        if (ref.kind == Value::Ref) seenFaces.insert(ref.ref);
    }
}

void CollectFaces(Ctx& ctx, std::vector<int>& faces) {
    const Doc& doc = ctx.doc;
    std::vector<int> breps;
    std::vector<int> shells;
    for (const auto& entry : doc.entities) {
        const Entity& entity = entry.second;
        if (entity.Has("MANIFOLD_SOLID_BREP") || entity.Has("BREP_WITH_VOIDS") ||
            entity.Has("FACETED_BREP")) {
            breps.push_back(entry.first);
        } else if (entity.Has("CLOSED_SHELL") || entity.Has("OPEN_SHELL") ||
                   entity.Has("CONNECTED_FACE_SET") || entity.Has("SHELL_BASED_SURFACE_MODEL")) {
            shells.push_back(entry.first);
        }
    }

    std::unordered_set<int> seenFaces;
    std::unordered_set<int> seenShells;
    for (int id : breps) {
        const Entity* entity = doc.Find(id);
        if (!entity) continue;
        if (entity->Has("MANIFOLD_SOLID_BREP")) {
            const std::vector<Value>* args = entity->Get("MANIFOLD_SOLID_BREP");
            CollectShellFaces(ctx, RefOf(args, 1), seenFaces, seenShells, 0);
        } else if (entity->Has("FACETED_BREP")) {
            const std::vector<Value>* args = entity->Get("FACETED_BREP");
            CollectShellFaces(ctx, RefOf(args, 1), seenFaces, seenShells, 0);
        } else if (entity->Has("BREP_WITH_VOIDS")) {
            const std::vector<Value>* args = entity->Get("BREP_WITH_VOIDS");
            CollectShellFaces(ctx, RefOf(args, 1), seenFaces, seenShells, 0);
            const Value* voids = At(args, 2);
            if (voids && voids->kind == Value::List) {
                for (const Value& ref : voids->items) {
                    if (ref.kind == Value::Ref) {
                        CollectShellFaces(ctx, ref.ref, seenFaces, seenShells, 1);
                    }
                }
            }
        }
    }

    if (seenFaces.empty()) {
        for (int id : shells) {
            const Entity* entity = doc.Find(id);
            if (!entity) continue;
            if (entity->Has("SHELL_BASED_SURFACE_MODEL")) {
                const std::vector<Value>* args = entity->Get("SHELL_BASED_SURFACE_MODEL");
                const Value* list = At(args, 1);
                if (list && list->kind == Value::List) {
                    for (const Value& ref : list->items) {
                        if (ref.kind == Value::Ref) {
                            CollectShellFaces(ctx, ref.ref, seenFaces, seenShells, 0);
                        }
                    }
                }
            } else {
                CollectShellFaces(ctx, id, seenFaces, seenShells, 0);
            }
        }
    }

    if (seenFaces.empty()) {
        for (const auto& entry : doc.entities) {
            const Entity& entity = entry.second;
            if (entity.Has("ADVANCED_FACE") || entity.Has("FACE_SURFACE")) {
                seenFaces.insert(entry.first);
            }
        }
    }

    faces.assign(seenFaces.begin(), seenFaces.end());
    std::sort(faces.begin(), faces.end());
}

double ReadLengthScale(const Doc& doc) {
    double scale = 1.0;
    for (const auto& entry : doc.entities) {
        const Entity& entity = entry.second;
        if (entity.Has("CONVERSION_BASED_UNIT")) {
            const std::vector<Value>* args = entity.Get("CONVERSION_BASED_UNIT");
            std::string name = TextOf(args, 0);
            for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (name == "INCH") return 0.0254;
            if (name == "FOOT") return 0.3048;
            if (name == "MILLIMETRE" || name == "MILLIMETER") return 0.001;
        }
        if (!entity.Has("SI_UNIT")) continue;
        const std::vector<Value>* args = entity.Get("SI_UNIT");
        const Value* unitName = At(args, 1);
        if (!unitName || unitName->kind != Value::Enum || unitName->text != "METRE") continue;
        const Value* prefix = At(args, 0);
        if (!prefix || prefix->kind != Value::Enum) {
            scale = 1.0;
            continue;
        }
        if (prefix->text == "MILLI") scale = 1.0e-3;
        else if (prefix->text == "CENTI") scale = 1.0e-2;
        else if (prefix->text == "DECI") scale = 1.0e-1;
        else if (prefix->text == "MICRO") scale = 1.0e-6;
        else if (prefix->text == "KILO") scale = 1.0e3;
        else scale = 1.0;
    }
    return scale;
}

void ConvertToWorld(Step::Mesh& mesh, double scale) {
    // CAD files are Z-up; the engine world is Y-up: (x, y, z) -> (x, z, -y).
    for (size_t i = 0; i < mesh.positions.size(); i += 3) {
        const double x = mesh.positions[i + 0] * scale;
        const double y = mesh.positions[i + 1] * scale;
        const double z = mesh.positions[i + 2] * scale;
        mesh.positions[i + 0] = static_cast<float>(x);
        mesh.positions[i + 1] = static_cast<float>(z);
        mesh.positions[i + 2] = static_cast<float>(-y);
    }
    for (size_t i = 0; i < mesh.normals.size(); i += 3) {
        const double x = mesh.normals[i + 0];
        const double y = mesh.normals[i + 1];
        const double z = mesh.normals[i + 2];
        mesh.normals[i + 0] = static_cast<float>(x);
        mesh.normals[i + 1] = static_cast<float>(z);
        mesh.normals[i + 2] = static_cast<float>(-y);
    }
    for (size_t i = 0; i + 2 < mesh.edges.size(); i += 3) {
        const double x = mesh.edges[i + 0] * scale;
        const double y = mesh.edges[i + 1] * scale;
        const double z = mesh.edges[i + 2] * scale;
        mesh.edges[i + 0] = static_cast<float>(x);
        mesh.edges[i + 1] = static_cast<float>(z);
        mesh.edges[i + 2] = static_cast<float>(-y);
    }
}

}  // namespace

namespace Step {

bool Tessellate(const std::string& text, Mesh& mesh, std::string& error) {
    mesh = Mesh();
    error.clear();

    if (text.empty()) {
        error = "The STEP file is empty.";
        return false;
    }
    if (text.size() > 512u * 1024u * 1024u) {
        error = "The STEP file is larger than 512 MB.";
        return false;
    }

    Doc doc;
    Parser parser(text);
    if (!parser.Parse(doc.entities, error)) return false;

    Ctx ctx{doc, mesh};
    std::vector<int> faces;
    CollectFaces(ctx, faces);
    if (faces.empty()) {
        error = "No B-rep faces were found in the STEP file.";
        return false;
    }

    for (int faceId : faces) {
        if (!TessellateFace(ctx, faceId)) ++mesh.skippedFaces;
        else ++mesh.faces;
    }

    if (mesh.indices.empty()) {
        error = "The STEP file contains no supported geometry (" +
                std::to_string(mesh.skippedFaces) + " face(s) skipped).";
        return false;
    }

    mesh.unitScale = ReadLengthScale(doc);
    ConvertToWorld(mesh, mesh.unitScale);

    if (mesh.skippedFaces > 0) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "%d face(s) skipped: surface type not supported by the STEP importer",
                      mesh.skippedFaces);
        mesh.warning = buffer;
    }
    return true;
}

}  // namespace Step
