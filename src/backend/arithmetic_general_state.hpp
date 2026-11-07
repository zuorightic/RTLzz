#pragma once
#include "backend/arithmetic_state.hpp"
#include <memory>

namespace pred::beir::arithmetic {
inline constexpr const char* kGeneralStateModel = "arithmetic-general-state-qualified-v1";
inline constexpr const char* kGeneralComponentManifest = "arithmetic-general-component-manifest-v1";

// Explicit surrounding RTL protocol, not inferred from the number of FFs.
// mode0: independent data/valid write enables; mode1: one shared enable;
// mode2: independent data enables and continuously advancing input valid.
struct ServiceStateProtocol {
    int mode = -1;
    std::map<std::string,std::string> data_write_enables;
    std::string valid_write_enable;
    bool external_data_unconditional = false, external_valid_unconditional = false;
};
struct GeneralStateSlot {
    StateSlot state;
    std::string owner, write_enable;
    std::vector<std::string> affine_bits;
    bool affine_supported = false;
};
struct GeneralStateCostQuery {
    StateCostQuery core;
    ServiceStateProtocol protocol;
    std::vector<GeneralStateSlot> slots;
    std::map<std::string,std::map<std::string,double>> owner_features;
    std::map<std::string,double> control_features;
    std::string layout_sha256, source_scope;
    bool source_scope_qualified = false;
};
struct GeneralStateCostEstimate {
    std::map<std::string,double> owner_area_um2, boundary_upper_ps, residual_budget_ps;
    std::uint64_t bits = 0, signals = 0;
    double owned_state_area_um2 = 0;
    std::string model_sha256, layout_sha256;
    bool shared_physical_area_included = false, joint_arithmetic_boundary_qualified = false;
};
GeneralStateCostQuery generalStateCostQuery(const Program&,const pipeline::Options&,
    const std::vector<int>&,double,const ServiceStateProtocol&);
GeneralStateCostQuery generalStateCostQuery(const pipeline::Program&,double,const ServiceStateProtocol&);
std::string generalStateCostQueryJson(const GeneralStateCostQuery&);
std::string generalStateCostEstimateJson(const GeneralStateCostEstimate&);

class GeneralStateModel {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    GeneralStateModel(const std::string&,const std::string&,const rtlzz::ArithmeticRequest&);
    GeneralStateCostEstimate price(const Program&,const GeneralStateCostQuery&) const;
    GeneralStateCostEstimate price(const pipeline::Program&,const ServiceStateProtocol&) const;
    std::string measurementIdentity() const;
    std::string nativeParent() const;
};

// Versioned production route. No whole-stage or scalar-boundary fallback.
class GeneralCompositeCostProvider final : public CostProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
protected:
    CostEstimate estimateGroup(const Program&,const std::vector<NodeId>&) const override;
public:
    GeneralCompositeCostProvider(const Program&,const rtlzz::ArithmeticRequest&);
    CostEstimate node(const Program&,NodeId) const override;
    CostEstimate nodeInStage(const Program&,NodeId,const StageCostQuery&) const override;
    bool requiresStageContext() const override {return true;}
    bool requiresStateQuery() const override {return true;}
    GeneralStateCostEstimate generalStateCost(const Program&,const GeneralStateCostQuery&) const override;
    StateCostEstimate stateCost(const Program&,const StateCostQuery&) const override;
    std::string componentReport(const Program&,const std::vector<int>& = {},int = 0,const std::vector<NodeId>& = {}) const override;
    std::string modelIdentity() const override;
    double stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const override;
    void validateStateProtocol(bool,bool,bool) const override;
    void validateInterface(const Program&,const std::string&) const override;
    double dataBudget(bool) const override;
    double stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    double stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    bool stageCovered(std::size_t,std::uint64_t) const override {return false;}
    bool stageShapeCovered(const std::vector<NodeId>&) const override {return false;}
    bool monotoneGroupBounds() const override {return false;}
};
}
