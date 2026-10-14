// A vehicle (ADR 0213): a chassis held up by four cast wheels on a floor. What
// is asked of it is what a driver sees -- it rests on its springs, goes where
// the throttle sends it, stops on the brake, turns with the wheel -- and what
// the engine needs: the same inputs are the same drive, and a restored world
// drives on as it would have.

#include <cmath>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

#include "engine/physics/backends.h"

using namespace engine;
using namespace engine::physics;

namespace {

constexpr f32 kFixedDt = 1.0f / 60.0f;

struct Road
{
    Road()
    {
        physics = createJoltPhysics();
        REQUIRE(physics != nullptr);
        world = physics->createWorld(WorldDesc{});
        REQUIRE(world.valid());
        BodyDesc floor;
        floor.shape.type = ShapeType::Box;
        floor.shape.size = core::Vec3{400.0f, 1.0f, 400.0f};
        floor.transform.position = core::DVec3{0.0, -0.5, 0.0};
        floor.motion = MotionType::Static;
        floor.friction = 1.0f;
        REQUIRE(physics->createBody(world, floor).valid());

        // A box of a car, about 1 200 kg, its wheels at its corners.
        BodyDesc body;
        body.shape.type = ShapeType::Box;
        body.shape.size = core::Vec3{1.8f, 0.6f, 4.0f};
        body.transform.position = core::DVec3{0.0, 1.0, 0.0};
        body.motion = MotionType::Dynamic;
        body.density = 280.0f;
        chassis = physics->createBody(world, body);
        REQUIRE(chassis.valid());

        VehicleDesc desc;
        desc.chassis = chassis;
        desc.maxTorque = 500.0f;
        for (const f32 z : {-1.4f, 1.4f}) {
            for (const f32 x : {-0.9f, 0.9f}) {
                VehicleWheelDesc wheel;
                wheel.position = core::Vec3{x, -0.2f, z};
                wheel.radius = 0.35f;
                wheel.width = 0.25f;
                // Front (toward -Z) steers; the rear drives.
                wheel.maxSteerAngle = z < 0.0f ? 0.5f : 0.0f;
                wheel.driven = z > 0.0f;
                wheel.maxHandBrakeTorque = z > 0.0f ? 4000.0f : 0.0f;
                desc.wheels.push_back(wheel);
            }
        }
        vehicle = physics->createVehicle(world, desc);
        REQUIRE(vehicle.valid());
    }

    ~Road() { physics->destroyWorld(world); }

    Road(const Road&) = delete;
    Road& operator=(const Road&) = delete;

    void drive(const VehicleInput& input, int ticks)
    {
        for (int tick = 0; tick < ticks; ++tick) {
            physics->setVehicleInput(world, vehicle, input);
            physics->step(world, kFixedDt);
        }
    }

    [[nodiscard]] BodyState body() const { return physics->bodyState(world, chassis); }
    [[nodiscard]] f64 speed() const
    {
        const core::Vec3 velocity = body().linearVelocity;
        return std::sqrt(static_cast<f64>(velocity.x * velocity.x + velocity.z * velocity.z));
    }

    std::unique_ptr<IPhysics3D> physics;
    WorldHandle world;
    BodyHandle chassis;
    VehicleHandle vehicle;
};

} // namespace

TEST_CASE("ADR 0213: a vehicle rests on its wheels, drives on the throttle, stops on the brake and turns")
{
    Road road;
    // **At rest**: all four wheels on the floor, the chassis held above it by
    // its springs, and not moving.
    road.drive(VehicleInput{}, 120);
    VehicleState state = road.physics->vehicleState(road.world, road.vehicle);
    REQUIRE(state.wheels.size() == 4);
    for (const VehicleWheelState& wheel : state.wheels) {
        CHECK(wheel.grounded);
        CHECK(wheel.suspensionLength > 0.15f);
        CHECK(wheel.suspensionLength < 0.4f);
    }
    const core::DVec3 rest = road.body().transform.position;
    CHECK(rest.y > 0.5);
    CHECK(road.speed() < 0.05);

    // **Forward is -Z**, as a part's LookVector: five seconds of throttle
    // take it some tens of meters that way, the engine turning and in gear.
    road.drive(VehicleInput{.forward = 1.0f}, 300);
    state = road.physics->vehicleState(road.world, road.vehicle);
    const core::DVec3 driven = road.body().transform.position;
    CHECK(driven.z < rest.z - 15.0);
    CHECK(std::abs(driven.x - rest.x) < 1.0);
    CHECK(road.speed() > 5.0);
    CHECK(state.rpm > 1000.0f);
    CHECK(state.gear >= 1);
    // The driven wheels turn the way the car goes.
    CHECK(state.wheels[2].angularVelocity != 0.0f);

    // **The brake stops it.**
    road.drive(VehicleInput{.brake = 1.0f}, 240);
    CHECK(road.speed() < 0.2);

    // **And the wheel turns it**: right, at a walk, and the car is to the
    // right of where it would have gone straight.
    const core::DVec3 before = road.body().transform.position;
    road.drive(VehicleInput{.forward = 0.6f, .right = 1.0f}, 240);
    state = road.physics->vehicleState(road.world, road.vehicle);
    CHECK(state.wheels[0].steerAngle != 0.0f);
    CHECK(road.body().transform.position.x > before.x + 1.0);
}

TEST_CASE("ADR 0213: the same inputs are the same drive, and a restored world drives on as it would have")
{
    const auto run = [](Road& road) {
        road.drive(VehicleInput{}, 30);
        road.drive(VehicleInput{.forward = 1.0f, .right = 0.3f}, 120);
        road.drive(VehicleInput{.forward = 0.4f, .right = -0.6f, .handBrake = 0.5f}, 90);
    };
    Road first;
    Road second;
    run(first);
    run(second);
    const core::CFrameD a = first.body().transform;
    const core::CFrameD b = second.body().transform;
    CHECK(a.position.x == b.position.x);
    CHECK(a.position.y == b.position.y);
    CHECK(a.position.z == b.position.z);

    // Saved, driven on, put back and driven the same: the same place.
    std::vector<u8> saved;
    REQUIRE(first.physics->saveState(first.world, saved));
    first.drive(VehicleInput{.forward = 1.0f, .right = -0.4f}, 120);
    const core::DVec3 once = first.body().transform.position;
    REQUIRE(first.physics->restoreState(first.world, saved));
    first.drive(VehicleInput{.forward = 1.0f, .right = -0.4f}, 120);
    const core::DVec3 again = first.body().transform.position;
    CHECK(once.x == again.x);
    CHECK(once.y == again.y);
    CHECK(once.z == again.z);
}

TEST_CASE("ADR 0213: a vehicle needs a moving chassis and a wheel, and goes with its chassis")
{
    Road road;
    VehicleDesc none;
    none.chassis = road.chassis;
    CHECK_FALSE(road.physics->createVehicle(road.world, none).valid());

    BodyDesc fixed;
    fixed.shape.type = ShapeType::Box;
    fixed.shape.size = core::Vec3{1.0f, 1.0f, 1.0f};
    fixed.motion = MotionType::Static;
    VehicleDesc onStatic;
    onStatic.chassis = road.physics->createBody(road.world, fixed);
    onStatic.wheels.emplace_back();
    CHECK_FALSE(road.physics->createVehicle(road.world, onStatic).valid());

    // The chassis gone, so is its vehicle: asked of, it answers nothing.
    road.physics->destroyBody(road.world, road.chassis);
    CHECK(road.physics->vehicleState(road.world, road.vehicle).wheels.empty());
    road.physics->setVehicleInput(road.world, road.vehicle, VehicleInput{.forward = 1.0f});
    road.physics->step(road.world, kFixedDt);
}
