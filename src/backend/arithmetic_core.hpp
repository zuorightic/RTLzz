#pragma once
#include "arithmetic_request.hpp"
#include "backend/beir.hpp"
#include "rtlzz.hpp"
#include <vector>
namespace pred::pipeline { struct Options; }
namespace pred::beir::arithmetic {
inline constexpr const char* kBackendContract = "native-beir-contract-v1";
// One static contract shared by expansion, normalization, cost and materialization.
void validate(const Program&, const std::string& phase, bool circt = false, const std::vector<std::string>& auxiliary_ports = {});
Program normalizeSelections(Program);
Program registerOriginalMultipliers(Program);
int billingWidth(const Program&, const Operand&);
struct CostContext {
    std::string target_platform, platform_profile_sha256;
    std::string cost_mode = "fixed", model_bundle_sha256;
    std::string evaluation_stage = "grt";
    std::string feature_schema = "beir-cost-features-v1";
    // -1 denotes a whole combinational graph; a future stage provider receives
    // actual stage members via group(), never only an application name.
    int stage_index = -1;
};
CostContext costContext(const rtlzz::ArithmeticRequest&);
struct CostEstimate {
    bool covered = true;
    double delay = 0;
    double combinational = 0, state_per_bit = 1;
    std::string delay_unit = "abstract", area_unit = "abstract";
    std::string source = "fixed-op-width-v1", reason;
    int billing_width = 0;
};
class CostProvider {
protected:
    CostContext context_;
    // Common group() validates legal boundaries before dispatching a provider.
    virtual CostEstimate estimateGroup(const Program&, const std::vector<NodeId>&) const;
public:
    explicit CostProvider(CostContext context = {}) : context_(std::move(context)) {}
    const CostContext& context() const { return context_; }
    virtual ~CostProvider() = default;
    virtual CostEstimate node(const Program&, NodeId) const = 0;
    CostEstimate group(const Program&, const std::vector<NodeId>&) const;
};
class FixedCostProvider final : public CostProvider {
public:
    explicit FixedCostProvider(CostContext context = {}) : CostProvider(std::move(context)) {}
    CostEstimate node(const Program&, NodeId) const override;
};
std::string report(const Program&,const rtlzz::ArithmeticRequest&,
                   const rtlzz::PipelineReport*, const std::vector<int>&,
                   const std::string& rtl_text, const pipeline::Options* protocol = nullptr);
}
