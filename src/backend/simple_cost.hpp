#pragma once
#include "backend/beir.hpp"
namespace pred::beir::simple_cost {
struct Cost { std::uint64_t delay=0,area=0; };
// Adapter for the already frozen Python simple-cost-v1.1/effective-width-v2.
// It reads verified static facts only. Unknown normalized operators fail closed.
Cost node(const Program& program, NodeId id);
}
