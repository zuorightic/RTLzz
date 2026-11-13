#pragma once
#include "backend/arithmetic_interval_features.hpp"

namespace pred::beir::arithmetic {
// Opt-in successor only. The public emitter and qualified prices keep their
// existing policy until a new physical/mapping contract is independently bound.
llvm::json::Object normalizeAddQuery(const llvm::json::Object& query);
std::map<std::string,double> normalizedAddFeatures(const llvm::json::Object& query);
std::string normalizedAddCore(const llvm::json::Object& query,const std::string& name);
}
