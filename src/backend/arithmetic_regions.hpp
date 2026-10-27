#pragma once
#include "backend/beopt.hpp"

namespace pred::beir::regions {
// The selected route keeps explicit semantic markers through ordinary BEOPT,
// then expands known implementations. Existing unmarked pattern recognition is
// rejected: arbitrary shared internals are never renamed to region inputs.
Program expandF32(Program program, const Program& implementation);
void validate(const Program& program);
Program optimizeAtomic(Program program, const opt::Options& options,
                       const opt::IterationCallback& callback = {});
std::string report(const Program& program);
} // namespace pred::beir::regions
