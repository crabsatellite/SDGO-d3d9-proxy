#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../mods/battle_ai/native_navigation.h"

namespace {

using battle_ai::Vec3;
using battle_ai::native::Aabb;
using battle_ai::native::NavigationApi;
using battle_ai::native::Ray3;

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
constexpr int kCapacity = 1024;

int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

template <typename T>
void Put(std::uint8_t* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

template <typename T>
void* Address(T method) {
    static_assert(sizeof(T) == sizeof(void*),
                  "fixture requires the Win32 single-inheritance member ABI");
    void* result = nullptr;
    std::memcpy(&result, &method, sizeof(result));
    return result;
}

struct SceneStorage {
    std::array<std::uint8_t, 0x180> bytes{};
    std::array<std::uint8_t, 0x108> obbs{};
    std::array<std::uint8_t, 0x40> spheres{};
    std::array<std::uint8_t, 0x10> object{};
};

struct Fixture;
Fixture* active = nullptr;

struct Callbacks {
    __declspec(noinline) void Query(void** output, int* count, int capacity,
                                    const Aabb*);
    __declspec(noinline) bool IsA(int kind);
    __declspec(noinline) bool TerrainRay(const Ray3*, float* distance);
};

struct Fixture {
    std::array<std::uint8_t, 0x8300> document{};
    std::array<std::uint8_t, 0x40> owner{};
    std::array<void*, 6> sceneVtable{};
    Callbacks quadtree{};
    std::array<SceneStorage, 4> scenes{};
    std::array<void*, kCapacity> results{};
    int reportedCount = 0;
    int queryCalls = 0;
    int segmentCalls = 0;
    int terrainCalls = 0;
    int obbCalls = 0;
    int sphereCalls = 0;
    std::uint8_t* terrainScene = nullptr;
    bool terrainHit = false;
    float terrainDistance = 0.0f;

    Fixture() {
        active = this;
        sceneVtable[5] = Address(&Callbacks::IsA);
        Put(document.data(), kDocumentQuadTree,
            static_cast<void*>(&quadtree));
        Put(owner.data(), kOwnerScene,
            static_cast<void*>(scenes[0].bytes.data()));
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            ConfigureScene(i, static_cast<std::uint32_t>(100 + i));
        }
    }

    void ConfigureScene(std::size_t index, std::uint32_t objectId) {
        auto& scene = scenes[index];
        scene.bytes.fill(0);
        scene.obbs.fill(0);
        scene.spheres.fill(0);
        scene.object.fill(0);
        Put(scene.bytes.data(), 0, sceneVtable.data());
        Put(scene.bytes.data(), kSceneBackPointer, scene.object.data());
        Put(scene.object.data(), kObjectId, objectId);
        Put(scene.bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {11.0f, 1.0f, 1.0f}});
        Put(scene.bytes.data(), kObbCount, 0);
        Put(scene.bytes.data(), kObbArray,
            static_cast<std::uint8_t*>(nullptr));
        Put(scene.bytes.data(), kSphereCount, 0);
        Put(scene.bytes.data(), kSphereArray,
            static_cast<std::uint8_t*>(nullptr));
        scene.bytes[kSphereEnabled] = 0;
    }

    void Results(std::initializer_list<std::size_t> indices) {
        results.fill(nullptr);
        reportedCount = 0;
        for (const std::size_t index : indices) {
            results[reportedCount++] = scenes[index].bytes.data();
        }
    }

    void Obb(std::size_t index, std::uint8_t code) {
        auto& scene = scenes[index];
        scene.obbs[0] = code;
        Put(scene.bytes.data(), kObbCount, 1);
        Put(scene.bytes.data(), kObbArray, scene.obbs.data());
    }

    void Sphere(std::size_t index, std::uint8_t code) {
        auto& scene = scenes[index];
        scene.spheres[0] = code;
        Put(scene.bytes.data(), kSphereCount, 1);
        Put(scene.bytes.data(), kSphereArray, scene.spheres.data());
        scene.bytes[kSphereEnabled] = 1;
    }

    NavigationApi Api();
};

void Callbacks::Query(void** output, int* count, int capacity, const Aabb*) {
    ++active->queryCalls;
    const int requested = active->reportedCount;
    const int copied = requested > 0
        ? (std::min)(requested, capacity)
        : 0;
    for (int i = 0; i < copied; ++i) output[i] = active->results[i];
    *count = requested;
}

bool Callbacks::IsA(int kind) {
    return kind == 3 && active->terrainScene ==
        reinterpret_cast<std::uint8_t*>(this);
}

bool Callbacks::TerrainRay(const Ray3*, float* distance) {
    ++active->terrainCalls;
    *distance = active->terrainDistance;
    return active->terrainHit;
}

__declspec(noinline) bool __cdecl SegmentAabb(const Vec3*, const Vec3*,
                                               const Aabb*) {
    ++active->segmentCalls;
    return true;
}

float DistanceFor(std::uint8_t code) {
    switch (code) {
        case 1: return 4.0f;
        case 2: return 11.0f;
        case 3: return std::numeric_limits<float>::quiet_NaN();
        default: return 0.0f;
    }
}

__declspec(noinline) bool __cdecl RayObb(const Ray3*, const void* shape,
                                         float* distance) {
    ++active->obbCalls;
    const auto code = *static_cast<const std::uint8_t*>(shape);
    *distance = DistanceFor(code);
    return code != 0;
}

__declspec(noinline) bool __cdecl RaySphere(const Ray3*, const void* shape,
                                            float* distance) {
    ++active->sphereCalls;
    const auto code = *static_cast<const std::uint8_t*>(shape);
    *distance = DistanceFor(code);
    return code != 0;
}

NavigationApi Fixture::Api() {
    NavigationApi api{};
    api.query = reinterpret_cast<battle_ai::native::QueryFn>(
        Address(&Callbacks::Query));
    api.segmentAabb = &SegmentAabb;
    api.terrainRay = reinterpret_cast<battle_ai::native::TerrainRayFn>(
        Address(&Callbacks::TerrainRay));
    api.rayObb = &RayObb;
    api.raySphere = &RaySphere;
    return api;
}

bool Clear(Fixture& fixture, std::uint32_t targetId = 999,
           Vec3 from = {0.0f, 0.0f, 0.0f},
           Vec3 to = {10.0f, 0.0f, 0.0f}) {
    active = &fixture;
    return battle_ai::native::FreshLineClear(
        fixture.Api(), fixture.document.data(), fixture.owner.data(),
        targetId, from, to);
}

void TestExcludedBodies() {
    Fixture fixture;
    fixture.Results({0, 1});
    fixture.Obb(0, 1);
    fixture.Obb(1, 1);
    Put(fixture.scenes[1].object.data(), kObjectId, std::uint32_t{42});
    Check(Clear(fixture, 42),
          "owner and selected-target collision bodies are excluded");
    Check(fixture.obbCalls == 0,
          "excluded bodies never reach primitive collision callbacks");
}

void TestPrimitiveBlockers() {
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.terrainScene = fixture.scenes[1].bytes.data();
        fixture.terrainHit = true;
        fixture.terrainDistance = 4.0f;
        Check(!Clear(fixture), "terrain hit within the segment blocks");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.Obb(1, 1);
        Check(!Clear(fixture), "OBB hit within the segment blocks");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.Sphere(1, 1);
        Check(!Clear(fixture), "sphere hit within the segment blocks");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.Obb(1, 2);
        fixture.Sphere(1, 2);
        Check(Clear(fixture),
              "primitive hits beyond the target endpoint do not block");
    }
}

void TestFreshDifferentTargets() {
    Fixture fixture;
    fixture.Results({1, 2});
    Put(fixture.scenes[1].object.data(), kObjectId, std::uint32_t{41});
    Put(fixture.scenes[2].object.data(), kObjectId, std::uint32_t{42});
    fixture.Obb(1, 1);
    fixture.Obb(2, 0);
    const bool target41 = Clear(fixture, 41);
    const bool target42 = Clear(fixture, 42);
    Check(target41 && !target42,
          "consecutive different-target queries produce independent results");
    Check(fixture.queryCalls == 2,
          "every line query invokes the quadtree without a two-second cache");
}

void TestCapacityAndMalformedData() {
    {
        Fixture fixture;
        fixture.reportedCount = kCapacity;
        fixture.results.fill(fixture.scenes[1].bytes.data());
        Check(!Clear(fixture),
              "a full 1024-result query is conservatively blocked");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        Put(fixture.scenes[1].bytes.data(), kObbCount, 1);
        Put(fixture.scenes[1].bytes.data(), kObbArray,
            static_cast<std::uint8_t*>(nullptr));
        Check(!Clear(fixture), "null OBB array with a positive count fails closed");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        Put(fixture.scenes[1].bytes.data(), kObbCount, 1);
        Put(fixture.scenes[1].bytes.data(), kObbArray,
            reinterpret_cast<std::uint8_t*>(1));
        Check(!Clear(fixture),
              "unreadable OBB storage is caught by the Win32 safety boundary");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        Put(fixture.scenes[1].bytes.data(), kObbCount, 4097);
        Put(fixture.scenes[1].bytes.data(), kObbArray,
            fixture.scenes[1].obbs.data());
        Check(!Clear(fixture), "excessive shape count fails closed");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.scenes[1].bytes[kSphereEnabled] = 1;
        Put(fixture.scenes[1].bytes.data(), kSphereCount, 1);
        Put(fixture.scenes[1].bytes.data(), kSphereArray,
            static_cast<std::uint8_t*>(nullptr));
        Check(!Clear(fixture),
              "null sphere array with a positive count fails closed");
    }
    {
        Fixture fixture;
        fixture.reportedCount = 1;
        fixture.results[0] = nullptr;
        Check(!Clear(fixture), "null query result fails closed");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Put(fixture.scenes[1].bytes.data(), kSceneAabb,
            Aabb{{nan, -1.0f, -1.0f}, {11.0f, 1.0f, 1.0f}});
        Check(!Clear(fixture), "NaN scene bounds fail closed");
    }
    {
        Fixture fixture;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Check(!Clear(fixture, 999, {nan, 0.0f, 0.0f},
                     {10.0f, 0.0f, 0.0f}),
              "NaN endpoints fail closed before the native query");
        Check(fixture.queryCalls == 0,
              "invalid endpoints do not reach native collision code");
    }
    {
        Fixture fixture;
        const float huge = (std::numeric_limits<float>::max)();
        Check(!Clear(fixture, 999, {huge, 0.0f, 0.0f},
                     {huge, 1.0f, 0.0f}),
              "overflowing segment midpoint fails closed");
    }
}

void TestInputsRemainUnchanged() {
    Fixture fixture;
    fixture.Results({1, 2, 3});
    fixture.terrainScene = fixture.scenes[1].bytes.data();
    fixture.Obb(2, 0);
    fixture.Sphere(3, 0);

    const auto document = fixture.document;
    const auto owner = fixture.owner;
    const auto scenes = fixture.scenes;
    Check(Clear(fixture), "mixed nonblocking scene query completes");
    bool sceneBytesEqual = true;
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        sceneBytesEqual =
            sceneBytesEqual &&
            scenes[i].bytes == fixture.scenes[i].bytes &&
            scenes[i].obbs == fixture.scenes[i].obbs &&
            scenes[i].spheres == fixture.scenes[i].spheres &&
            scenes[i].object == fixture.scenes[i].object;
    }
    Check(document == fixture.document && owner == fixture.owner &&
              sceneBytesEqual,
          "document, owner, scenes, objects, and shapes remain byte-identical");
}

}  // namespace

int main() {
    static_assert(sizeof(void*) == 4,
                  "GOnline navigation fixture must run as Win32");
    TestExcludedBodies();
    TestPrimitiveBlockers();
    TestFreshDifferentTargets();
    TestCapacityAndMalformedData();
    TestInputsRemainUnchanged();
    std::printf("NAVIGATION WRAPPER CHECKS %s\n",
                failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
