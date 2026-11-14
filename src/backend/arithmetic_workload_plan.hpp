#pragma once
#include "backend/arithmetic_state.hpp"
#include "backend/arithmetic_reduction.hpp"
#include <llvm/Support/JSON.h>
#include <tuple>

namespace pred::beir::arithmetic {
inline constexpr const char* kWorkloadBoundaryPolicy = "unsigned-workload-owners-v2";
struct WorkloadOperator {
    NodeId node = kInvalidNodeId;
    int stage = 0;
    llvm::json::Object query;
    // Exact projection only: absent when v1 cannot represent a real connection.
    std::optional<llvm::json::Object> predecessor_query;
    std::string predecessor_rejection;
};
struct WorkloadPlan {
    std::vector<WorkloadOperator> operators;
    std::vector<ReductionRegion> reductions;
    std::map<NodeId,llvm::json::Object> reduction_queries;
    std::map<NodeId, NodeId> reduction_owners;
    // Semantic leaf bit at a stage -> actual materialized signal/bit. A
    // forwarding bank may retain a slice/concat alias rather than its source.
    std::map<std::tuple<NodeId,int,int>,std::pair<NodeId,int>> launches;
    std::vector<NodeId> functional_logic;
    std::string graph_sha256, layout_sha256, profile_sha256;
    double target_ps = 0;
};
// Discover from original graph + actual placement + verified state. No sample
// IDs or backend data enter discovery. Both provider and emitter use this plan.
WorkloadPlan workloadPlan(const pipeline::Program&, double target_ps,
                          const std::string& profile_sha256);
llvm::json::Object workloadPlanJson(const WorkloadPlan&);
std::map<std::string,double> workloadQueryFeatures(const llvm::json::Object&);
llvm::json::Object projectWorkloadQuery(const llvm::json::Object&);
}
