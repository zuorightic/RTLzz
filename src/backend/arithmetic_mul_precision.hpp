#pragma once
#include "backend/beir.hpp"
namespace pred::beir::arithmetic {
// Restore unsigned precision after BEOPT has expressed truncate+zero-extend as
// a wide mask. Only statically proven zero high bits are removed. The original
// declared result is restored explicitly, before original regions are tagged.
Program canonicalizeUnsignedMulPrecision(Program);
}
