#pragma once
#include "backend/beir.hpp"
namespace pred::beir::mul_split {
// Deterministic post-BEOPT expansion. Only unowned unsigned 32x32->64 Mul
// nodes are eligible. Existing atomic regions, including FP, are never entered.
Program expand32(Program program);
}
