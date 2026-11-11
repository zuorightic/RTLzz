#include "backend/arithmetic_factored_timing.hpp"
#include "backend/arithmetic_operator_query.hpp"
#include "backend/rtlgen.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
static std::string read(const std::filesystem::path& p){std::ifstream f(p);require(bool(f),"missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static V parse(const std::string& s){auto v=llvm::json::parse(s);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));return std::move(*v);}
int main(int argc,char** argv){try{
    require(argc==8,"root, source manifest, node model, S binding, timing policy, policy SHA, output required");
    std::filesystem::path root=argv[1],out=argv[7];require(!std::filesystem::exists(out),"preserve native owned-timing attempt");std::filesystem::create_directories(out);
    auto manifest=parse(read(argv[2]));A sources,negatives,positives;bool checked=false;
    for(const auto& item:*manifest.getAsObject()->getArray("sources")){
        const auto& record=*item.getAsObject();auto source=parse(read(record.getString("request")->str()));const auto& row=*source.getAsObject();
        require(row.getString("role")=="combination_validation","locked joint source cannot enter repricing");
        auto prior=parse(read(record.getString("parent_export")->str()));const auto& old=*prior.getAsObject();
        pipelinev2::PipelineConfig c;c.source_name=row.getString("id")->str()+".cpp";c.source_text=read(row.getString("source")->str());
        require(rtlzz::arithmeticHash(*c.source_text)==row.getString("source_sha256")->str(),"real source changed");
        c.top_function="stage7_top";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();auto& options=*c.pipeline;
        options.register_stages=1;options.reset_enabled=true;options.input_valid_name="iv";options.output_valid_name="ov";
        for(const auto& p:*row.getArray("inputs"))options.input_reset_values[p.getAsObject()->getString("name")->str()]={0};options.input_reset_values["iv"]={0};
        rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";fixed.target_platform="asap7";
        fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
        fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));
        fixed.latency=2;fixed.target_period_ps=1500;fixed.candidate_policy="core-original-v1";c.arithmetic_request=fixed;
        auto compiled=pipelinev2::compile(c);require(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);options.arithmetic_request=fixed;
        auto inventory=enumerateConditionalViews(compiled.pipeline_program->combinational,options,fixed,100000);require(inventory.complete,"structural inventory incomplete");
        auto request=fixed;request.cost_mode="backend-model";request.model_bundle=argv[3];request.model_bundle_sha256=rtlzz::arithmeticHash(read(argv[3]));
        A variants;
        for(const auto& value:*old.getArray("variants")){
            const auto& v=*value.getAsObject();std::vector<int> placement;for(const auto& x:*v.getArray("placement"))placement.push_back(int(*x.getAsInteger()));
            const ConditionalView* found=nullptr;for(const auto& view:inventory.views)if(view.placement==placement&&rtlzz::arithmeticHash(emitText(view.graph))==old.getObject("graph")->getString("sha256"))found=&view;
            require(found,"real compiler graph/placement changed");auto actual=pipeline::materializeSchedule(found->graph,options,placement);
            require(rtlzz::arithmeticHash(rtlgen::emitSystemVerilogWithArithmeticBoundaries(actual))==v.getString("rtl_sha256"),"real materialized RTL changed");
            std::vector<NodeId> controls;for(const auto& p:found->graph.ports)if(p.name=="iv"||p.name=="ov")controls.push_back(p.element_nodes.front());
            auto cuts=conditionalBoundaryRequests(found->graph,placement,1,controls);A targets;
            for(double t:{500.,1500.,3000.}){request.target_period_ps=t;auto state=stateCostQuery(actual,t,true);A queries;
                for(const auto& q:operatorQueries(found->graph,placement,1,t,fixed.platform_profile_sha256,fixed.operator_boundary_policy,controls))queries.push_back(parse(operatorQueryJson(q)));
                FactoredTimingCostProvider costs(argv[4],rtlzz::arithmeticHash(read(argv[4])),request,argv[5],argv[6]);
                auto result=costs.estimate(found->graph,state,cuts);
                if(result.getString("status")=="candidate_covered"){
                    std::set<int64_t> ids;double area=0;for(const auto& x:*result.getArray("node_prices")){const auto& n=*x.getAsObject();require(ids.insert(*n.getInteger("node")).second,"shared operator charged twice");area+=*n.getNumber("area_um2");}
                    require(std::abs(area-*result.getObject("owner_area_um2")->getNumber("operators"))<1e-8,"unique node area mismatch");
                    if(!checked&&t==1500){checked=true;
                        auto reject=[&](const char* name,auto fn){bool refused=false;try{fn();}catch(const std::exception&){refused=true;}require(refused,std::string("accepted negative ")+name);negatives.push_back(name);};
                        auto unavailable=[&](const char* name,const auto& bad_state,const auto& bad_cuts){auto answer=costs.estimate(found->graph,bad_state,bad_cuts);require(answer.getString("status")=="cost_uncovered",std::string("accepted stale input ")+name);negatives.push_back(name);};
                        reject("wrong_policy_sha",[&]{FactoredTimingCostProvider x(argv[4],rtlzz::arithmeticHash(read(argv[4])),request,argv[5],"wrong");});
                        auto bad_request=request;bad_request.model_bundle_sha256="wrong";reject("wrong_node_sha",[&]{FactoredTimingCostProvider x(argv[4],rtlzz::arithmeticHash(read(argv[4])),bad_request,argv[5],argv[6]);});
                        reject("wrong_S_binding_sha",[&]{FactoredTimingCostProvider x(argv[4],"wrong",request,argv[5],argv[6]);});
                        bad_request=request;bad_request.target_period_ps=499;reject("outside_target_interval",[&]{FactoredTimingCostProvider x(argv[4],rtlzz::arithmeticHash(read(argv[4])),bad_request,argv[5],argv[6]);});
                        auto malformed=parse(read(argv[5]));(*malformed.getAsObject())["target_interval_ps"]=A{"bad",3000};auto invalid=out/"invalid-policy.json";std::ofstream(invalid)<<llvm::formatv("{0:2}",malformed).str();
                        reject("malformed_target_interval",[&]{FactoredTimingCostProvider x(argv[4],rtlzz::arithmeticHash(read(argv[4])),request,invalid.string(),rtlzz::arithmeticHash(read(invalid)));});
                        auto bad_state=state;bad_state.period_ps+=1;unavailable("wrong_state_period",bad_state,cuts);
                        bad_state=state;bad_state.layout_sha256="wrong";unavailable("forged_layout",bad_state,cuts);
                        bad_state=state;require(!bad_state.slots.empty(),"missing real state");bad_state.slots.front().reset_values[0]^=1;unavailable("forged_state_reset",bad_state,cuts);
                        auto bad_cuts=cuts;bad_cuts.front().cut.cut_sha256="wrong";unavailable("stale_cut_sha",state,bad_cuts);
                        bad_cuts=cuts;require(!bad_cuts.front().edges.empty(),"missing raw edges");bad_cuts.front().edges.erase(bad_cuts.front().edges.begin());unavailable("missing_wire_endpoint",state,bad_cuts);
                        bad_cuts=cuts;bad_cuts.front().captures.clear();unavailable("missing_capture",state,bad_cuts);
                        bad_cuts=cuts;bad_cuts.front().launches.clear();unavailable("missing_launch",state,bad_cuts);
                        auto equivalent=found->graph;bool changed=false;for(auto& s:equivalent.signals)if(s.driver&&s.driver->kind==OperationKind::Assign&&s.driver->operands.size()==1&&s.driver->operands[0].type.width==s.type.width){s.driver->kind=OperationKind::Cast;changed=true;break;}
                        require(changed,"no same-width alias in actual graph");auto materialized=pipeline::materializeSchedule(equivalent,options,placement);
                        auto equivalent_cuts=conditionalBoundaryRequests(equivalent,placement,1,controls);auto same=costs.estimate(equivalent,stateCostQuery(materialized,t,true),equivalent_cuts);
                        require(same.getString("status")=="candidate_covered","equivalent pure wire refused");
                        const auto& before=*result.getArray("stages");const auto& after=*same.getArray("stages");require(before.size()==after.size(),"wire changed stage count");
                        for(std::size_t i=0;i<before.size();++i)for(auto key:{"captures","connections"})require(V(A(*before[i].getAsObject()->getArray(key)))==V(A(*after[i].getAsObject()->getArray(key))),"pure wire representation changed composition");
                        require(V(O(*result.getObject("owner_area_um2")))==V(O(*same.getObject("owner_area_um2"))),"pure wire changed node/S area");positives.push_back("real_graph_same_width_assign_to_cast_preserves_prices_and_bit_connections");
                    }
                }
                targets.push_back(O{{"target_ps",t},{"queries",std::move(queries)},{"owned_timing",std::move(result)}});
            }
            variants.push_back(O{{"id",v.getString("id")->str()},{"placement",A(*v.getArray("placement"))},{"cuts",A(*v.getArray("cuts"))},
                {"state",parse(stateCostQueryJson(stateCostQuery(actual,1500,true)))},{"graph",factoredTimingGraphJson(found->graph)},{"targets",std::move(targets)}});
        }
        sources.push_back(O{{"source",row.getString("id")->str()},{"source_sha256",row.getString("source_sha256")->str()},
            {"variants",std::move(variants)},{"prices_used_to_choose_placement",false}});
    }
    std::ofstream(out/"native.json")<<llvm::formatv("{0:2}",V(O{{"version","owned-timing-native-v1"},{"sources",std::move(sources)},
        {"model_sha256",rtlzz::arithmeticHash(read(argv[3]))},{"policy_sha256",argv[6]},{"negatives",std::move(negatives)},{"positives",std::move(positives)},{"full_D_ready",false}})).str()<<'\n';
    std::cout<<"OWNED_TIMING_REAL_CONSUMER_COMPLETE\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
