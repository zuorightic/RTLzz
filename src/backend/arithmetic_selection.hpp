#pragma once
#include "backend/pipeline.hpp"
#include <functional>
namespace pred::beir::arithmetic {
struct CandidateEnumeration { std::size_t eligible=0, unique=0; };
// Shared production enumeration: eligibility, registry order, exact graph
// de-duplication and K=16. The visitor never grants model coverage.
CandidateEnumeration visitCandidateGraphs(const Program&,const rtlzz::ArithmeticRequest&,
    const std::function<void(const Program&,const std::string&,const std::string&)>&,
    const std::function<void(const std::string&,const rtlzz::ArithmeticDiagnostic&)>& = {});
struct Selection { pipeline::Program pipeline; std::string candidate_id; std::string report; };
// Ordered registry configurations, eligibility filtering, structural de-dup and
// K=16 before scheduling; all accepted candidates use the same pipeline builder.
Selection select(const Program&,const pipeline::Options&);
}
