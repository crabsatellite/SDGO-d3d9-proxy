#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "combat_math.h"

namespace battle_ai::native {

// Layouts and function signatures are for the manifest-verified 32-bit
// GOnline build. Addresses are supplied by the core symbol resolver.
struct Ray3 {
    Vec3 origin;
    Vec3 direction;
};

struct Aabb {
    Vec3 min;
    Vec3 max;
};

using QueryFn =
    void(__thiscall*)(void* quadtree, void** scenes, int* count, int capacity, const Aabb* bounds);
using SegmentAabbFn =
    bool(__cdecl*)(const Vec3* segmentCenter, const Vec3* halfDelta, const Aabb* bounds);
using TerrainRayFn = bool(__thiscall*)(void* scene, const Ray3* ray, float* distance);
using RayObbFn = bool(__cdecl*)(const Ray3* ray, const void* obb, float* distance);
using RaySphereFn = bool(__cdecl*)(const Ray3* ray, const void* sphere, float* distance);

// RoadSearch returns native-allocated Vec3 storage. Initialize *count to zero,
// copy the required points, then release the returned pointer with NativeFreeFn.
using RouteSearchFn = Vec3*(__thiscall*)(void* navigation, std::uint32_t objectId, Vec3 start,
                                        Vec3 end, Vec3* unused, std::uint32_t* count);
using NativeFreeFn = void(__cdecl*)(void* allocation);

struct NavigationApi {
    QueryFn query = nullptr;
    SegmentAabbFn segmentAabb = nullptr;
    TerrainRayFn terrainRay = nullptr;
    RayObbFn rayObb = nullptr;
    RaySphereFn raySphere = nullptr;
};

static_assert(sizeof(Vec3) == 12, "GOnline Vec3 ABI changed");
static_assert(sizeof(Ray3) == 24, "GOnline Ray3 ABI changed");
static_assert(sizeof(Aabb) == 24, "GOnline AABB ABI changed");

namespace detail {

constexpr std::size_t kDocumentQuadTree = 0x82C4;
constexpr std::size_t kOwnerScene = 0x34;
constexpr std::size_t kSceneBackPointer = 0x0C;
constexpr std::size_t kObjectId = 0x04;
constexpr std::size_t kSceneAabb = 0xBC;
constexpr std::size_t kObbCount = 0xD4;
constexpr std::size_t kObbArray = 0xD8;
constexpr std::size_t kSphereCount = 0xDC;
constexpr std::size_t kSphereArray = 0xE0;
constexpr std::size_t kSphereEnabled = 0x115;
constexpr std::size_t kObbStride = 0x84;
constexpr std::size_t kSphereStride = 0x20;
constexpr int kSceneCapacity = 1024;
constexpr int kShapeCountLimit = 4096;
constexpr double kLengthEpsilon = 1.0e-6;
constexpr double kEndpointEpsilon = 1.0e-3;

using SceneIsAFn = bool(__thiscall*)(void* scene, int kind);

inline bool ValidShapeArray(const std::uint8_t* array, int count, std::size_t stride) noexcept {
    if (count < 0 || count > kShapeCountLimit) return false;
    if (count == 0) return true;
    if (!array) return false;

    const std::size_t last = static_cast<std::size_t>(count - 1);
    if (last > (static_cast<std::size_t>(UINTPTR_MAX) - stride) / stride) return false;
    const std::size_t extent = last * stride + stride;
    return reinterpret_cast<std::uintptr_t>(array) <= UINTPTR_MAX - extent;
}

inline bool BlockingHit(bool hit, float distance, double length) noexcept {
    if (!hit) return false;
    if (!std::isfinite(distance)) return true;
    return distance >= 0.0f &&
           static_cast<double>(distance) < length - kEndpointEpsilon;
}

inline bool ValidAabb(const Aabb& bounds) noexcept {
    return Finite(bounds.min) && Finite(bounds.max) &&
           bounds.min.x <= bounds.max.x &&
           bounds.min.y <= bounds.max.y &&
           bounds.min.z <= bounds.max.z;
}

inline bool FreshLineClearUnsafe(const NavigationApi& api, std::uint8_t* document,
                                 std::uint8_t* owner, std::uint32_t targetId,
                                 Vec3 from, Vec3 to) noexcept {
    if (!api.query || !api.segmentAabb || !api.terrainRay || !api.rayObb ||
        !api.raySphere || !document || !owner || targetId == 0 ||
        !Finite(from) || !Finite(to)) {
        return false;
    }

    auto* quadtree = *reinterpret_cast<void**>(document + kDocumentQuadTree);
    auto* ownerScene = *reinterpret_cast<void**>(owner + kOwnerScene);
    if (!quadtree || !ownerScene) return false;

    const Vec3 delta = to - from;
    const double length = Length(delta);
    if (!std::isfinite(length)) return false;
    if (length <= kLengthEpsilon) return true;

    const Ray3 ray = {from, delta * static_cast<float>(1.0 / length)};
    if (!Finite(ray.direction)) return false;

    const Vec3 center = (from + to) * 0.5f;
    const Vec3 halfDelta = delta * 0.5f;
    if (!Finite(center) || !Finite(halfDelta)) return false;
    const Aabb bounds = {
        {from.x < to.x ? from.x : to.x,
         from.y < to.y ? from.y : to.y,
         from.z < to.z ? from.z : to.z},
        {from.x > to.x ? from.x : to.x,
         from.y > to.y ? from.y : to.y,
         from.z > to.z ? from.z : to.z}
    };

    // Keep query storage private to this call. The underlying quadtree and
    // scene graph are not synchronized for background access: invoke this only
    // serially from the game's update thread.
    void* scenes[kSceneCapacity] = {};
    int sceneCount = 0;
    api.query(quadtree, scenes, &sceneCount, kSceneCapacity, &bounds);
    if (sceneCount < 0 || sceneCount >= kSceneCapacity) return false;

    for (int sceneIndex = 0; sceneIndex < sceneCount; ++sceneIndex) {
        auto* scene = static_cast<std::uint8_t*>(scenes[sceneIndex]);
        if (!scene) return false;
        if (scene == ownerScene) continue;

        auto* object = *reinterpret_cast<std::uint8_t**>(scene + kSceneBackPointer);
        if (object && *reinterpret_cast<std::uint32_t*>(object + kObjectId) == targetId) {
            continue;
        }

        const auto* sceneBounds = reinterpret_cast<const Aabb*>(scene + kSceneAabb);
        if (!ValidAabb(*sceneBounds)) return false;
        if (!api.segmentAabb(&center, &halfDelta, sceneBounds)) continue;

        auto** vtable = *reinterpret_cast<void***>(scene);
        if (!vtable || !vtable[5]) return false;
        const auto sceneIsA = reinterpret_cast<SceneIsAFn>(vtable[5]);

        float distance = 0.0f;
        if (sceneIsA(scene, 3)) {
            const bool hit = api.terrainRay(scene, &ray, &distance);
            if (BlockingHit(hit, distance, length)) {
                return false;
            }
            continue;
        }

        const int obbCount = *reinterpret_cast<const int*>(scene + kObbCount);
        auto* obbs = *reinterpret_cast<std::uint8_t**>(scene + kObbArray);
        if (!ValidShapeArray(obbs, obbCount, kObbStride)) return false;
        for (int obbIndex = 0; obbIndex < obbCount; ++obbIndex) {
            distance = 0.0f;
            const bool hit =
                api.rayObb(&ray, obbs + static_cast<std::size_t>(obbIndex) * kObbStride,
                           &distance);
            if (BlockingHit(hit, distance, length)) {
                return false;
            }
        }

        int sphereCount = 0;
        std::uint8_t* spheres = nullptr;
        if (*(scene + kSphereEnabled) != 0) {
            sphereCount = *reinterpret_cast<const int*>(scene + kSphereCount);
            spheres = *reinterpret_cast<std::uint8_t**>(scene + kSphereArray);
        }
        if (!ValidShapeArray(spheres, sphereCount, kSphereStride)) return false;
        for (int sphereIndex = 0; sphereIndex < sphereCount; ++sphereIndex) {
            distance = 0.0f;
            const bool hit =
                api.raySphere(&ray,
                              spheres + static_cast<std::size_t>(sphereIndex) * kSphereStride,
                              &distance);
            if (BlockingHit(hit, distance, length)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace detail

inline bool FreshLineClear(const NavigationApi& api, std::uint8_t* document,
                           std::uint8_t* owner, std::uint32_t targetId,
                           Vec3 from, Vec3 to) noexcept {
#if defined(_MSC_VER) && defined(_WIN32)
    // Keep the SEH frame free of objects with non-trivial destructors.
    __try {
        return detail::FreshLineClearUnsafe(api, document, owner, targetId, from, to);
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
#else
    return detail::FreshLineClearUnsafe(api, document, owner, targetId, from, to);
#endif
}

} // namespace battle_ai::native
