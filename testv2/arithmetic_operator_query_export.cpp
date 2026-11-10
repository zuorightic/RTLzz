#include "backend/arithmetic_operator_query.hpp"
#include "backend/arithmetic_selection.hpp"
#include "backend/arithmetic_state.hpp"
#include "backend/rtlgen.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <tuple>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
static std::string read(const std::filesystem::path& path){std::ifstream f(path);check(bool(f),"cannot read "+path.string());return {std::istreambuf_iterator<char>(f),{}};}
static V parse(const std::string& s){auto value=llvm::json::parse(s);check(bool(value),"invalid JSON");return std::move(*value);}
static void write(const std::filesystem::path& path,V value){std::ofstream(path)<<llvm::formatv("{0:2}",value).str()<<'\n';}
static A numbers(const std::vector<int>& values){A out;for(auto n:values)out.push_back(n);return out;}
struct Score {
    double area=0,violation=0,delay=0;std::uint64_t bits=0;
    O report;
};
static Score fixture(const Program& p,const std::vector<int>& placement,const pipeline::Options& options,
                     double t,const std::vector<OperatorQuery>& queries){
    std::map<NodeId,double> costs;double area=0;A node_prices;
    for(const auto& q:queries){const auto& f=q.features;auto get=[&](const char* key){return f.at(key);};bool mul=get("operation_mul");
        double d=(mul?12+0.12*get("width_product"):7+0.3*std::max(get("lhs_width"),get("rhs_width")))+
            0.04*get("mapped_dynamic_bits")+0.02*get("consumer_result_max")+0.001*t;
        double a=(mul?0.05*get("width_product"):0.08*get("result_width"))+0.01*get("mapped_unique_bits");
        costs[q.origin]=d;area+=a;node_prices.push_back(O{{"node",q.origin},{"delay_ps",d},{"comb_area_um2",a},{"status","TEST_ONLY"}});
    }
    std::map<NodeId,double> arrivals;std::set<NodeId> visiting;
    std::function<double(NodeId)> arrival=[&](NodeId id){if(arrivals.count(id))return arrivals.at(id);check(visiting.insert(id).second,"cyclic DAG");
        const auto& s=p.signal(id);double a=0;if(!(s.value.valid&&s.value.constant)&&s.driver->kind!=OperationKind::PortRead){for(const auto& operand:s.driver->operands)if(operand.kind==OperandKind::Symbol&&placement[operand.node]==placement[id])a=std::max(a,arrival(operand.node));a+=costs[id];}
        visiting.erase(id);return arrivals[id]=a;
    };
    double violation=0,delay=0;A stages;std::vector<NodeId> controls;
    for(const auto& port:p.ports)if(port.name==options.input_valid_name||port.name==options.output_valid_name)controls.push_back(port.element_nodes.front());
    for(int stage=0;stage<=options.register_stages;++stage){auto cut=stageCostQuery(p,placement,stage,options.register_stages,controls);double d=0;for(const auto& capture:cut.captures)d=std::max(d,arrival(capture.value));double budget=(stage==0?0.8:1.0)*t-40;
        double slack=budget-d;violation=std::max(violation,-slack);delay=std::max(delay,d+40);stages.push_back(O{{"index",stage},{"comb_delay_ps",d},{"fixture_boundary_ps",40},{"fixture_IO_ps",stage==0?0.2*t:0},{"slack_ps",slack}});
    }
    auto state=stateCostQuery(p,options,placement,t,true);double native_bits=0,external_bits=0,native_words=0,external_words=0;
    for(const auto& slot:state.slots){if(slot.external_capture){external_bits+=slot.variable_bits;++external_words;}else{native_bits+=slot.variable_bits;++native_words;}}
    double input_area=.2*state.input_variable_bits,native_area=.2*native_bits+.03*native_words,external_area=.18*external_bits+.02*external_words;
    double shared_area=.4+.005*(native_bits+external_bits);auto structural=pipeline::evaluateSchedule(p,options,placement);
    double total=area+input_area+native_area+external_area+shared_area;
    return {total,std::max(0.0,violation),delay,structural.register_bits,O{{"status","TEST_ONLY"},{"node_prices",std::move(node_prices)},
        {"stages",std::move(stages)},{"total_area_um2",total},{"timing_violation_ps",std::max(0.0,violation)},{"maximum_fixture_delay_ps",delay},
        {"register_bits",structural.register_bits},{"owner_area_um2",O{{"operators",area},{"input_service",input_area},{"native_state",native_area},{"external_state",external_area},{"shared_clock_physical_repair",shared_area}}},
        {"state_query",parse(stateCostQueryJson(state))},{"S_protocol_note","synthetic coefficients on actual projected slots; external capture unconditionally observed; no new S physical qualification"},
        {"full_path_qualification",false},{"production_binding_implemented",false}}};
}

int main(int argc,char** argv){try{
    check(argc==4||argc==5,"root, explicit source/view request, new output and optional boundary flag required");bool preserve=argc==5;check(!preserve||std::string(argv[4])=="--preserve-arithmetic-boundaries","unknown emission policy");std::filesystem::path root=argv[1],out=argv[3];check(!std::filesystem::exists(out),"preserve earlier attempts");std::filesystem::create_directories(out);
    auto input=parse(read(argv[2]));auto* request=input.getAsObject();check(request&&request->getString("mode")=="TEST_ONLY","software fixture mode must be explicit");
    auto* row=request->getObject("source");check(row,"explicit source");
    pipelinev2::PipelineConfig c;c.source_name=row->getString("source_label")->str();c.source_text=read(row->getString("source")->str());c.top_function=row->getString("top")->str();
    check(rtlzz::arithmeticHash(*c.source_text)==row->getString("generated_source_sha256")->str(),"public source hash changed");c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();auto& options=*c.pipeline;
    options.register_stages=*row->getInteger("N");options.reset_enabled=true;options.input_valid_name=row->getString("iv")->str();options.output_valid_name=row->getString("ov")->str();
    for(const auto& item:*row->getObject("reset")){std::vector<std::uint64_t> words;for(const auto& word:*item.second.getAsArray())words.push_back(std::stoull(word.getAsString()->str(),nullptr,16));options.input_reset_values[item.first.str()]=words;}
    rtlzz::ArithmeticRequest fixed;fixed.target_platform="asap7";fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));fixed.latency=options.register_stages+1;fixed.target_period_ps=1500;fixed.candidate_policy="core-original-v1";c.arithmetic_request=fixed;
    auto compiled=pipelinev2::compile(c);check(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);const auto source=compiled.pipeline_program->combinational;fixed.candidate_policy="mul-candidates-v1";options.arithmetic_request=fixed;
    struct Variant {std::string id;Program graph;std::vector<int> placement;O row;};std::vector<Variant> variants;A candidate_order;unsigned invalid_checks=0;
    auto enumeration=visitCandidateGraphs(source,fixed,[&](const Program& graph,const std::string& id,const std::string& hash){
        const O* old=nullptr;for(const auto& v:*request->getArray("candidates")){const auto* item=v.getAsObject();if(item->getString("candidate")==id)old=item;}
        check(old&&old->getString("policy_shape_sha256")==hash,"current candidate graph differs from frozen structural requirement");
        auto baseline=pipeline::build(graph,options);const auto& p=baseline.combinational;candidate_order.push_back(id);int index=0;
        auto units=pipeline::diagnosticSchedulingUnits(p,options);std::set<std::vector<int>> valid_maps;std::vector<const pipeline::SchedulingUnit*> movable;
        for(const auto& unit:units)if(!unit.constant&&!unit.fixed)movable.push_back(&unit);auto trial=baseline.initial_stages;
        std::function<void(std::size_t)> enumerate=[&](std::size_t n){if(n==movable.size()){if(std::isfinite(pipeline::evaluateSchedule(p,options,trial).final_delay))valid_maps.insert(trial);return;}for(int stage=0;stage<=options.register_stages;++stage){for(auto node:movable[n]->members)trial[node]=stage;enumerate(n+1);}};
        enumerate(0);check(valid_maps.size()==old->getArray("views")->size(),"actual structural views changed");
        for(const auto& saved:*old->getArray("views")){const auto& view=*saved.getAsObject();std::vector<int> placement;for(const auto& n:*view.getArray("placement"))placement.push_back(int(*n.getAsInteger()));check(valid_maps.erase(placement)==1,"view not in new native enumeration");
            auto actual=pipeline::materializeSchedule(p,options,placement);pipeline::validateProgram(actual);auto expected=stateCostQuery(p,options,placement,1500,true);auto materialized=stateCostQuery(actual,1500,true);check(expected.layout_sha256==materialized.layout_sha256,"selected view state/physical register layout mismatch");
            auto name=std::string("view")+std::to_string(variants.size());std::ofstream(out/(name+".sv"))<<(preserve?rtlgen::emitSystemVerilogWithArithmeticBoundaries(actual):rtlgen::emitSystemVerilog(actual));
            O result{{"id",name},{"candidate",id},{"placement",numbers(placement)},{"rtl_file",name+".sv"},{"rtl_sha256",rtlzz::arithmeticHash(read(out/(name+".sv")))},{"materialized_state_query",parse(stateCostQueryJson(materialized))},{"target_rows",A{}}};
            variants.push_back({id,p,std::move(placement),std::move(result)});++index;
        }
        auto wrong=baseline.stages;for(const auto& port:p.ports)if(port.direction==PortDirection::Input)wrong[port.element_nodes.front()]=options.register_stages+1;
        try{pipeline::materializeSchedule(p,options,wrong);throw std::runtime_error("invalid map materialized");}catch(const rtlzz::ArithmeticError&){++invalid_checks;}
    });
    A selections;std::size_t query_count=0;A rejected_queries;
    for(const auto& value:*request->getArray("targets_ps")){double target=*value.getAsNumber();std::optional<std::tuple<double,double,double,std::uint64_t,std::size_t>> best;std::size_t chosen=0;bool any_feasible=false;
        std::vector<Score> scores;
        for(std::size_t i=0;i<variants.size();++i){auto& v=variants[i];std::vector<NodeId> controls;for(const auto& port:v.graph.ports)if(port.name==options.input_valid_name||port.name==options.output_valid_name)controls.push_back(port.element_nodes.front());
            auto queries=operatorQueries(v.graph,v.placement,options.register_stages,target,fixed.platform_profile_sha256,preserve?"unsigned-op-hierarchy-v1":"TEST_ONLY-unresolved-physical-boundary-v1",controls);A rows;for(const auto& q:queries)rows.push_back(parse(operatorQueryJson(q)));query_count+=queries.size();
            if(rejected_queries.empty()&&!queries.empty()){
                auto reject=[&](const char* name,const std::function<void(O&)>& mutate){O bad(queries.front().descriptor);mutate(bad);bool caught=false;try{operatorQueryFeatures(bad);}catch(const rtlzz::ArithmeticError&){caught=true;}check(caught,std::string("native query negative accepted: ")+name);rejected_queries.push_back(name);};
                reject("posterior_input",[](O& o){o["data_setup_slack_ps"]=0;});
                reject("unknown_semantics",[](O& o){o["semantics"]="signed";});
                reject("bad_target_relation",[](O& o){(*o.getObject("flow_context"))["abc_target_ps"]=123;});
                reject("unknown_boundary",[](O& o){(*o.getObject("flow_context"))["boundary_policy"]="";});
                reject("bad_result_demand",[](O& o){o["required_result_bits"]=A{};});
                reject("conflicting_mask",[](O& o){auto* p=o.getObject("local_context")->getObject("producer");(*p)["lhs_zero_hex"]="1";(*p)["lhs_one_hex"]="1";});
            }
            auto score=fixture(v.graph,v.placement,options,target,queries);any_feasible|=score.violation==0;
            v.row.getArray("target_rows")->push_back(O{{"target_ps",target},{"queries",std::move(rows)},{"score",O(score.report)}});scores.push_back(std::move(score));
        }
        for(std::size_t i=0;i<scores.size();++i){const auto& score=scores[i];if(any_feasible&&score.violation>0)continue;auto key=std::make_tuple(any_feasible?0:score.violation,score.area,score.delay,score.bits,i);if(!best||key<*best){best=key;chosen=i;}}
        auto status=any_feasible?"TEST_ONLY_predicted_feasible_selected":"TEST_ONLY_target_unmet";auto file="selected-"+std::to_string(int(target))+".sv";std::filesystem::copy_file(out/("view"+std::to_string(chosen)+".sv"),out/file);
        selections.push_back(O{{"target_ps",target},{"selected_view",std::uint64_t(chosen)},{"selection_status",status},{"score",O(scores[chosen].report)}, {"rtl_file",file},{"RTL_functional_validation_required",true},{"formal_D_selection",false}});
    }
    A output;for(auto& v:variants)output.push_back(std::move(v.row));write(out/"queries.json",O{{"version","operator-query-native-software-v1"},{"mode","TEST_ONLY"},{"arithmetic_boundary_emission",preserve},{"candidate_order",std::move(candidate_order)},
        {"candidates",std::uint64_t(enumeration.unique)},{"view_count",std::uint64_t(variants.size())},{"query_count",std::uint64_t(query_count)}, {"variants",std::move(output)},{"selections",std::move(selections)},
        {"invalid_map_rejections",invalid_checks},{"negative_query_rejections",std::move(rejected_queries)},{"candidate_PPA_calls",0},{"full_D_ready",false},{"production_binding_implemented",false}});
    std::cout<<"OPERATOR_QUERY_NATIVE_PASS views="<<variants.size()<<" queries="<<query_count<<" invalid_maps="<<invalid_checks<<" TEST_ONLY\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
