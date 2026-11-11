// Actual step-5 publication consumption. No fixture prices and no EDA.
#include "backend/arithmetic_shared_function.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static void check(bool b,const std::string& m){if(!b)throw std::runtime_error(m);}
static std::string read(const std::filesystem::path& p){std::ifstream f(p);check(bool(f),"missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static V parse(const std::string& s){auto v=llvm::json::parse(s);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));return std::move(*v);}
static void write(const std::filesystem::path& p,V v){std::ofstream(p)<<llvm::formatv("{0:2}",v).str()<<'\n';}
int main(int argc,char** argv){try{
    check(argc==8,"root, real source request, node model, S binding, policy, policy SHA, output required");std::filesystem::path root=argv[1],out=argv[7];
    check(!std::filesystem::exists(out),"preserve earlier attempt");std::filesystem::create_directories(out);
    auto document=parse(read(argv[2]));const auto& row=*document.getAsObject()->getObject("source");pipelinev2::PipelineConfig c;
    c.source_name=row.getString("source_label")->str();c.source_text=read(row.getString("source")->str());
    check(rtlzz::arithmeticHash(*c.source_text)==row.getString("generated_source_sha256")->str(),"source changed");c.top_function=row.getString("top")->str();
    c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();auto& options=*c.pipeline;
    options.register_stages=*row.getInteger("N");options.reset_enabled=true;options.input_valid_name=row.getString("iv")->str();options.output_valid_name=row.getString("ov")->str();
    for(const auto& item:*row.getObject("reset")){std::vector<std::uint64_t> words;for(const auto& v:*item.second.getAsArray())words.push_back(std::stoull(v.getAsString()->str(),nullptr,16));options.input_reset_values[item.first.str()]=words;}
    rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";fixed.target_platform="asap7";
    fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));
    fixed.latency=options.register_stages+1;fixed.target_period_ps=1500;fixed.candidate_policy="core-original-v1";c.arithmetic_request=fixed;
    auto compiled=pipelinev2::compile(c);check(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);options.arithmetic_request=fixed;
    fixed.candidate_policy="mul-candidates-v1";auto inventory=enumerateConditionalViews(compiled.pipeline_program->combinational,options,fixed);
    check(inventory.complete&&inventory.views.size()==44,"structural regression inventory changed");
    auto request=fixed;request.cost_mode="backend-model";request.model_bundle=argv[3];request.model_bundle_sha256=rtlzz::arithmeticHash(read(argv[3]));
    A targets;unsigned covered=0,uncovered=0;
    for(double t:{500.,750.,1000.,1250.,1500.,2200.,3000.}){request.target_period_ps=t;A views;
        for(const auto& view:inventory.views){
            auto materialized=pipeline::materializeSchedule(view.graph,options,view.placement);
            std::vector<NodeId> controls;for(const auto& p:view.graph.ports)if(p.name==options.input_valid_name||p.name==options.output_valid_name)controls.push_back(p.element_nodes.front());
            auto cuts=conditionalBoundaryRequests(view.graph,view.placement,options.register_stages,controls);
            SharedFunctionCostProvider provider(argv[4],rtlzz::arithmeticHash(read(argv[4])),request,argv[5],argv[6]);
            auto result=provider.estimate(view.graph,stateCostQuery(materialized,t,true),cuts);
            if(result.getString("status")=="candidate_covered")++covered;else ++uncovered;
            auto state=stateCostQuery(materialized,t,true);
            ReusedStateBoundaryProvider baseline(argv[4],rtlzz::arithmeticHash(read(argv[4])),request,StateBoundaryUse::CandidateReuse);
            auto quote=baseline.price(view.graph,state,cuts,request.model_bundle_sha256);check(quote.covered,quote.reason);
            auto diagnostics=parse(quote.diagnostics_json);double functional=0;
            for(auto n:{"input_service","native_state","external_state"})functional+=quote.owner_area_um2.at(n);
            ConditionalOperatorCostProvider nodes(view.graph,request,ConditionalUse::Production);auto prices=nodes.placementPrices(view.graph,view.placement,options.register_stages,controls);
            for(const auto& value:prices){check(value.price.covered,value.price.reason);functional+=value.price.comb_area_um2;}
            views.push_back(O{{"id",view.id},{"estimate",std::move(result)},{"state",parse(stateCostQueryJson(state))},
                {"diagnostics",std::move(diagnostics)},{"predicted_functional_area_um2",functional},{"placement",A{}},
                {"graph",sharedFunctionGraphJson(view.graph)}});
        }
        targets.push_back(O{{"target_ps",t},{"views",std::move(views)}});
    }
    write(out/"proof.json",O{{"status","completed_actual_factored_MAC32_demand"},{"views",44},{"covered",covered},{"uncovered",uncovered},{"targets",std::move(targets)},
        {"policy_sha256",argv[6]},{"model_sha256",request.model_bundle_sha256},{"prices_used_to_choose_placement",false},{"full_D_ready",false}});
    std::cout<<"FACTORED_MAC32_DEMAND_COMPLETE covered="<<covered<<" uncovered="<<uncovered<<"\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
