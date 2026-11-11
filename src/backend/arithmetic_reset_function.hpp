#pragma once
#include <llvm/Support/JSON.h>
#include <string>

namespace pred::beir::arithmetic {
struct ResetTimingPrice {
    bool covered=false,violates=false;
    std::string reason;
    double point_intrinsic_ps=0,guard_intrinsic_ps=0,required_period_ps=0;
};
// Common numerical function, no source, shape, view or exact-clock identity.
ResetTimingPrice resetTimingPrice(const llvm::json::Object& model,double ff,
    double reset_one_bits,double target_ps,int native_depth=1);
}
