#pragma once
#include "backend/arithmetic_common_features.hpp"

namespace pred::beir::arithmetic {
std::map<std::string,double> intervalOperatorFeatures(const llvm::json::Object&);
std::map<std::string,double> stateControlLoadFeatures(const std::vector<int>& input_widths,
    const std::vector<int>& stage_enabled_data_bits, int external_data_bits, int service_mode);
}
