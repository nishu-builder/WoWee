#include "rendering/liquid_type.hpp"

#include <catch_amalgamated.hpp>

using wowee::rendering::BasicLiquidType;
using wowee::rendering::basicLiquidTypeIndex;
using wowee::rendering::classifyBasicLiquidType;
using wowee::rendering::isReflectiveLiquid;

TEST_CASE("vanilla MCLQ liquid types use direct 0..3 classification", "[rendering][water]") {
    REQUIRE(classifyBasicLiquidType(0) == BasicLiquidType::Water);
    REQUIRE(classifyBasicLiquidType(1) == BasicLiquidType::Ocean);
    REQUIRE(classifyBasicLiquidType(2) == BasicLiquidType::Magma);
    REQUIRE(classifyBasicLiquidType(3) == BasicLiquidType::Slime);

    REQUIRE(basicLiquidTypeIndex(0) == 0);
    REQUIRE(basicLiquidTypeIndex(1) == 1);
    REQUIRE(basicLiquidTypeIndex(2) == 2);
    REQUIRE(basicLiquidTypeIndex(3) == 3);

    REQUIRE(isReflectiveLiquid(0));
    REQUIRE(isReflectiveLiquid(1));
    REQUIRE_FALSE(isReflectiveLiquid(2));
    REQUIRE_FALSE(isReflectiveLiquid(3));
}

TEST_CASE("larger liquid material ids preserve legacy modulo fallback", "[rendering][water]") {
    REQUIRE(classifyBasicLiquidType(4) == BasicLiquidType::Slime);
    REQUIRE(classifyBasicLiquidType(5) == BasicLiquidType::Water);
    REQUIRE(classifyBasicLiquidType(6) == BasicLiquidType::Ocean);
    REQUIRE(classifyBasicLiquidType(7) == BasicLiquidType::Magma);
    REQUIRE(classifyBasicLiquidType(8) == BasicLiquidType::Slime);
}
