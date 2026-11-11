// Actual step-5 publication consumption. No fixture prices and no EDA.
#include "backend/arithmetic_state_boundary.hpp"
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
    check(argc==6,"root, real source request, released model, S binding, output required");std::filesystem::path root=argv[1],out=argv[5];
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
    A targets;unsigned queries=0;bool group_refused=false,interface_refused=false;
    for(double target:{500.,1000.,1500.,3000.}){request.target_period_ps=target;A views;
        for(const auto& view:inventory.views){std::vector<NodeId> controls;for(const auto& p:view.graph.ports)if(p.name==options.input_valid_name||p.name==options.output_valid_name)controls.push_back(p.element_nodes.front());
            ConditionalOperatorCostProvider provider(view.graph,request,ConditionalUse::Production);A prices;
            auto quote=provider.placementPrices(view.graph,view.placement,options.register_stages,controls);check(!quote.empty(),"no node prices");
            for(const auto& row:quote){check(row.price.covered,"published model misses a real required query: "+row.price.reason);++queries;
                prices.push_back(O{{"node",row.query.origin},{"stage",row.query.stage},{"descriptor",O(row.query.descriptor)},
                    {"operator_delay_ps",row.price.operator_delay_ps},{"delay_reserve_ps",row.price.delay_reserve_ps},{"comb_area_um2",row.price.comb_area_um2}});}
            if(!group_refused){try{provider.group(view.graph,{quote.front().query.origin});}catch(const std::exception&){group_refused=true;}}
            if(!interface_refused){try{provider.validateInterface(view.graph,"node-only");}catch(const std::exception&){interface_refused=true;}}
            views.push_back(O{{"id",view.id},{"node_prices",std::move(prices)}});
        }targets.push_back(O{{"T_ps",target},{"views",std::move(views)}});
    }
    check(group_refused&&interface_refused,"node function granted a whole-stage price");
    A negatives;
    auto changed=request;changed.model_bundle_sha256=std::string(64,'0');bool hash_refused=false;
    try{ConditionalOperatorCostProvider bad(inventory.views.front().graph,changed);}catch(const std::exception&){hash_refused=true;}
    check(hash_refused,"wrong model identity accepted");negatives.push_back(O{{"test","wrong_model_hash"},{"status","rejected"}});
    changed=request;changed.platform_profile_sha256=std::string(64,'0');bool profile_refused=false;
    try{ConditionalOperatorCostProvider bad(inventory.views.front().graph,changed);}catch(const std::exception&){profile_refused=true;}
    check(profile_refused,"wrong platform profile accepted");negatives.push_back(O{{"test","wrong_profile"},{"status","rejected"}});
    for(double t:{499.,3001.,3389.}){request.target_period_ps=t;const auto& v=inventory.views.front();std::vector<NodeId> controls;
        for(const auto& p:v.graph.ports)if(p.name==options.input_valid_name||p.name==options.output_valid_name)controls.push_back(p.element_nodes.front());
        ConditionalOperatorCostProvider provider(v.graph,request);auto quotes=provider.placementPrices(v.graph,v.placement,options.register_stages,controls);
        check(std::all_of(quotes.begin(),quotes.end(),[](const auto& q){return !q.price.covered;}),"outside publication clock accepted");negatives.push_back(O{{"T_ps",t},{"status","cost_uncovered"}});
    }
    request.target_period_ps=1500;ReusedStateBoundaryProvider boundary(argv[4],rtlzz::arithmeticHash(read(argv[4])),request,StateBoundaryUse::CandidateReuse);
    auto joint=selectConditionalViews(inventory,options,request,boundary,ConditionalUse::Production);auto report=parse(joint.report);
    check(!joint.pipeline&&report.getAsObject()->getBoolean("formal_D_selection")==false,"node publication bypassed S/G joint gate");
    write(out/"joint-refusal.json",std::move(report));
    write(out/"proof.json",O{{"status","passed_actual_production_node_consumer"},{"view_count",44},{"targets",std::move(targets)},
        {"queries",queries},{"published_model_sha256",request.model_bundle_sha256},{"candidate_permission_used",false},
        {"out_of_scope_refusals",std::move(negatives)},{"group_and_interface_refused",true},{"full_D_joint_gate_retained",true},{"new_EDA_calls",0},{"full_D_ready",false}});
    std::cout<<"PRODUCTION_NODE_RELEASE_CONSUMER_PASS views=44 targets=4 queries="<<queries<<" joint_gate_retained\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
