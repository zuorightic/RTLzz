// Shared-function train/validation exporter. Placements are chosen structurally,
// before reading any model price or physical result.
#include "backend/arithmetic_state_boundary.hpp"
#include "backend/arithmetic_operator_query.hpp"
#include "backend/rtlgen.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <tuple>
using namespace pred; using namespace beir; using namespace beir::arithmetic;
using O=llvm::json::Object; using A=llvm::json::Array; using V=llvm::json::Value;
static void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
static std::string read(const std::filesystem::path& p){std::ifstream f(p);require(bool(f),"missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static V parse(const std::string& s){auto v=llvm::json::parse(s);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));return std::move(*v);}
static void write(const std::filesystem::path& p,V v){std::ofstream(p)<<llvm::formatv("{0:2}",v).str()<<'\n';}
template<class T> static A nums(const T& v){A a;for(auto x:v)a.push_back(x);return a;}
static O graphJson(const Program& p){
    A nodes,ports;
    for(const auto& s:p.signals){A parents; if(s.driver)for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol)parents.push_back(a.node);
        nodes.push_back(O{{"id",s.id},{"name",s.name},{"width",s.type.width},{"parents",std::move(parents)},
            {"constant",s.value.valid&&s.value.constant},{"port",s.port_name}});}
    for(const auto& port:p.ports)ports.push_back(O{{"name",port.name},{"width",port.type.width},
        {"input",port.direction==PortDirection::Input},{"nodes",nums(port.element_nodes)}});
    return O{{"function",p.function_name},{"sha256",rtlzz::arithmeticHash(emitText(p))},{"nodes",std::move(nodes)},{"ports",std::move(ports)}};
}
static O quoteJson(const ConditionalBoundaryPrice& q){
    O owners;for(const auto& [k,v]:q.owner_area_um2)owners[k]=v;A stages;
    for(const auto& s:q.stages){A launches,captures,wires;
        for(auto [id,v]:s.launch_arrival_ps)launches.push_back(O{{"node",id},{"ps",v}});
        for(auto [id,v]:s.capture_required_ps)captures.push_back(O{{"node",id},{"ps",v}});
        for(auto [e,v]:s.wire_delay_ps)wires.push_back(O{{"from",e.first},{"to",e.second},{"ps",v}});
        stages.push_back(O{{"cut_sha256",s.cut_sha256},{"launches",std::move(launches)},
            {"captures",std::move(captures)},{"wires",std::move(wires)},{"other_timing_violation_ps",s.other_timing_violation_ps}});
    }
    return O{{"covered",q.covered},{"joint_qualified",q.joint_qualified},{"reason",q.reason},
        {"binding_sha256",q.model_sha256},{"state_model_sha256",q.state_model_sha256},
        {"service_sha256",q.service_contract_sha256},{"layout_sha256",q.layout_sha256},
        {"operator_model_sha256",q.operator_model_sha256},{"target_ps",q.target_period_ps},
        {"owners_um2",std::move(owners)},{"stages",std::move(stages)}};
}
int main(int argc,char** argv){try{
    require(argc==6,"root, source request, frozen node candidate, S binding, fresh output required");
    std::filesystem::path root=argv[1],out=argv[5];require(!std::filesystem::exists(out),"preserve previous attempts");std::filesystem::create_directories(out);
    auto input=parse(read(argv[2]));const auto& row=*input.getAsObject();pipelinev2::PipelineConfig c;
    c.source_name=row.getString("id")->str()+".cpp";c.source_text=read(row.getString("source")->str());
    require(rtlzz::arithmeticHash(*c.source_text)==row.getString("source_sha256")->str(),"source changed");
    c.top_function="stage7_top";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();auto& options=*c.pipeline;
    options.register_stages=1;options.reset_enabled=true;options.input_valid_name="iv";options.output_valid_name="ov";
    for(const auto& port:*row.getArray("inputs")){auto o=port.getAsObject();options.input_reset_values[o->getString("name")->str()]={std::stoull(o->getString("reset_hex")->str(),nullptr,16)};}options.input_reset_values["iv"]={0};
    rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";fixed.target_platform="asap7";
    fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));
    fixed.latency=2;fixed.target_period_ps=1500;fixed.candidate_policy="core-original-v1";c.arithmetic_request=fixed;
    auto compiled=pipelinev2::compile(c);require(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);options.arithmetic_request=fixed;
    auto inventory=enumerateConditionalViews(compiled.pipeline_program->combinational,options,fixed,100000);
    require(inventory.complete,"incomplete structural enumeration: "+inventory.reason);
    const auto& graph=inventory.views.front().graph;std::vector<NodeId> controls;
    for(const auto& p:graph.ports)if(p.name=="iv"||p.name=="ov")controls.push_back(p.element_nodes.front());
    auto base_queries=operatorQueries(graph,inventory.views.front().placement,1,1500,fixed.platform_profile_sha256,fixed.operator_boundary_policy,controls);
    require(base_queries.size()>=1,"expected real arithmetic after optimization");
    std::vector<NodeId> arithmetic;for(const auto& q:base_queries)arithmetic.push_back(q.origin);
    std::set<NodeId> arithmetic_set(arithmetic.begin(),arithmetic.end());std::map<NodeId,int> depths;
    std::function<int(NodeId)> depth=[&](NodeId id){if(depths.count(id))return depths.at(id);int n=0;
        const auto& s=graph.signal(id);if(s.driver)for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol)n=std::max(n,depth(a.node));
        return depths[id]=n+(arithmetic_set.count(id)?1:0);};
    std::sort(arithmetic.begin(),arithmetic.end(),[&](auto a,auto b){return std::make_pair(depth(a),a)<std::make_pair(depth(b),b);});
    A signatures;for(const auto& v:inventory.views){A a;for(auto n:arithmetic)a.push_back(v.placement[n]);signatures.push_back(std::move(a));}
    write(out/"structural-audit.json",O{{"graph",graphJson(graph)},{"arithmetic_topological_order",nums(arithmetic)},{"legal_arithmetic_placements",std::move(signatures)}});
    // A cuts after the first arithmetic node; B cuts before the last. For two
    // nodes B places both early. Stable ties use the native placement vector.
    std::vector<const ConditionalView*> chosen;
    for(int variant=0;variant<1;++variant){const ConditionalView* best=nullptr;std::optional<std::tuple<int,std::vector<int>>> best_key;
        for(const auto& v:inventory.views){int distance=0;for(std::size_t j=0;j<arithmetic.size();++j){int want=row.getString("cut")=="early"?0:(j?1:0);distance+=std::abs(v.placement[arithmetic[j]]-want);}
            auto key=std::make_tuple(distance,v.placement);if(!best_key||key<*best_key){best_key=key;best=&v;}}
        require(best&&std::get<0>(*best_key)==0,"predeclared arithmetic cut not legal");chosen.push_back(best);
    }
    // One preregistered cut per source; no price-driven placement selection.
    auto model=fixed;model.cost_mode="backend-model";model.model_bundle=argv[3];model.model_bundle_sha256=rtlzz::arithmeticHash(read(argv[3]));
    std::string binding=argv[4],binding_sha=rtlzz::arithmeticHash(read(binding));A variants;
    for(std::size_t i=0;i<chosen.size();++i){const auto& view=*chosen[i];auto actual=pipeline::materializeSchedule(view.graph,options,view.placement);pipeline::validateProgram(actual);
        auto q=stateCostQuery(actual,1500,true);auto projected=stateCostQuery(view.graph,options,view.placement,1500,true);require(q.layout_sha256==projected.layout_sha256,"materialized state mismatch");
        std::string stem="placement-"+std::to_string(i);std::ofstream(out/(stem+".sv"))<<rtlgen::emitSystemVerilogWithArithmeticBoundaries(actual);
        std::ofstream(out/(stem+".beir"))<<pipeline::emitText(actual);A registers,cuts,targets;
        for(const auto& r:actual.registers){const auto& s=actual.rtl_program.signal(r.node);registers.push_back(O{{"node",r.node},{"name",s.name},{"origin",r.source},
            {"stage",r.stage},{"enable",r.enable},{"enable_name",r.enable==kInvalidNodeId?"":actual.rtl_program.signal(r.enable).name},{"valid",r.is_valid},{"width",s.type.width}});}
        auto requests=conditionalBoundaryRequests(view.graph,view.placement,1,controls);
        for(const auto& b:requests){A captures,edges;for(const auto& capture:b.cut.captures)captures.push_back(O{{"node",capture.value},{"banks",capture.banks}});
            for(auto e:b.edges)edges.push_back(A{e.first,e.second});cuts.push_back(O{{"stage",b.cut.index},{"sha256",b.cut.cut_sha256},
                {"members",nums(b.cut.members)},{"controls",nums(b.cut.controls)},{"launches",nums(b.launches)},
                {"captures",std::move(captures)},{"edges",std::move(edges)}});}
        for(double t:{500.,1500.,3000.}){A queries;for(const auto& oq:operatorQueries(view.graph,view.placement,1,t,fixed.platform_profile_sha256,fixed.operator_boundary_policy,controls))queries.push_back(parse(operatorQueryJson(oq)));
            model.target_period_ps=t;ReusedStateBoundaryProvider provider(binding,binding_sha,model,StateBoundaryUse::CandidateReuse);
            auto state=stateCostQuery(actual,t,true);auto quote=provider.price(view.graph,state,requests,model.model_bundle_sha256);
            require(quote.covered,quote.reason);
            targets.push_back(O{{"target_ps",t},{"queries",std::move(queries)},{"real_S_candidate",quoteJson(quote)},{"diagnostics",parse(quote.diagnostics_json)}});}
        variants.push_back(O{{"id",stem},{"placement",nums(view.placement)},{"rtl_file",stem+".sv"},
            {"rtl_sha256",rtlzz::arithmeticHash(read(out/(stem+".sv")))},{"state",parse(stateCostQueryJson(q))},
            {"registers",std::move(registers)},{"cuts",std::move(cuts)},{"targets",std::move(targets)}});
    }
    write(out/"export.json",O{{"version","stage7-real-combination-export-v1"},{"source_sha256",row.getString("source_sha256")->str()},
        {"graph",graphJson(graph)},{"arithmetic_nodes",nums(arithmetic)},{"structural_assignments",std::uint64_t(inventory.assignments)},
        {"structural_legal_placements",std::uint64_t(inventory.views.size())},{"variants",std::move(variants)},
        {"operator_candidate_sha256",model.model_bundle_sha256},{"S_binding_sha256",binding_sha},{"price_used_for_placement",false},{"full_D_ready",false}});
    std::cout<<"STAGE7_REAL_EXPORT_PASS source="<<c.source_name<<" arithmetic="<<arithmetic.size()<<" placements=1\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
