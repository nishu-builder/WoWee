#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {

enum class BasicLiquidType : uint8_t {
    Water = 0,
    Ocean = 1,
    Magma = 2,
    Slime = 3,
};

inline BasicLiquidType classifyBasicLiquidType(uint16_t liquidType) {
    if (liquidType <= 3) {
        return static_cast<BasicLiquidType>(liquidType);
    }

    // WMO/newer material IDs are not the vanilla MCLQ 0..3 enum. Preserve the
    // historical fallback for those larger IDs while keeping vanilla terrain
    // water/ocean/magma/slime values direct.
    return static_cast<BasicLiquidType>((liquidType - 1) % 4);
}

inline uint8_t basicLiquidTypeIndex(uint16_t liquidType) {
    return static_cast<uint8_t>(classifyBasicLiquidType(liquidType));
}

inline bool isReflectiveLiquid(uint16_t liquidType) {
    const auto basicType = classifyBasicLiquidType(liquidType);
    return basicType == BasicLiquidType::Water ||
           basicType == BasicLiquidType::Ocean;
}

} // namespace rendering
} // namespace wowee
