// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/ForceButton.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

constexpr std::uint32_t kForce = 1U << 12;

TEST(ForceButton, EachPressSwitchesTheSpringOffThenOn) {
    ForceButton force(12, 40.0F);
    EXPECT_TRUE(force.on());   // on at Start
    EXPECT_FALSE(force.update(0).has_value());
    EXPECT_EQ(force.update(kForce), 0.0F);
    EXPECT_FALSE(force.on());
    EXPECT_FALSE(force.update(kForce).has_value());   // held: one press
    EXPECT_FALSE(force.update(1U).has_value());       // another button
    EXPECT_EQ(force.update(kForce | 1U), 40.0F);
    EXPECT_TRUE(force.on());
}

TEST(ForceButton, NothingToSwitchWithoutAStrength) {
    ForceButton force(12, 0.0F);
    EXPECT_FALSE(force.on());
    EXPECT_FALSE(force.update(kForce).has_value());
    EXPECT_FALSE(force.update(0).has_value());
    EXPECT_FALSE(force.update(kForce).has_value());
}

TEST(ForceButton, AButtonPastTheMaskNeverPresses) {
    ForceButton force(40, 50.0F);
    EXPECT_FALSE(force.update(0xFFFFFFFFU).has_value());
}

}  // namespace
}  // namespace wheelbri
