#pragma once
#include "backend/beir.hpp"
namespace pred::beir::mul_split {
struct TemplateDescriptor { const char* id; const char* implementation; int block_divisor; const char* reduction; bool internally_cuttable; };
const std::vector<TemplateDescriptor>& registry();
struct Configuration { std::string region_id; int block_width = 0; };
// Only registered unsigned original Mul regions are replaced. Input provenance
// and truncation are preserved. Zero block_width retains the original region.
Program expand(Program program, const std::vector<Configuration>& configurations);
}
