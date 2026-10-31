#pragma once
#include "backend/beir.hpp"
namespace pred::beir::arithmetic {
// Semantic structural certificate for registered mul-block-tree-v1 graphs.
// Combinational and materialized graphs share the same check; transparent
// register aliases are followed, never inferred from signal names.
void validateBlockTree(const Program&,const ArithmeticRegion&,const std::string& phase);
}
