#pragma once
#include "backend/arithmetic_operator_query.hpp"

namespace pred::beir::arithmetic {
// New ABI: existing <=257-bit qualified Query/CostProvider is unchanged.
std::map<std::string,double> wideOperatorQueryFeatures(const llvm::json::Object& query);
std::string wideOperatorCore(const llvm::json::Object& query,const std::string& name);
// Same scalar implementation as the sampled wide family. Connectivity/price
// qualification is owned by the caller's actual graph query, never fabricated
// in order to pass the predecessor single-consumer validator.
std::string wideOperatorSpecCore(const llvm::json::Object& spec,const std::string& name);
std::string evaluateWideOperator(const llvm::json::Object& spec,const std::string& a_hex,
    const std::string& b_hex,bool control);
}
