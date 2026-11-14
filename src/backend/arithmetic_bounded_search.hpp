#pragma once
#include "backend/pipeline.hpp"
#include <llvm/Support/JSON.h>

namespace pred::beir::arithmetic {
struct BoundedSchedulePolicy { std::size_t beam_width=32, max_extensions=100000; };
struct BoundedSchedules {
    std::vector<std::vector<int>> placements;
    std::size_t extensions=0, beam_discarded=0, invalid_leaves=0;
    bool exhaustive=true, budget_exhausted=false;
};
// The declared grammar fixes wiring at its earliest dependency stage and
// explores arithmetic units at every legal stage. Beam ordering balances the
// number of arithmetic units; this is structural software, never physical cost.
BoundedSchedules boundedSchedules(const Program&,const pipeline::Options&,const BoundedSchedulePolicy& = {});
llvm::json::Object boundedScheduleReport(const BoundedSchedules&,const BoundedSchedulePolicy&);
}
