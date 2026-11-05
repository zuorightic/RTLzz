#pragma once
#include "arithmetic_request.hpp"
#include "backend/beir.hpp"
#include "rtlzz.hpp"
#include <vector>
#include <memory>
namespace pred::pipeline { struct Options; }
namespace pred::beir::arithmetic {
inline constexpr const char* kBackendContract = "native-beir-contract-v1";
// One static contract shared by expansion, normalization, cost and materialization.
void validate(const Program&, const std::string& phase, bool circt = false, const std::vector<std::string>& auxiliary_ports = {});
Program normalizeSelections(Program);
Program registerOriginalOperators(Program);
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
    std::string physical_operation;
    std::map<std::string,double> physical_features;
    // A validated composite stage model owns this node's cost. It is counted
    // as an operation for scheduling, but receives no separate node charge.
    bool deferred_to_stage = false;
};
struct StageCapture {
    NodeId value;
    unsigned banks;
};
// A complete proposed placement, including logical captures at this cut.
// Constructed from BEIR and the register map, never from a sample identifier.
struct StageCostQuery {
    int index = 0, last = 0;
    std::vector<int> placement;
    std::vector<NodeId> members, controls;
    std::vector<StageCapture> captures;
    std::string graph_sha256, cut_sha256;
};
StageCostQuery stageCostQuery(const Program&,const std::vector<int>&,int,int,
                             const std::vector<NodeId>& controls = {});
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
    virtual CostEstimate nodeInStage(const Program& p,NodeId id,const StageCostQuery&) const { return node(p,id); }
    virtual bool requiresStageContext() const { return false; }
    // Separate component coverage from permission to price an entire pipeline.
    virtual std::string componentReport(const Program&,const std::vector<int>& = {},int = 0,
                                        const std::vector<NodeId>& = {}) const { return {}; }
    CostEstimate group(const Program&, const std::vector<NodeId>&) const;
    virtual double stateArea(std::uint64_t bits,std::size_t,std::size_t,std::size_t) const { return static_cast<double>(bits); }
    virtual void validateStateProtocol(bool,bool,bool) const {}
    virtual void validateInterface(const Program&,const std::string&) const {}
    virtual double dataBudget(bool) const;
    virtual double stageDelay(double base,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const { return base; }
    virtual double stageArea(double base,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const { return base; }
    virtual bool monotoneGroupBounds() const { return true; }
    virtual bool stageCovered(std::size_t,std::uint64_t) const { return true; }
    // Shape qualification receives the actual candidate stage members. Scalar
    // size bounds alone do not establish support for a new stage topology.
    virtual bool stageShapeCovered(const std::vector<NodeId>&) const { return true; }
    virtual std::string modelIdentity() const { return "fixed-op-width-v1"; }
};
class FixedCostProvider final : public CostProvider {
public:
    explicit FixedCostProvider(CostContext context = {}) : CostProvider(std::move(context)) {}
    CostEstimate node(const Program&, NodeId) const override;
};
std::unique_ptr<CostProvider> makeCostProvider(const Program&,const rtlzz::ArithmeticRequest&);
std::string report(const Program&,const rtlzz::ArithmeticRequest&,
                   const rtlzz::PipelineReport*, const std::vector<int>&,
                   const std::string& rtl_text, const pipeline::Options* protocol = nullptr, bool selected = true);
}
