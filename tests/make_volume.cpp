// Writes the grids of tests/scenes/volume.vdb (volume.usda, test_volume.py):
//   density      a cloud: a ball of radius 1.6 with fractal noise eating
//                into its edge, density up to 2 per unit;
//   temperature  where the cloud is, up to 0.5 at its bottom (normalized,
//                1 ~ 5000 K, as Houdini's pyro: 2500 K);
//   density      again (fieldIndex 1): a ball of radius 0.5, density 4.
// Voxels 0.05 apart, centred on the origin. Built with volumes (OpenVDB);
// run: make_volume tests\scenes\volume.vdb
// make_volume --sequence <dir> <frames> instead writes <dir>/seq_0000.vdb ...:
// a density ball per frame, growing (a new bbox and size each frame), for
// test_volume_sequence.py.
#include <openvdb/openvdb.h>
#include <openvdb/tools/Prune.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr double kVoxel = 0.05;

// Value noise in [0, 1] on the integer lattice, smoothly interpolated.
double Hash(int x, int y, int z)
{
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return double(h & 0xFFFFFF) / double(0xFFFFFF);
}

double Noise(double x, double y, double z)
{
    const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    auto s = [](double t) { return t * t * (3 - 2 * t); };
    const double fx = s(x - ix), fy = s(y - iy), fz = s(z - iz);
    double v = 0;
    for (int k = 0; k < 8; ++k) {
        const int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
        v += Hash(ix + dx, iy + dy, iz + dz) * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
    }
    return v;
}

double Fractal(double x, double y, double z)
{
    double v = 0, amplitude = 0.5;
    for (int octave = 0; octave < 4; ++octave, x *= 2, y *= 2, z *= 2, amplitude *= 0.5) {
        v += amplitude * Noise(x, y, z);
    }
    return v;  // ~[0, 0.94]
}

openvdb::FloatGrid::Ptr Grid(const char *name)
{
    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.0f);
    grid->setName(name);
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(kVoxel));
    grid->setSaveFloatAsHalf(true);
    return grid;
}

// Frame f of the sequence: a ball of radius 0.3 + 0.1 f, density 3.
void WriteSequence(const std::string &dir, int frames)
{
    for (int f = 0; f < frames; ++f) {
        openvdb::FloatGrid::Ptr ball = Grid("density");
        auto b = ball->getAccessor();
        const double radius = 0.3 + 0.1 * f;
        const int n = int(radius / kVoxel) + 1;
        for (int i = -n; i <= n; ++i) {
            for (int j = -n; j <= n; ++j) {
                for (int k = -n; k <= n; ++k) {
                    if (std::sqrt(double(i * i + j * j + k * k)) * kVoxel <= radius) {
                        b.setValue({i, j, k}, 3.0f);
                    }
                }
            }
        }
        char name[32];
        std::snprintf(name, sizeof(name), "/seq_%04d.vdb", f);
        openvdb::io::File file(dir + name);
        file.write({ball});
        file.close();
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--sequence") {
        openvdb::initialize();
        WriteSequence(argv[2], std::atoi(argv[3]));
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_volume out.vdb\n");
        return 2;
    }
    openvdb::initialize();
    openvdb::FloatGrid::Ptr cloud = Grid("density"), temperature = Grid("temperature"), ball = Grid("density");
    auto c = cloud->getAccessor(), t = temperature->getAccessor(), b = ball->getAccessor();
    for (int i = -40; i <= 40; ++i) {
        for (int j = -40; j <= 40; ++j) {
            for (int k = -40; k <= 40; ++k) {
                const double x = i * kVoxel, y = j * kVoxel, z = k * kVoxel, r = std::sqrt(x * x + y * y + z * z);
                const double edge = 1.6 - r - 0.9 * (Fractal(x * 1.7 + 3, y * 1.7, z * 1.7) - 0.45);
                const float d = float(std::fmin(std::fmax(edge * 5.0, 0.0), 1.0) * 2.0);
                if (d > 0.0f) {
                    c.setValue({i, j, k}, d);
                    t.setValue({i, j, k}, float(std::fmax(0.0, (-0.4 - y) / 1.2) * 0.5));
                }
                if (r <= 0.5) {
                    b.setValue({i, j, k}, 4.0f);
                }
            }
        }
    }
    for (const auto &grid : {cloud, temperature, ball}) {
        openvdb::tools::prune(grid->tree());
    }
    openvdb::io::File file(argv[1]);
    file.write({cloud, temperature, ball});
    file.close();
    std::printf("%s: %zu, %zu and %zu active voxels\n", argv[1], size_t(cloud->activeVoxelCount()),
                size_t(temperature->activeVoxelCount()), size_t(ball->activeVoxelCount()));
    return 0;
}
