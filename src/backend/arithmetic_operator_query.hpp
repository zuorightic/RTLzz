#pragma once
#include "backend/arithmetic_core.hpp"
#include <llvm/Support/JSON.h>

namespace pred::beir::arithmetic {
inline constexpr const char* kOperatorQueryVersion="arithmetic-operator-query-v1";
// Descriptor-only interface: no model files, fitting or physical tools. Node
// identities stay outside the predictive descriptor and numerical features.
struct OperatorQuery {
    NodeId origin=kInvalidNodeId;
    int stage=0;
    llvm::json::Object descriptor;
    std::map<std::string,double> features;
};
std::map<std::string,double> operatorQueryFeatures(const llvm::json::Object&);
std::vector<OperatorQuery> operatorQueries(const Program&,const std::vector<int>& placement,
    int last,double target_ps,const std::string& profile_sha256,const std::string& boundary_policy,
    const std::vector<NodeId>& controls = {});
// Serializes trace and prediction inputs separately. Query extraction is not
// a qualified price and never causes production D to accept an unknown model.
std::string operatorQueryJson(const OperatorQuery&);
}
