// SPDX-License-Identifier: GPL-3.0-or-later
// Native steering wheel: indexed-matrix ownership and array matching. The
// ownership cases come from heurazy's mario-kart-wii-VR-port test set.
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "gx_test_common.hpp"
#include "gx/native_wheel.hpp"

namespace {

// Three positions, 12 bytes each. Position 1 is on the wheel; 0 and 2 belong to
// the body/wing.
struct OwnershipFixture {
  std::array<uint8_t, 36> original{};
  std::array<uint8_t, 36> replacement{};
  // PN matrix byte, texture matrix byte, big-endian position index.
  std::array<uint8_t, 12> vertices{0, 30, 0, 0, 0, 30, 0, 1, 3, 30, 0, 2};
  OwnershipFixture() { replacement[12] = 7; }
  bool matches(uint16_t mask) const {
    return aurora::NativeWheelDrawMatches(original, replacement, 12, vertices, 4, 2, 2, mask);
  }
};

TEST(NativeWheelMatch, MixedBodyAndWingDrawAcceptsWheelPositionsOnLocalBody) {
  OwnershipFixture f;
  EXPECT_TRUE(f.matches(1));
}

TEST(NativeWheelMatch, UnrelatedWingJointCannotAnimateWheel) {
  OwnershipFixture f;
  EXPECT_FALSE(f.matches(2));
}

TEST(NativeWheelMatch, OpponentWithoutLocalMatrixIsRejected) {
  OwnershipFixture f;
  EXPECT_FALSE(f.matches(0));
}

TEST(NativeWheelMatch, WheelPositionUnderAnotherMatrixIsRejected) {
  OwnershipFixture f;
  f.vertices[4] = 6;
  EXPECT_FALSE(f.matches(1));
  EXPECT_TRUE(f.matches(4)) << "local body can occupy a different palette slot";
}

TEST(NativeWheelMatch, MalformedMatrixSelectorRejected) {
  OwnershipFixture f;
  f.vertices[4] = 1;
  EXPECT_FALSE(f.matches(1));
}

TEST(NativeWheelMatch, OutOfRangePositionIndexRejected) {
  OwnershipFixture f;
  f.vertices[11] = 3;
  EXPECT_FALSE(f.matches(1));
}

TEST(NativeWheelMatch, SelectionTouchingAnotherJointCannotDeformIt) {
  OwnershipFixture f;
  f.replacement[24] = 1;
  EXPECT_FALSE(f.matches(1));
}

TEST(NativeWheelMatch, EightBitIndicesAndLayoutValidation) {
  OwnershipFixture f;
  std::array<uint8_t, 4> indexed8{0, 1, 3, 2};
  EXPECT_TRUE(aurora::NativeWheelDrawMatches(f.original, f.replacement, 12, indexed8, 2, 1, 1, 1));
  EXPECT_FALSE(aurora::NativeWheelDrawMatches(f.original, f.replacement, 12, indexed8, 2, 2, 1, 1))
      << "invalid vertex layout rejected";
  EXPECT_FALSE(aurora::NativeWheelDrawMatches(f.original, f.original, 12, indexed8, 2, 1, 1, 1))
      << "unchanged positions are not counted as animation";
}

// Draw-time matching against the GX position-matrix palette.
class NativeWheelArrayTest : public ::testing::Test {
protected:
  void SetUp() override {
    aurora::gx::g_gxState = {};
    aurora::gx::nativeWheelArrays.clear();
    aurora::gx::nativeWheelMatches = 0;
    aurora::gx::NativeWheelArray replacement;
    replacement.source = source.data();
    replacement.bytes.assign(36, 0);
    replacement.bytes[12] = 1;
    aurora::gx::nativeWheelArrays.push_back(replacement);
    array.data = source.data();
    array.size = 36;
    array.stride = 12;
  }
  void TearDown() override {
    aurora::gx::g_gxState = {};
    aurora::gx::nativeWheelArrays.clear();
  }
  static float* pos(uint32_t slot) { return reinterpret_cast<float*>(&aurora::gx::g_gxState.pnMtx[slot].pos); }
  std::array<uint8_t, 36> source{};
  aurora::gx::AttrArray array{};
};

TEST_F(NativeWheelArrayTest, MatchesLocalVehicleMatrixOnly) {
  EXPECT_NE(aurora::gx::native_wheel_array(array, nullptr, 0, 0, 0), nullptr);
  EXPECT_EQ(aurora::gx::nativeWheelMatches, 1u);
  // Same asset drawn by an opponent: a translated matrix.
  pos(0)[3] = 100;
  EXPECT_EQ(aurora::gx::native_wheel_array(array, nullptr, 0, 0, 0), nullptr);
  EXPECT_EQ(aurora::gx::nativeWheelMatches, 1u);
}

TEST_F(NativeWheelArrayTest, UnboundSourceIsNotAWheelArray) {
  std::array<uint8_t, 36> other{};
  aurora::gx::AttrArray unrelated = array;
  unrelated.data = other.data();
  EXPECT_EQ(aurora::gx::native_wheel_array(unrelated, nullptr, 0, 0, 0), nullptr);
  EXPECT_TRUE(aurora::gx::native_wheel_source(source.data()));
  EXPECT_FALSE(aurora::gx::native_wheel_source(other.data()));
}

TEST_F(NativeWheelArrayTest, MultiJointBodyChecksPerPositionOwnership) {
  // Multi-joint body with an independently animated wing, as in Mario's
  // mb/mc kart models. Only the wheel position uses the matching body slot.
  aurora::gx::g_gxState.vtxDesc[GX_VA_PNMTXIDX] = GX_DIRECT;
  aurora::gx::g_gxState.vtxDesc[GX_VA_POS] = GX_INDEX16;
  for (uint32_t slot = 0; slot < aurora::gx::MaxPnMtx; ++slot) pos(slot)[3] = 100;
  pos(2)[3] = 0;
  uint8_t vertices[]{6, 0, 1, 3, 0, 2};
  EXPECT_NE(aurora::gx::native_wheel_array(array, vertices, sizeof(vertices), 3, 1), nullptr);
  vertices[0] = 0; // opponent wheel while an unrelated palette slot still matches
  EXPECT_EQ(aurora::gx::native_wheel_array(array, vertices, sizeof(vertices), 3, 1), nullptr);
}

TEST_F(NativeWheelArrayTest, NonFiniteMatrixNeverMatches) {
  pos(0)[0] = __builtin_nanf("");
  EXPECT_EQ(aurora::gx::native_wheel_array(array, nullptr, 0, 0, 0), nullptr);
}

// Draw merging around an animated array. A merged draw appends its vertices to the previous draw's range and renders
// through that draw's array binding, so two draws may merge only when they resolved the same replacement. The kart's
// display list is hundreds of same-state primitives, and merging them is worth several ms an eye on a tiler.
class NativeWheelMergeTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    aurora::gfx::testing::use_draw_command_tracking(true);
    aurora::gx::nativeWheelArrays.clear();
    aurora::gx::nativeWheelLastDecision = nullptr;
    aurora::gx::nativeWheelLastDrawCommand = nullptr;
    aurora::gx::NativeWheelArray replacement;
    replacement.source = source.data();
    replacement.bytes.assign(source.size(), 0);
    replacement.bytes[12] = 1; // one animated position
    aurora::gx::nativeWheelArrays.push_back(replacement);
    auto& state = aurora::gx::g_gxState;
    state.lastVtxFmt = GX_VTXFMT0;
    state.lastVtxSize = 1;
    state.vtxDesc[GX_VA_POS] = GX_INDEX8;
    state.arrays[GX_VA_POS].data = source.data();
    state.arrays[GX_VA_POS].size = static_cast<u32>(source.size());
    state.arrays[GX_VA_POS].stride = 12;
    state.stateDirty = true;
  }
  void TearDown() override {
    aurora::gx::nativeWheelArrays.clear();
    aurora::gx::nativeWheelLastDecision = nullptr;
    aurora::gx::nativeWheelLastDrawCommand = nullptr;
  }
  void draw() {
    std::vector<u8> fifo{static_cast<u8>(GX_TRIANGLES) | static_cast<u8>(GX_VTXFMT0), 0, 3, 0, 1, 2};
    decode_fifo(fifo);
  }
  // The local vehicle's matrix: the replacement's model-view is all zeroes, and so is a default palette slot.
  void makeOpponent() { reinterpret_cast<float*>(&aurora::gx::g_gxState.pnMtx[0].pos)[3] = 100.f; }
  std::array<uint8_t, 36> source{};
};

TEST_F(NativeWheelMergeTest, PrimitivesSharingTheAnimatedArrayStillMerge) {
  draw();
  ASSERT_EQ(aurora::gx::nativeWheelLastDecision, &aurora::gx::nativeWheelArrays.front());
  draw();
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 1u);
}

TEST_F(NativeWheelMergeTest, PrimitivesThatTakeTheOriginalArrayAlsoStillMerge) {
  makeOpponent();
  draw();
  ASSERT_EQ(aurora::gx::nativeWheelLastDecision, nullptr);
  draw();
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 1u);
}

TEST_F(NativeWheelMergeTest, OpponentPrimitiveNeverFoldsIntoAnAnimatedDraw) {
  draw();
  makeOpponent();
  draw();
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 0u) << "the merged whole would render the opponent animated";
}

TEST_F(NativeWheelMergeTest, AnimatedPrimitiveNeverFoldsIntoAnOpponentDraw) {
  makeOpponent();
  draw();
  reinterpret_cast<float*>(&aurora::gx::g_gxState.pnMtx[0].pos)[3] = 0.f;
  draw();
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 0u) << "the wheel would render on the original vertices";
}

} // namespace
