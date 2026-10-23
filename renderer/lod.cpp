#include "lod.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace vkr {

namespace {

// Area-weighted sum of plane quadrics (Garland & Heckbert) and its weight:
// Raw / weight is the weighted mean squared distance to the planes.
struct Quadric {
    double q[10] = {};  // xx xy xz xw yy yz yw zz zw ww
    double weight = 0;

    void AddPlane(const double n[3], double d, double w)
    {
        const double p[4] = {n[0], n[1], n[2], d};
        int k = 0;
        for (int i = 0; i < 4; ++i) {
            for (int j = i; j < 4; ++j) {
                q[k++] += w * p[i] * p[j];
            }
        }
        weight += w;
    }
    void Add(const Quadric &o)
    {
        for (int i = 0; i < 10; ++i) {
            q[i] += o.q[i];
        }
        weight += o.weight;
    }
    double Raw(const float *p) const
    {
        const double x = p[0], y = p[1], z = p[2];
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y +
               2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
    }
};

void Normal(const float *a, const float *b, const float *c, double n[3])
{
    const double u[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
    const double v[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
}

// A collapse of vertex `from` onto its neighbour `to`; valid while
// `from`'s stamp is unchanged.
struct Candidate {
    double cost;
    uint32_t from, to, stamp;
    bool operator<(const Candidate &o) const { return cost > o.cost; }  // cheapest first
};

} // namespace

std::vector<LodLevel> BuildLods(const float *positions, size_t stride, size_t vertexCount,
                                const std::vector<uint32_t> &indices, uint32_t maxLevels, float maxError,
                                const std::function<float(uint32_t, uint32_t)> &difference)
{
    std::vector<LodLevel> levels;
    const size_t triangles = indices.size() / 3;
    if (triangles < 4 || maxLevels == 0) {
        return levels;
    }
    auto P = [&](uint32_t v) { return positions + size_t(v) * stride; };
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    for (const uint32_t v : indices) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], P(v)[k]);
            hi[k] = std::max(hi[k], P(v)[k]);
        }
    }
    const double radius = 0.5 * std::sqrt(double(hi[0] - lo[0]) * (hi[0] - lo[0]) +
                                          double(hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                          double(hi[2] - lo[2]) * (hi[2] - lo[2]));
    if (!(radius > 0)) {
        return levels;
    }
    const double maxCost = (maxError * radius) * (maxError * radius);
    const double differenceCost = 0.0625 * radius * radius;  // a difference of 1: a quarter radius

    std::vector<uint32_t> tri(indices);
    std::vector<uint8_t> alive(triangles, 1);
    std::vector<std::vector<uint32_t>> vertexTris(vertexCount);
    std::vector<Quadric> quadric(vertexCount);
    std::vector<uint64_t> edges;  // (smaller vertex, larger), once per triangle using it
    edges.reserve(triangles * 3);
    for (size_t t = 0; t < triangles; ++t) {
        const uint32_t *v = &tri[t * 3];
        double n[3];
        Normal(P(v[0]), P(v[1]), P(v[2]), n);
        const double area2 = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (area2 > 0) {
            for (double &c : n) {
                c /= area2;
            }
            const double d = -(n[0] * P(v[0])[0] + n[1] * P(v[0])[1] + n[2] * P(v[0])[2]);
            for (int k = 0; k < 3; ++k) {
                quadric[v[k]].AddPlane(n, d, 0.5 * area2);
            }
        }
        for (int k = 0; k < 3; ++k) {
            vertexTris[v[k]].push_back(uint32_t(t));
            const uint32_t a = v[k], b = v[(k + 1) % 3];
            edges.push_back(uint64_t(std::min(a, b)) << 32 | std::max(a, b));
        }
    }
    // Border and seam vertices stay where they are (see lod.h).
    std::vector<uint8_t> locked(vertexCount, 0);
    std::sort(edges.begin(), edges.end());
    for (size_t i = 0, j = 0; i < edges.size(); i = j) {
        while (j < edges.size() && edges[j] == edges[i]) {
            ++j;
        }
        if (j - i != 2) {
            locked[uint32_t(edges[i] >> 32)] = locked[uint32_t(edges[i])] = 1;
        }
    }
    edges = std::vector<uint64_t>();

    std::vector<uint8_t> removed(vertexCount, 0);
    std::vector<uint32_t> stamp(vertexCount, 0);
    // Each vertex's quadric at its own position: a collapse onto it costs
    // (Q_from(p_to) + this) / both weights.
    std::vector<double> self(vertexCount);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        self[v] = quadric[v].Raw(P(v));
    }
    std::priority_queue<Candidate> heap;
    // The cheapest collapse of `from` onto a neighbour, queued.
    auto queue = [&](uint32_t from) {
        ++stamp[from];
        if (locked[from] || removed[from]) {
            return;
        }
        Candidate best{std::numeric_limits<double>::max(), from, from, stamp[from]};
        for (const uint32_t t : vertexTris[from]) {
            if (!alive[t]) {
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const uint32_t to = tri[t * 3 + k];
                if (to == from) {
                    continue;
                }
                const double weight = quadric[from].weight + quadric[to].weight;
                double cost = weight > 0 ? std::max(quadric[from].Raw(P(to)) + self[to], 0.0) / weight : 0.0;
                if (difference) {
                    const double d = difference(from, to);
                    cost += d * d * differenceCost;
                }
                if (cost < best.cost) {
                    best.cost = cost;
                    best.to = to;
                }
            }
        }
        if (best.to != from) {
            heap.push(best);
        }
    };
    for (uint32_t v = 0; v < vertexCount; ++v) {
        if (!vertexTris[v].empty()) {
            queue(v);
        }
    }

    // Moving `from` onto `to` must not flip or squash its other triangles.
    auto valid = [&](uint32_t from, uint32_t to) {
        for (const uint32_t t : vertexTris[from]) {
            const uint32_t *v = &tri[t * 3];
            if (!alive[t] || v[0] == to || v[1] == to || v[2] == to) {
                continue;
            }
            double before[3], after[3];
            Normal(P(v[0]), P(v[1]), P(v[2]), before);
            Normal(P(v[0] == from ? to : v[0]), P(v[1] == from ? to : v[1]), P(v[2] == from ? to : v[2]), after);
            const double dot = before[0] * after[0] + before[1] * after[1] + before[2] * after[2];
            const double lb = std::sqrt(before[0] * before[0] + before[1] * before[1] + before[2] * before[2]);
            const double la = std::sqrt(after[0] * after[0] + after[1] * after[1] + after[2] * after[2]);
            if (!(dot > 0.2 * lb * la) || la < 1e-3 * lb) {
                return false;
            }
        }
        return true;
    };

    size_t live = triangles, target = triangles / 2, previous = triangles;
    std::vector<uint32_t> neighbours;
    double worst = 0;  // the largest collapse cost so far
    while (levels.size() < maxLevels) {
        bool done = heap.empty();
        while (live > target && !heap.empty()) {
            const Candidate c = heap.top();
            if (c.cost > maxCost) {
                done = true;
                break;
            }
            heap.pop();
            if (c.stamp != stamp[c.from] || removed[c.to] || !valid(c.from, c.to)) {
                continue;  // stale; a rejected one comes back when its neighbourhood changes
            }
            for (const uint32_t t : vertexTris[c.from]) {
                if (!alive[t]) {
                    continue;
                }
                uint32_t *v = &tri[t * 3];
                if (v[0] == c.to || v[1] == c.to || v[2] == c.to) {
                    alive[t] = 0;
                    --live;
                } else {
                    std::replace(v, v + 3, c.from, c.to);
                    vertexTris[c.to].push_back(t);
                }
            }
            removed[c.from] = 1;
            vertexTris[c.from] = std::vector<uint32_t>();
            quadric[c.to].Add(quadric[c.from]);
            self[c.to] = quadric[c.to].Raw(P(c.to));
            worst = std::max(worst, c.cost);
            // Everything around `to` has a new neighbourhood.
            std::vector<uint32_t> &around = vertexTris[c.to];
            around.erase(std::remove_if(around.begin(), around.end(), [&](uint32_t t) { return !alive[t]; }),
                         around.end());
            neighbours.clear();
            for (const uint32_t t : around) {
                neighbours.insert(neighbours.end(), &tri[t * 3], &tri[t * 3] + 3);
            }
            std::sort(neighbours.begin(), neighbours.end());
            neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());
            for (const uint32_t v : neighbours) {
                queue(v);
            }
        }
        done = done || heap.empty();
        // A level where it halved; at the end, the last one if it gained a quarter.
        if (live <= target || (done && live * 4 <= previous * 3)) {
            LodLevel level;
            level.indices.reserve(live * 3);
            for (size_t t = 0; t < triangles; ++t) {
                if (alive[t]) {
                    level.indices.insert(level.indices.end(), &tri[t * 3], &tri[t * 3] + 3);
                }
            }
            level.error = float(std::sqrt(worst) / radius);
            levels.push_back(std::move(level));
            previous = live;
            target = live / 2;
        }
        if (done || live < 4) {
            break;
        }
    }
    return levels;
}

} // namespace vkr
