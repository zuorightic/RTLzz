#pragma once
#include "backend/pipeline.hpp"
namespace pred::beir::arithmetic {
struct Selection { pipeline::Program pipeline; std::string candidate_id; std::string report; };
// Ordered registry configurations, eligibility filtering, structural de-dup and
// K=16 before scheduling; all accepted candidates use the same pipeline builder.
Selection select(const Program&,const pipeline::Options&);
}
