// A box cut into pieces (ADR 0215): the pieces fill the box and nothing
// else, every one is held to the others, and a seed is a cut -- the same on
// every machine.

#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/scene/fracture.h"

using namespace engine;

namespace {

[[nodiscard]] bool inside(const scene::FracturePiece& piece, core::Vec3 box)
{
    const auto within = [](core::f32 center, core::f32 half, core::f32 bound) {
        return center - half >= -bound * 0.5f - 1.0e-4f && center + half <= bound * 0.5f + 1.0e-4f;
    };
    return within(piece.center.x, piece.size.x * 0.5f, box.x) && within(piece.center.y, piece.size.y * 0.5f, box.y) &&
           within(piece.center.z, piece.size.z * 0.5f, box.z);
}

} // namespace

TEST_CASE("ADR 0215: a box cut into pieces is filled by them, and every piece is held to the rest")
{
    const core::Vec3 wall{6.0f, 3.0f, 0.4f};
    for (const core::u32 wanted : {1u, 2u, 8u, 50u, 64u}) {
        const scene::FractureCut cut = scene::cutBox(wall, wanted, 7);
        REQUIRE(cut.pieces.size() == wanted);
        double volume = 0.0;
        for (const scene::FracturePiece& piece : cut.pieces) {
            CHECK(inside(piece, wall));
            CHECK(piece.size.x > 0.0f);
            CHECK(piece.size.y > 0.0f);
            CHECK(piece.size.z > 0.0f);
            volume += static_cast<double>(piece.size.x) * static_cast<double>(piece.size.y) *
                      static_cast<double>(piece.size.z);
        }
        CHECK(volume ==
              doctest::Approx(static_cast<double>(wall.x) * static_cast<double>(wall.y) * static_cast<double>(wall.z))
                  .epsilon(1.0e-4));

        // One whole: every piece reached from the first through the joints.
        std::vector<bool> reached(cut.pieces.size(), false);
        reached[0] = true;
        for (bool grew = true; grew;) {
            grew = false;
            for (const scene::FractureJoint& joint : cut.joints) {
                if (reached[joint.first] != reached[joint.second]) {
                    reached[joint.first] = reached[joint.second] = true;
                    grew = true;
                }
            }
        }
        for (const bool one : reached)
            CHECK(one);
    }
}

TEST_CASE("ADR 0215: a seed is a cut, and another seed is another")
{
    const core::Vec3 box{2.0f, 2.0f, 2.0f};
    const scene::FractureCut a = scene::cutBox(box, 12, 42);
    const scene::FractureCut b = scene::cutBox(box, 12, 42);
    const scene::FractureCut c = scene::cutBox(box, 12, 43);
    REQUIRE(a.pieces.size() == b.pieces.size());
    bool differs = false;
    for (std::size_t index = 0; index < a.pieces.size(); ++index) {
        CHECK(a.pieces[index].center.x == b.pieces[index].center.x);
        CHECK(a.pieces[index].size.y == b.pieces[index].size.y);
        differs = differs || a.pieces[index].size.x != c.pieces[index].size.x ||
                  a.pieces[index].size.y != c.pieces[index].size.y || a.pieces[index].size.z != c.pieces[index].size.z;
    }
    CHECK(differs);
    CHECK(a.joints.size() == b.joints.size());
    // Held to the most a cut makes.
    CHECK(scene::cutBox(box, 1000, 1).pieces.size() == scene::MaxFracturePieces);
    CHECK(scene::cutBox(box, 0, 1).pieces.size() == 1);
}
