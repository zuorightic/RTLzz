#pragma once
#include "backend/arithmetic_operator_query.hpp"
namespace pred::beir::arithmetic {
// Prospective geometry only: does not relax the published G support gate.
std::map<std::string,double> livePrefixReductionFeatures(
    const llvm::json::Object& geometry,double target_ps);
}
