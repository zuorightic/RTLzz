#pragma once
#include "backend/arithmetic_condition_selection.hpp"

namespace pred::beir::arithmetic {
inline constexpr const char* kBlockPipelinePolicy = "mul-width-tree-cuts-v1";

// Structural parameters, independent of source names and physical prices.
// Zero preserves an original Mul. Positive widths use the existing exact
// schoolbook tree; register cuts may occur between its reduction levels.
struct BlockPipelinePolicy {
    std::vector<int> block_widths{0,4,8,16,32};
    std::size_t max_structures=16, max_views=4096, max_assignments=8192;
};
struct BlockPipelineInventory {
    ConditionalViews conditional;
    BlockPipelinePolicy policy;
    std::size_t dependency_pruned=0, duplicate_structures=0;
    bool structures_complete=true, placements_complete=true;
    std::vector<std::string> rejected_structures;
};

// Enumerates dependency-respecting placements of the actual scheduling units.
// Every accepted map still passes the shared legality evaluator. Exhaustion of
// either finite budget is explicit, and never grants full candidate coverage.
BlockPipelineInventory enumerateBlockPipelineViews(const Program&,
    const pipeline::Options& fixed_structural_options,const BlockPipelinePolicy& = {});
llvm::json::Object blockPipelinePolicyJson(const BlockPipelinePolicy&);
}
