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
  for (int side = 0; side < 2; ++side) {
    const float forward = side == 0 ? -1.0f : 1.0f;
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
        e.reach = std::max(e.reach, vertex.position[1] * forward);
        e.palm = std::max(e.palm, vertex.position[0]);
        e.back = std::min(e.back, vertex.position[0]);
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
