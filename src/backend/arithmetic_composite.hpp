#pragma once
#include "backend/arithmetic_state.hpp"
#include <memory>

namespace pred::beir::arithmetic {
inline constexpr const char* kComponentManifest = "arithmetic-component-manifest-v1";
inline constexpr const char* kNativeStateModel = "arithmetic-native-state-qualified-v1";

// Consumes only frozen parameters. Query qualification is independent of the
// numerical predictor (the published NNLS currently uses just two features).
class NativeStateModel {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    NativeStateModel(const std::string& path,const std::string& sha256,
                     const rtlzz::ArithmeticRequest&);
    StateCostEstimate price(const Program&,const StateCostQuery&) const;
    StateCostEstimate price(const pipeline::Program&,bool external_outputs) const;
    std::string primitiveParent() const;
    std::string measurementIdentity() const;
};

// This manifest binds qualified components, not a qualified whole-D model.
// Whole-stage/interval/joint-boundary consumers require versioned successors.
class CompositeCostProvider final : public CostProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
protected:
    CostEstimate estimateGroup(const Program&,const std::vector<NodeId>&) const override;
public:
    CompositeCostProvider(const Program&,const rtlzz::ArithmeticRequest&);
    CostEstimate node(const Program&,NodeId) const override;
    CostEstimate nodeInStage(const Program&,NodeId,const StageCostQuery&) const override;
    bool requiresStageContext() const override { return true; }
    bool requiresStateQuery() const override { return true; }
    StateCostEstimate stateCost(const Program&,const StateCostQuery&) const override;
    std::string componentReport(const Program&,const std::vector<int>& = {},int = 0,
                                const std::vector<NodeId>& = {}) const override;
    std::string modelIdentity() const override;
    double stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const override;
    void validateStateProtocol(bool,bool,bool) const override;
    void validateInterface(const Program&,const std::string&) const override;
    double dataBudget(bool) const override;
    double stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    double stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    bool stageCovered(std::size_t,std::uint64_t) const override { return false; }
    bool stageShapeCovered(const std::vector<NodeId>&) const override { return false; }
    bool monotoneGroupBounds() const override { return false; }
};
}
