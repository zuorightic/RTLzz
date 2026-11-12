#pragma once
#include "backend/arithmetic_function_selection.hpp"
namespace pred::beir::arithmetic {
struct FusedDContext : FunctionDContext { std::string G_model,G_model_sha256; };
ConditionalSelection selectFusedD(const ConditionalViews&,const pipeline::Options&,
    const rtlzz::ArithmeticRequest&,const FusedDContext&);
}
