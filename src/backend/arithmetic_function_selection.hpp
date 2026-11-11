#pragma once
#include "backend/arithmetic_condition_selection.hpp"

namespace pred::beir::arithmetic {
struct FunctionDContext {
    std::string S_binding,S_binding_sha256,composition_policy,composition_policy_sha256;
    // Explicit scoped output. This does not assert full joint qualification.
    std::string output_scope="prototype-no-reset-timing-v1";
};
// Uses the frozen operator functions, actual S state and normalized zero-G DAG.
// Every enumerated candidate must be covered before any RTL can be selected.
ConditionalSelection selectFunctionD(const ConditionalViews&,const pipeline::Options&,
    const rtlzz::ArithmeticRequest&,const FunctionDContext&);
}
