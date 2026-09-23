// SPDX-License-Identifier: GPL-3.0-or-later
// VR cockpit overlay geometry: what the synthetic wheel, handlebar and hands
// build in the seated frame, without a GPU.
#include <gtest/gtest.h>

#include <cstring>

#include "gfx/cockpit.hpp"

namespace {
using aurora::gfx::cockpit::V;
using aurora::gfx::cockpit::Vertex;

bool all_finite(const std::vector<Vertex>& vertices) {
  for (const auto& vertex : vertices) {
    for (float value : vertex.position) {
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      if ((bits & 0x7f800000u) == 0x7f800000u) {
        return false;
      }
    }
  }
  return true;
}

void set_identity(float (&matrix)[12], V translation) {
  const auto identity = aurora::gfx::cockpit::identity();
  std::memcpy(matrix, identity.data(), sizeof(matrix));
  matrix[3] = translation[0];
  matrix[7] = translation[1];
  matrix[11] = translation[2];
}

class CockpitGeometry : public ::testing::Test {
protected:
  void SetUp() override { clear_meshes(); }
  void TearDown() override { clear_meshes(); }
  static void clear_meshes() {
    std::lock_guard lock(aurora::gfx::cockpit::meshMutex);
    aurora::gfx::cockpit::meshes = {};
  }
};

TEST_F(CockpitGeometry, NativeWheelWithoutHandsDrawsNothing) {
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  EXPECT_TRUE(aurora::gfx::cockpit::geometry(cockpit).empty());
}

TEST_F(CockpitGeometry, SyntheticKartWheelSitsOnItsRim) {
  AuroraCockpit cockpit{};
  const auto vertices = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_FALSE(vertices.empty());
  ASSERT_TRUE(all_finite(vertices));
  // The rim, spokes and hub stay within the 0.18 m wheel plus its tube, around
  // the wheel centre the input side uses (steering_wheel.h).
  for (const auto& vertex : vertices) {
    const float x = vertex.position[0];
    const float y = vertex.position[1] + 0.30f;
    EXPECT_LE(std::hypot(x, y), 0.18f + 0.02f);
    EXPECT_NEAR(vertex.position[2], -0.42f, 0.04f);
  }
}

TEST_F(CockpitGeometry, SyntheticWheelTurnsWithTheAngle) {
  AuroraCockpit cockpit{};
  const auto straight = aurora::gfx::cockpit::geometry(cockpit);
  cockpit.wheelAngle = 0.5f;
  const auto turned = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_EQ(straight.size(), turned.size());
  bool moved = false;
  for (size_t i = 0; i < straight.size() && !moved; ++i) {
    moved = std::abs(straight[i].position[0] - turned[i].position[0]) > 1e-3f;
  }
  EXPECT_TRUE(moved);
}

TEST_F(CockpitGeometry, SyntheticHandlebarFollowsItsFrame) {
  AuroraCockpit cockpit{};
  cockpit.bike = true;
  cockpit.handlebarRadius = 0.25f;
  // Bar axis along seat +X, centred 0.3 m down and 0.42 m ahead.
  const float pose[12]{1, 0, 0, 0, 0, 0, 1, -0.3f, 0, -1, 0, -0.42f};
  std::memcpy(cockpit.seatFromHandlebar, pose, sizeof(pose));
  const auto vertices = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_FALSE(vertices.empty());
  ASSERT_TRUE(all_finite(vertices));
  float minX = 1e9f;
  float maxX = -1e9f;
  for (const auto& vertex : vertices) {
    minX = std::min(minX, vertex.position[0]);
    maxX = std::max(maxX, vertex.position[0]);
  }
  EXPECT_NEAR(minX, -0.25f, 0.03f);
  EXPECT_NEAR(maxX, 0.25f, 0.03f);
}

TEST_F(CockpitGeometry, TrackedHandDrawsAGloveAtItsGrip) {
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  cockpit.hands[1].tracked = true;
  cockpit.hands[1].squeeze = 1.0f;
  set_identity(cockpit.hands[1].seatFromGrip, {0.2f, -0.3f, -0.4f});
  const auto vertices = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_FALSE(vertices.empty());
  ASSERT_TRUE(all_finite(vertices));
  for (const auto& vertex : vertices) {
    EXPECT_LT(std::abs(vertex.position[0] - 0.2f), 0.15f);
    EXPECT_LT(std::abs(vertex.position[1] + 0.3f), 0.15f);
    EXPECT_LT(std::abs(vertex.position[2] + 0.4f), 0.15f);
  }
}

// The grip space OpenXR defines: -Z up the curled fingers' tube towards the
// thumb, +X out of the palm. So the fingers run along Y (+Y on the right hand,
// -Y on the left) and close towards +X, never out of the back of the hand.
TEST_F(CockpitGeometry, GloveFingersRunAlongTheHandAndCloseIntoThePalm) {
  // Both grips carry the same orientation when the hands hold a wheel symmetrically, so the fingers
  // run along -Y on both and it is the palm side that mirrors: +X on the left hand, -X on the right.
  // Building the right hand's fingers on +Y instead pointed them at the player (PC, 2026-09-23).
  for (int side = 0; side < 2; ++side) {
    const float palmSide = side == 0 ? 1.0f : -1.0f;
    const auto build = [&](float squeeze) {
      AuroraCockpit cockpit{};
      cockpit.nativeWheel = true;
      cockpit.hands[side].tracked = true;
      cockpit.hands[side].squeeze = squeeze;
      set_identity(cockpit.hands[side].seatFromGrip, {0.0f, 0.0f, 0.0f});
      return aurora::gfx::cockpit::geometry(cockpit);
    };
    struct Extent {
      float reach = 0.0f;   // furthest along the fingers
      float palm = 0.0f;    // furthest towards the palm's normal
      float back = 0.0f;    // furthest out of the back of the hand
      float across = 0.0f;  // furthest across the knuckles
    };
    const auto measure = [&](const std::vector<Vertex>& vertices) {
      Extent e{};
      for (const auto& vertex : vertices) {
        e.reach = std::max(e.reach, -vertex.position[1]);
        e.palm = std::max(e.palm, vertex.position[0] * palmSide);
        e.back = std::min(e.back, vertex.position[0] * palmSide);
        e.across = std::max(e.across, std::abs(vertex.position[2]));
      }
      return e;
    };
    const auto open = measure(build(0.0f));
    const auto closed = measure(build(1.0f));
    EXPECT_GT(open.reach, 0.09f) << "open fingers reach along the hand, side " << side;
    EXPECT_LT(open.palm, 0.05f) << "an open hand is flat, side " << side;
    EXPECT_LT(closed.reach, open.reach - 0.02f) << "closing shortens the reach, side " << side;
    EXPECT_GT(closed.palm, open.palm + 0.02f) << "closing moves the fingers into the palm, side " << side;
    EXPECT_GT(closed.back, -0.03f) << "fingers never bend out of the back of the hand, side " << side;
    EXPECT_LT(closed.across, 0.07f) << "fingers stay across the knuckles, side " << side;
  }
}

TEST_F(CockpitGeometry, RuntimeFingersCurlTowardPalmForSqueezeAndWheelGrab) {
  using namespace aurora::gfx::cockpit;
  // OpenXR joint space: -Z runs toward the fingertip, +Y out of the back
  // of the hand, for BOTH hands. Mirror positions, not the curl direction.
  for (int side = 0; side < 2; ++side) {
    SCOPED_TRACE(side);
    HandMesh mesh;
    mesh.parents.fill(1);
    mesh.parents[1] = -1;
    const float rootPose[7]{0, 0, 0.70710678f, 0.70710678f, 0.12f, -0.08f, 0.03f};
    const M root = from_pose(rootPose);
    mesh.bind.fill(root);
    const int bases[]{2, 6, 11, 16, 21};
    for (int finger = 0; finger < 5; ++finger) {
      const int base = bases[finger];
      const int count = finger == 0 ? 4 : 5;
      for (int bone = 0; bone < count; ++bone) {
        M bind = identity();
        bind[3] = (side == 0 ? -1.0f : 1.0f) * (finger - 2) * 0.018f;
        bind[11] = -0.025f * (bone + 1);
        mesh.bind[base + bone] = compose(root, bind);
        mesh.parents[base + bone] = bone == 0 ? 1 : base + bone - 1;
      }
    }
    for (int j = 0; j < 26; ++j) mesh.inverseBind[j] = inverse(mesh.bind[j]);
    // A tiny triangle rigidly weighted to each joint, including each fingertip.
    for (int j = 0; j < 26; ++j) {
      for (V offset : {V{0, 0, 0}, V{0.001f, 0, 0}, V{0, 0, 0.001f}}) {
        AuroraVRHandVertex vertex{};
        const V p = point(mesh.bind[j].data(), offset);
        std::memcpy(vertex.position, p.data(), sizeof(vertex.position));
        vertex.joints[0] = j;
        vertex.weights[0] = 1;
        mesh.indices.push_back(static_cast<uint16_t>(mesh.vertices.size()));
        mesh.vertices.push_back(vertex);
      }
    }
    AuroraCockpitHand hand{};
    set_identity(hand.seatFromGrip, {0, 0, 0});
    const auto build = [&](float squeeze, bool held) {
      hand.squeeze = squeeze;
      hand.held = held;
      std::vector<Vertex> vertices;
      runtime_hand(vertices, hand, mesh);
      return vertices;
    };
    const auto open = build(0, false);
    for (int j = 0; j < 26; ++j) {
      const V bind = point(mesh.inverseBind[1].data(), point(mesh.bind[j].data(), {0, 0, 0}));
      for (int axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(open[j * 3].position[axis], bind[axis] + (axis == 2 ? 0.04f : 0), 1e-6f);
    }
    for (const auto& closed : {build(0.5f, false), build(1, false), build(0, true)}) {
      ASSERT_TRUE(all_finite(closed));
      for (int tip : {5, 10, 15, 20, 25}) {
        EXPECT_LT(closed[tip * 3].position[1], open[tip * 3].position[1] - 0.005f)
            << "fingertip must move toward palm (-Y), joint " << tip;
        EXPECT_GT(closed[tip * 3].position[2], open[tip * 3].position[2])
            << "curl must shorten finger reach, joint " << tip;
      }
      for (int rigid : {0, 1, 6, 11, 16, 21})
        for (int axis = 0; axis < 3; ++axis)
          EXPECT_NEAR(closed[rigid * 3].position[axis], open[rigid * 3].position[axis], 1e-6f);
    }
  }
}

TEST_F(CockpitGeometry, RuntimeHandMeshIsSkinnedWithoutNans) {
  using namespace aurora::gfx::cockpit;
  auto mesh = std::make_shared<HandMesh>();
  // A 26-joint chain, each joint 1 cm past its parent; one triangle on the tip.
  for (int j = 0; j < 26; ++j) {
    mesh->bind[j] = identity();
    mesh->bind[j][11] = -0.01f * float(j);
    mesh->inverseBind[j] = inverse(mesh->bind[j]);
    mesh->parents[j] = j - 1;
  }
  for (int i = 0; i < 3; ++i) {
    AuroraVRHandVertex vertex{};
    vertex.position[0] = 0.01f * float(i);
    vertex.position[2] = -0.25f;
    vertex.joints[0] = 25;
    vertex.joints[1] = vertex.joints[2] = vertex.joints[3] = -1;
    vertex.weights[0] = 1.0f;
    mesh->vertices.push_back(vertex);
    mesh->indices.push_back(uint16_t(i));
  }
  {
    std::lock_guard lock(meshMutex);
    meshes[0] = mesh;
  }
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  cockpit.hands[0].tracked = true;
  cockpit.hands[0].held = true;
  set_identity(cockpit.hands[0].seatFromGrip, {-0.2f, -0.3f, -0.4f});
  const auto vertices = geometry(cockpit);
  ASSERT_EQ(vertices.size(), 3u) << "the runtime mesh replaces the glove";
  EXPECT_TRUE(all_finite(vertices));
}

} // namespace
