// test_side_table.cc — exercise the color side-table.

#include "cap_runtime.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace {

stopless_cap_t fake_cap(uintptr_t off) {
    return reinterpret_cast<stopless_cap_t>(off);
}

class SideTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        heap_ = stopless_heap_create(1 << 20);  // 1 MiB
        ASSERT_NE(heap_, nullptr);
    }
    void TearDown() override {
        stopless_heap_destroy(heap_);
    }
    stopless_heap_t* heap_ = nullptr;
};

TEST_F(SideTableTest, DefaultColorIsGood) {
    EXPECT_EQ(stopless_side_table_load_color(heap_, fake_cap(0x40)),
              STOPLESS_COLOR_GOOD);
}

TEST_F(SideTableTest, StoreThenLoadRoundtrips) {
    stopless_cap_t c = fake_cap(0x80);
    stopless_side_table_store_color(heap_, c, STOPLESS_COLOR_MARKED1);
    EXPECT_EQ(stopless_side_table_load_color(heap_, c),
              STOPLESS_COLOR_MARKED1);
}

TEST_F(SideTableTest, IndependentSlotsDoNotInterfere) {
    stopless_cap_t a = fake_cap(0x100);
    stopless_cap_t b = fake_cap(0x108);     // adjacent 8-byte slot
    stopless_side_table_store_color(heap_, a, STOPLESS_COLOR_MARKED0);
    stopless_side_table_store_color(heap_, b, STOPLESS_COLOR_REMAPPED);
    EXPECT_EQ(stopless_side_table_load_color(heap_, a), STOPLESS_COLOR_MARKED0);
    EXPECT_EQ(stopless_side_table_load_color(heap_, b), STOPLESS_COLOR_REMAPPED);
}

TEST_F(SideTableTest, FlipChangesMatchingColors) {
    stopless_cap_t a = fake_cap(0x200);
    stopless_cap_t b = fake_cap(0x208);
    stopless_side_table_store_color(heap_, a, STOPLESS_COLOR_MARKED0);
    stopless_side_table_store_color(heap_, b, STOPLESS_COLOR_MARKED1);

    // Flip MARKED0 → REMAPPED globally; MARKED1 should be untouched.
    stopless_side_table_flip_region(heap_, /*region=*/nullptr,
                                    STOPLESS_COLOR_MARKED0,
                                    STOPLESS_COLOR_REMAPPED);
    EXPECT_EQ(stopless_side_table_load_color(heap_, a), STOPLESS_COLOR_REMAPPED);
    EXPECT_EQ(stopless_side_table_load_color(heap_, b), STOPLESS_COLOR_MARKED1);
}

}  // namespace
