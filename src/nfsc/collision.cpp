// Native replacement of the collision triangle gather WCollisionMgr::GetTriList (0x813290): the same double-precision
// steps and float stores as the translated x87 code, its leaf callees inlined, the list append inlined on its fast
// path. collision_native=0 runs the translated body instead.

#include "collision.h"
#include "shadow.h"

#include <native.h>
#include <platform/config.h>
#include <recomp.h>

#include <array>
#include <cstdint>
#include <vector>

namespace nfsc
{

// Translated body of the overridden guest function, and the translated list append it calls.
void sub_813290_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_812360(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

int s_native = -1;

bool useNative()
{
    if (s_native < 0)
        s_native = pf::Config::getBool("collision_native", true) ? 1 : 0;
    return s_native != 0;
}

// shadow_collision=1: each call runs the translated body, then the native one gathering its triangles into host
// memory, and compares them with the triangles the translated body appended.
shadow::Group s_shadow("shadow_collision");

// Read-only constants of the guest: 0, the closest point's length epsilon, the sphere radius unit.
constexpr float Zero = rc::f32(0x00000000u);
constexpr float Epsilon = rc::f32(0x358637bdu);
constexpr float RadiusUnit = rc::f32(0x3d800000u);

// A triangle of the list (0x30 bytes): three vertices (x, y, z, w), the first w the material, the second 0, the third
// the strip vertex's flags and the two guest frame bytes after them.
typedef std::array<x86::reg32, 12> Record;

struct Vertex
{
    float x, y, z;
};

x86::reg32 bits(float value)
{
    return __builtin_bit_cast(x86::reg32, value);
}

// Strip vertex index (int16 x, y, z, flags) scaled and moved to the origin (0x7e7c30, 0x7e7d40).
Vertex unpack(x86::reg32 strip, x86::reg32 index, double scale, const float origin[3])
{
    const x86::reg32 v = strip + index * 8;
    auto coordinate = [&](x86::reg32 k) {
        return float(double(int16_t(rc::ld16(v + 2 * k))) * scale + double(origin[k]));
    };
    return { coordinate(0), coordinate(1), coordinate(2) };
}

// Squared distance below r2 from (px, pz) to the closest point of the edge a-b as the guest computes it inline for
// the first edge: the x step stored as a float, the z step kept.
bool nearFirstEdge(double px, double pz, const Vertex& a, const Vertex& b, double r2)
{
    const double dz = double(b.z) - double(a.z);
    const float dx = float(double(b.x) - double(a.x));
    const double length2 = dz * dz + double(dx) * double(dx);
    double t = Zero;
    if (length2 > double(Epsilon))
        t = ((pz - double(a.z)) * dz + (px - double(a.x)) * double(dx)) / length2;
    const float cx = float(double(dx) * t + double(a.x));
    const float cz = float(dz * t + double(a.z));
    const double ex = px - double(cx), ez = pz - double(cz);
    return ex * ex + ez * ez < r2;
}

// The same for the edges the guest passes to 0x7eecb0: the z step stored as a float, the x step kept.
bool nearEdge(double px, double pz, const Vertex& a, const Vertex& b, double r2)
{
    const double dx = double(b.x) - double(a.x);
    const double dz = double(b.z) - double(a.z);
    const float dzf = float(dz);
    const double length2 = dz * dz + dx * dx;
    double t = Zero;
    if (length2 > double(Epsilon))
        t = ((pz - double(a.z)) * double(dzf) + (px - double(a.x)) * dx) / length2;
    const float cz = float(double(dzf) * t + double(a.z));
    const float cx = float(dx * t + double(a.x));
    const double ex = px - double(cx), ez = pz - double(cz);
    return ez * ez + ex * ex < r2;
}

// The triangle overlaps the circle of squared radius r2 around (px, pz), with the bounds of the query square.
bool touches(const Vertex& v0, const Vertex& v1, const Vertex& v2, double px, double pz, double r2, float lowX,
             float lowZ, float highX, float highZ)
{
    float minX = v2.x, minZ = v2.z, maxX = v2.x, maxZ = v2.z;
    if (v1.x < minX)
        minX = v1.x;
    if (v0.x < minX)
        minX = v0.x;
    if (v1.z < minZ)
        minZ = v1.z;
    if (v0.z < minZ)
        minZ = v0.z;
    if (v1.x > maxX)
        maxX = v1.x;
    if (v0.x > maxX)
        maxX = v0.x;
    if (v1.z > maxZ)
        maxZ = v1.z;
    if (v0.z > maxZ)
        maxZ = v0.z;
    if (lowX > maxX || lowZ > maxZ || highX < minX || highZ < minZ)
        return false;
    const double zero = Zero;
    const double dz1 = double(v1.z) - double(v0.z);
    const float dx1 = float(double(v1.x) - double(v0.x));
    const float area = float((double(v2.x) - double(v0.x)) * dz1 - (double(v2.z) - double(v0.z)) * double(dx1));
    // Each edge with the point outside or on it: the circle reaches the edge.
    const double s0 = ((pz - double(v1.z)) * (double(v0.x) - double(v1.x)) -
                       (double(v0.z) - double(v1.z)) * (px - double(v1.x))) * double(area);
    if (s0 <= zero && !nearFirstEdge(px, pz, v0, v1, r2))
        return false;
    const double s1 = ((pz - double(v2.z)) * (double(v1.x) - double(v2.x)) -
                       (double(v1.z) - double(v2.z)) * (px - double(v2.x))) * double(area);
    if (s1 <= zero && !nearEdge(px, pz, v1, v2, r2))
        return false;
    const double s2 = ((pz - double(v0.z)) * (double(v2.x) - double(v0.x)) -
                       (double(v2.z) - double(v0.z)) * (px - double(v0.x))) * double(area);
    if (s2 <= zero && !nearEdge(px, pz, v2, v0, r2))
        return false;
    return true;
}

// Appends a triangle to the list as 0x812360 does: in place while the current block has room, else through the
// guest function with the record in the guest frame. False when the guest call terminated.
bool append(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 list, x86::reg32 frame, const Record& record)
{
    const x86::reg32 block = rc::ld32(list + 0x10);
    if (block)
    {
        const x86::reg32 begin = rc::ld32(block), end = rc::ld32(block + 4), capacity = rc::ld32(block + 8);
        if (x86::sreg32(end - begin) / 0x30 != x86::sreg32(capacity - begin) / 0x30 && end < capacity)
        {
            rc::st32(block + 4, end + 0x30);
            if (end)
            {
                for (x86::reg32 k = 0; k < 12; ++k)
                    rc::st32(end + 4 * k, record[k]);
            }
            return true;
        }
    }
    for (x86::reg32 k = 0; k < 12; ++k)
        rc::st32(frame + 0x28 + 4 * k, record[k]);
    rc::st32(frame - 4, frame + 0x28);
    rc::st32(frame - 8, 0x81378a);
    cpu.ecx = list;
    cpu.esp = frame - 8;
    sub_812360(app, cpu);
    return !cpu.terminate;
}

// GetTriList (stdcall: instance list, position, radius, triangle list): for every instance with collision data, the
// bounding spheres within the radius of the position (in the instance's frame), then the triangles of their strips
// that reach the circle around the position in x/z. collected: gather into host memory instead of the list.
// Returns false when a guest call terminated.
bool gather(win32::WinApplication* app, x86::CPU& cpu, std::vector<Record>* collected)
{
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 frame = entry - 0xfc;
    const x86::reg32 instances = rc::ld32(entry + 4);
    const x86::reg32 position = rc::ld32(entry + 8);
    const float radius = rc::ldf32(entry + 0xc);
    const x86::reg32 list = rc::ld32(entry + 0x10);
    const double r = radius;
    const double r2 = float(r * r);
    const float px = rc::ldf32(position), py = rc::ldf32(position + 4), pz = rc::ldf32(position + 8);
    const float lowX = float(double(px) - r), lowZ = float(double(pz) - r);
    const float highX = float(r + double(px)), highZ = float(r + double(pz));
    const double scale = rc::ldf32(0xa7ad44);
    // The record's last two bytes: the guest frame's, which the guest never writes.
    const x86::reg32 tail = x86::reg32(rc::ld16(frame + 0x56)) << 16;
    x86::reg32 it = rc::ld32(instances);
    while (it != rc::ld32(instances + 4))
    {
        const x86::reg32 instance = rc::ld32(it);
        const x86::reg32 mesh = rc::ld32(instance + 0x1c);
        if (mesh)
        {
            // The position in the instance's frame (x and z of 0x4d9ba0), the up row from 0x44b5e0 when flagged.
            auto f = [instance](x86::reg32 offset) { return double(rc::ldf32(instance + offset)); };
            float upX = 0.0f, upZ = 0.0f;
            if (rc::ld8(instance + 0x12) & 3)
            {
                upX = float(f(8) * f(0x24) - f(4) * f(0x28));
                upZ = float(f(0x20) * f(4) - f(0) * f(0x24));
            }
            const float localX = float(((double(pz) * f(0x20) + double(py) * double(upX)) + double(px) * f(0)) +
                                       f(0x30));
            const float localZ = float(((double(pz) * f(0x28) + double(py) * double(upZ)) + double(px) * f(8)) +
                                       f(0x38));
            const x86::reg32 spheres = rc::ld16(mesh);
            for (x86::reg32 k = 0; k < spheres; ++k)
            {
                const x86::reg32 sphere = mesh + 0x10 + 16 * k;
                const float dx = float(double(rc::ldf32(sphere)) - double(localX));
                const float dz = float(double(rc::ldf32(sphere + 8)) - double(localZ));
                const double bound = double(rc::ld16(sphere + 0xc)) * double(RadiusUnit) + r;
                if (!(bound * bound > double(dz) * double(dz) + double(dx) * double(dx)))
                    continue;
                const float origin[3] = { float(double(rc::ldf32(sphere)) - f(0x30)),
                                          float(double(rc::ldf32(sphere + 4)) - f(0x34)),
                                          float(double(rc::ldf32(sphere + 8)) - f(0x38)) };
                const x86::reg32 strip = mesh + 0x10 + rc::ld16(sphere + 0xe);
                const x86::sreg32 count = x86::sreg32(rc::ld16(strip + 6)) - 2;
                if (count <= 0)
                    continue;
                Vertex v0 = unpack(strip, 0, scale, origin), v1 = unpack(strip, 1, scale, origin);
                Vertex v2 = unpack(strip, 2, scale, origin);
                x86::reg32 flags = rc::ld16(strip + 2 * 8 + 6);
                for (x86::sreg32 i = 1;; ++i)
                {
                    if (touches(v0, v1, v2, px, pz, r2, lowX, lowZ, highX, highZ))
                    {
                        const x86::reg32 material =
                            rc::ld32(mesh + rc::ld16(mesh + 2) + rc::ld16(mesh + 6) + (flags & 0xff) * 4 + 0x10);
                        const Record record = { bits(v0.x), bits(v0.y), bits(v0.z), material,
                                                bits(v1.x), bits(v1.y), bits(v1.z), 0,
                                                bits(v2.x), bits(v2.y), bits(v2.z), flags | tail };
                        if (collected)
                            collected->push_back(record);
                        else if (!append(app, cpu, list, frame, record))
                            return false;
                    }
                    if (i >= count)
                        break;
                    v0 = v1;
                    v1 = v2;
                    v2 = unpack(strip, x86::reg32(i) + 2, scale, origin);
                    flags = rc::ld16(strip + (x86::reg32(i) + 2) * 8 + 6);
                }
                app->allowPreemption(cpu);
            }
        }
        it += 4;
    }
    cpu.eax = it;
    cpu.esp = entry + 0x14;
    return true;
}

// The records appended to the list after the block index first and the record index at of its current block.
void appended(x86::reg32 list, x86::reg32 block, x86::reg32 at, x86::reg32 first, std::vector<Record>& out)
{
    auto take = [&out](x86::reg32 from, x86::reg32 to) {
        for (x86::reg32 p = from; p + 0x30 <= to; p += 0x30)
        {
            Record record;
            for (x86::reg32 k = 0; k < 12; ++k)
                record[k] = rc::ld32(p + 4 * k);
            out.push_back(record);
        }
    };
    if (block)
        take(rc::ld32(block) + at * 0x30, rc::ld32(block + 4));
    const x86::reg32 blocks = rc::ld32(list);
    const x86::reg32 count = (rc::ld32(list + 4) - blocks) / 4;
    for (x86::reg32 b = first; b < count; ++b)
    {
        const x86::reg32 added = rc::ld32(blocks + 4 * b);
        if (added && added != block)
            take(rc::ld32(added), rc::ld32(added + 4));
    }
}

// Runs the translated body, then the native gather into host memory, and compares the triangles and eax/esp.
void shadowCompare(win32::WinApplication* app, x86::CPU& cpu)
{
    const char* const name = "GetTriList 0x813290";
    const x86::reg32 list = rc::ld32(cpu.esp + 0x10);
    const x86::reg32 block = rc::ld32(list + 0x10);
    const x86::reg32 at = block ? (rc::ld32(block + 4) - rc::ld32(block)) / 0x30 : 0;
    const x86::reg32 first = (rc::ld32(list + 4) - rc::ld32(list)) / 4;
    x86::CPU probe = cpu;
    sub_813290_guest(app, cpu);
    if (cpu.terminate)
        return;
    std::vector<Record> translated, native;
    appended(list, block, at, first, translated);
    gather(app, probe, &native);
    int found = 0;
    if (probe.eax != cpu.eax)
        s_shadow.report(name, "eax", cpu.eax, probe.eax, found);
    if (probe.esp != cpu.esp)
        s_shadow.report(name, "esp", cpu.esp, probe.esp, found);
    if (translated.size() != native.size())
        s_shadow.report(name, "triangles", x86::reg32(translated.size()), x86::reg32(native.size()), found);
    auto isNan = [](x86::reg32 v) { return (v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu) != 0; };
    bool nanOnly = false, wordDiffers = false;
    for (size_t i = 0; i < translated.size() && i < native.size() && !wordDiffers; ++i)
    {
        for (x86::reg32 k = 0; k < 12; ++k)
        {
            const x86::reg32 t = translated[i][k], n = native[i][k];
            if (t == n)
                continue;
            if (isNan(t) && isNan(n))
            {
                nanOnly = true;
                continue;
            }
            s_shadow.report(name, k == 3 ? "triangle material" : "triangle word", t, n, found);
            wordDiffers = true;
            break;
        }
    }
    s_shadow.tally(found, nanOnly);
}

}

namespace collision
{

void logShadow()
{
    s_shadow.log();
}

}

namespace native
{

void sub_813290(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_shadow.active())
    {
        shadowCompare(app, cpu);
        return;
    }
    if (!useNative())
    {
        sub_813290_guest(app, cpu);
        return;
    }
    gather(app, cpu, nullptr);
}

}
}
