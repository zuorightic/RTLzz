#include "backend/arithmetic_selection.hpp"
#include "backend/arithmetic_composite.hpp"
#include "backend/arithmetic_primitives.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <set>

using namespace pred;
using namespace beir;
using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static std::string read(const std::filesystem::path& p){std::ifstream f(p);if(!f)throw std::runtime_error("cannot read "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static void check(bool good,const std::string& why){if(!good)throw std::runtime_error(why);}
static void write(const std::filesystem::path& p,V value){std::ofstream(p)<<llvm::formatv("{0:2}",value).str()<<'\n';}
static A ids(const std::vector<NodeId>& values){A a;for(auto id:values)a.push_back(id);return a;}
static A stages(const std::vector<int>& values){A a;for(int x:values)a.push_back(x);return a;}
static A words(const std::vector<std::uint64_t>& values){A a;for(auto x:values){std::ostringstream s;s<<std::hex<<x;a.push_back(s.str());}return a;}
static std::string operation(const Operation& d){
    if(d.kind==OperationKind::Ite)return "mux";
    if(d.kind==OperationKind::AddCarry)return "addcarry";
    if(d.kind==OperationKind::ReduceOr)return "reduce_or";
    if(d.kind==OperationKind::Binary||d.kind==OperationKind::Unary)switch(d.op){
    case OpCode::Mul:return "mul";case OpCode::Div:return "div";case OpCode::Mod:return "mod";
    case OpCode::Add:return "add";case OpCode::Sub:return "sub";case OpCode::BitOr:return "or";
    case OpCode::BitAnd:return "and";case OpCode::BitXor:return "xor";case OpCode::LogicNot:return "eq_zero";
    case OpCode::Eq:
        for(const auto& a:d.operands)if(a.kind==OperandKind::Literal&&a.constant.isZero())return "eq_zero";
        return "eq";
    case OpCode::Ne:return "ne";case OpCode::Lt:return "lt";case OpCode::Le:return "le";
    case OpCode::Gt:return "gt";case OpCode::Ge:return "ge";case OpCode::LogicAnd:return "logic_and";
    case OpCode::LogicOr:return "logic_or";case OpCode::BitNot:return "bit_not";case OpCode::Neg:return "neg";
    case OpCode::Shl:return "shl";case OpCode::Shr:return "shr";default:return "other_arithmetic";}
    return d.kind==OperationKind::PortRead?"input":"wire";
}
static NodeId canonical(const Program& p,NodeId id){std::set<NodeId> seen;
    while(seen.insert(id).second){const auto& s=p.signal(id);const auto& d=*s.driver;
        if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast)||d.operands.size()!=1||d.operands[0].kind!=OperandKind::Symbol||d.operands[0].type.width!=s.type.width)return id;
        id=d.operands[0].node;
    }throw std::runtime_error("cyclic channel alias");
}
struct Semantics {
    const Program& p;std::map<NodeId,const ArithmeticRegion*> fp;std::set<NodeId> fp_internal;
    std::map<NodeId,std::set<NodeId>> leaves;
    Semantics(const Program& program):p(program){for(const auto& r:p.arithmetic_regions)if(r.template_version=="softfloat3d-bitvector-mul-v1"){
        for(auto id:r.members)fp_internal.insert(id);for(auto id:r.outputs){fp[id]=&r;fp_internal.erase(id);}
    }}
    std::vector<Operand> inputs(NodeId id) const {auto it=fp.find(id);return it==fp.end()?p.signal(id).driver->operands:it->second->inputs;}
    const std::set<NodeId>& sources(NodeId id){auto it=leaves.find(id);if(it!=leaves.end())return it->second;
        std::set<NodeId> result;if(p.signal(id).driver->kind==OperationKind::PortRead)result.insert(id);
        else for(const auto& a:inputs(id))if(a.kind==OperandKind::Symbol){const auto& parent=sources(a.node);result.insert(parent.begin(),parent.end());}
        return leaves.emplace(id,std::move(result)).first->second;
    }
    O source(NodeId id){const auto& s=p.signal(id);const auto& input=sources(id);A roots;for(auto x:input)roots.push_back(x);
        return O{{"origin",id},{"channel_root",canonical(p,id)},{"input_sources",std::move(roots)},
            {"known_one_hex",words(s.value.known_one)},{"known_zero_hex",words(s.value.known_zero)}};
    }
    A nodes(){A rows;for(const auto& s:p.signals){if(fp_internal.count(s.id)||!s.driver)continue;A operands;
        for(const auto& a:inputs(s.id)){O row{{"width",a.type.width},{"signed",a.signed_view}};
            if(a.kind==OperandKind::Symbol){row["kind"]="symbol";row["node"]=a.node;row["billing_width"]=billingWidth(p,a);}
            else if(a.kind==OperandKind::Literal){row["kind"]="literal";row["limbs_hex"]=words(a.constant.limbs);}
            else row["kind"]="port";operands.push_back(std::move(row));}
        rows.push_back(O{{"id",s.id},{"width",s.type.width},{"operation",fp.count(s.id)?"fp32":operation(*s.driver)},
            {"constant",s.value.valid&&s.value.constant},{"driver_kind",int(s.driver->kind)},{"opcode",int(s.driver->op)},
            {"operands",std::move(operands)},{"known_zero_hex",words(s.value.known_zero)},{"known_one_hex",words(s.value.known_one)}});
    }return rows;}
};

int main(int argc,char** argv){try{
    check(argc==4,"root, nine source manifest, output required");const std::filesystem::path root=argv[1],manifest=argv[2],out=argv[3];std::filesystem::create_directories(out);
    auto parsed=llvm::json::parse(read(manifest));check(bool(parsed),"source manifest JSON");const auto& entries=*parsed->getAsObject()->getArray("cases");
    const auto model_path=root/"experiments/arithmetic_dse/configs/model_asap7_d_components_v1.json";
    const auto primitive_path=root/"experiments/arithmetic/evidence/step7_m2_mux_static_v1/asap7-primitives.json";
    rtlzz::ArithmeticRequest fixed;fixed.target_platform="asap7";fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));fixed.target_period_ps=1500;
    A cases;std::size_t total_candidates=0,total_views=0,total_illegal=0;
    for(const auto& value:entries){const auto& entry=*value.getAsObject();auto name=entry.getString("name")->str();
        pipelinev2::PipelineConfig config;config.source_name=entry.getString("source_label")->str();config.source_text=read(entry.getString("source")->str());config.top_function=entry.getString("top")->str();config.clang_args={"-std=c++20","-I"+(root/"vullib").string()};config.use_circt=false;config.pipeline.emplace();auto& options=*config.pipeline;
        options.register_stages=*entry.getInteger("N");options.reset_enabled=true;options.input_valid_name=entry.getString("iv")->str();options.output_valid_name=entry.getString("ov")->str();
        for(const auto& item:*entry.getObject("reset")){std::vector<std::uint64_t> reset;for(const auto& w:*item.second.getAsArray())reset.push_back(std::stoull(w.getAsString()->str(),nullptr,16));options.input_reset_values[item.first.str()]=reset;}
        auto request=fixed;request.latency=options.register_stages+1;request.candidate_policy="core-original-v1";config.arithmetic_request=request;
        auto compiled=pipelinev2::compile(config);check(compiled.ok()&&compiled.pipeline_program.has_value(),name+": "+compiled.error);const auto source=compiled.pipeline_program->combinational;
        request.candidate_policy="mul-candidates-v1";A candidates;
        auto production_options=options;production_options.arithmetic_request=request;auto production_C=select(source,production_options);
        auto production_report=llvm::json::parse(production_C.report);check(bool(production_report),"production C selection report");
        std::map<std::string,std::string> production_shapes;A production_order;
        for(const auto& item:*production_report->getAsObject()->getArray("candidates")){const auto& c=*item.getAsObject();auto candidate=c.getString("id")->str();production_shapes[candidate]=c.getString("shape_sha256")->str();production_order.push_back(candidate);}
        auto enumeration=visitCandidateGraphs(source,request,[&](const Program& graph,const std::string& id,const std::string& shape){
            check(production_shapes.count(id)&&production_shapes.at(id)==shape,"diagnostic and production C candidate graph differ");
            auto settings=options;settings.arithmetic_request=request;auto actual=pipeline::build(graph,settings);pipeline::validateProgram(actual);
            const auto& p=actual.combinational;Semantics semantic(p);auto units=pipeline::diagnosticSchedulingUnits(p,settings);O groups;A group_rows;
            for(const auto& unit:units){groups[unit.label]=ids(unit.members);group_rows.push_back(O{{"label",unit.label},{"members",ids(unit.members)},{"fixed",unit.fixed},{"constant",unit.constant}});}
            auto model=request;model.cost_mode="backend-model";model.fixed_cost_table.clear();model.fixed_cost_table_sha256.clear();model.model_bundle=model_path.string();model.model_bundle_sha256=rtlzz::arithmeticHash(read(model_path));
            auto provider=makeCostProvider(p,model);auto primitive_request=model;primitive_request.model_bundle=primitive_path.string();primitive_request.model_bundle_sha256=rtlzz::arithmeticHash(read(primitive_path));auto primitive=makeCostProvider(p,primitive_request);
            std::vector<NodeId> controls;for(const auto& port:p.ports)if(port.name==settings.input_valid_name)controls.insert(controls.begin(),port.element_nodes.front());
            for(const auto& port:p.ports)if(port.name==settings.output_valid_name)controls.push_back(port.element_nodes.front());check(controls.size()==2,"actual valid controls");
            struct View {std::vector<int> placement;A roles;};std::vector<View> views;std::map<std::vector<int>,std::size_t> seen;std::size_t illegal=0;
            auto add_view=[&](const std::vector<int>& placement,const std::string& role){
                if(auto at=seen.find(placement);at!=seen.end()){views[at->second].roles.push_back(role);return;}
                if(!std::isfinite(pipeline::evaluateSchedule(p,settings,placement).final_delay)){++illegal;return;}
                auto [where,inserted]=seen.emplace(placement,views.size());if(inserted)views.push_back({placement,{}});views[where->second].roles.push_back(role);
            };
            add_view(actual.initial_stages,"C_initial");add_view(actual.stages,"C_final");
            auto current=actual.initial_stages;
            for(std::size_t m=0;m<actual.moves.size();++m){const auto& move=actual.moves[m];auto trial=current;const auto* group=groups.getArray(move.group);check(group,"move must use actual production unit");
                for(const auto& member:*group)trial.at(*member.getAsInteger())=move.to;
                add_view(trial,"C_move_attempt_"+std::to_string(m));if(move.accepted)current=std::move(trial);
            }
            check(current==actual.stages,"replayed C moves must reach final placement");
            for(const auto& base:std::vector<std::pair<std::string,std::vector<int>>>{{"initial",actual.initial_stages},{"final",actual.stages}})
                for(const auto& unit:units)if(!unit.fixed&&!unit.constant)for(int at=0;at<=settings.register_stages;++at)if(at!=base.second[unit.members.front()]){
                    auto trial=base.second;for(auto member:unit.members)trial[member]=at;add_view(trial,base.first+"_legal_neighbour/"+unit.label+"/"+std::to_string(at));}
            A regions;for(const auto& r:p.arithmetic_regions){O parameters;for(const auto& x:r.template_parameters)parameters[x.first]=x.second;A cut_groups;for(const auto& group:r.legal_cut_groups)cut_groups.push_back(ids(group));
                regions.push_back(O{{"id",r.id},{"template",r.template_version},{"operation",r.operation},{"internal_split",r.internal_split_enabled},
                    {"parameters",std::move(parameters)},{"members",ids(r.members)},{"outputs",ids(r.outputs)},{"legal_cut_groups",std::move(cut_groups)}});}
            A ports;for(const auto& port:p.ports)ports.push_back(O{{"name",port.name},{"direction",port.direction==PortDirection::Input?"input":"output"},{"width",port.type.width},{"nodes",ids(port.element_nodes)}});
            A placements;O source_info;
            for(auto& view:views){A stage_rows;for(int at=0;at<=settings.register_stages;++at){auto q=stageCostQuery(p,view.placement,at,settings.register_stages,controls);A members,captures;
                for(auto member:q.members)if(!semantic.fp_internal.count(member))members.push_back(member);
                for(auto capture:q.captures)captures.push_back(O{{"value",capture.value},{"banks",capture.banks},{"declared_width",p.signal(capture.value).type.width}});
                stage_rows.push_back(O{{"index",at},{"cut_sha256",q.cut_sha256},{"semantic_members",std::move(members)},{"captures",std::move(captures)}});}
                auto q=stateCostQuery(p,settings,view.placement,1500,true);for(const auto& slot:q.slots)source_info[std::to_string(slot.origin)]=semantic.source(slot.origin);
                O state;auto state_json=llvm::json::parse(stateCostQueryJson(q));state["query"]=std::move(*state_json);
                try{auto cost=provider->stateCost(p,q);state["production_status"]="predicted_in_domain";state["area_um2"]=cost.area_um2;}
                catch(const rtlzz::ArithmeticError& e){state["production_status"]="cost_uncovered";state["first_reason"]=e.diagnostic.failure_reason;}
                placements.push_back(O{{"placement",stages(view.placement)},{"roles",std::move(view.roles)},{"stages",std::move(stage_rows)},{"state",std::move(state)}});
            }
            auto materialized=stateCostQuery(actual,1500,true);auto projected=stateCostQuery(p,settings,actual.stages,1500,true);check(materialized.layout_sha256==projected.layout_sha256,"actual C materialized/projected layout");
            auto coverage=llvm::json::parse(primitive->componentReport(p,actual.stages,settings.register_stages,controls));check(bool(coverage),"primitive coverage JSON");
            auto filename=name+"-"+std::to_string(candidates.size())+".json";
            write(out/filename,O{{"version","D-candidate-static-requirements-v1"},{"case",name},{"candidate",id},{"policy_shape_sha256",shape},
                {"materialized_graph_sha256",materialized.graph_sha256},{"L",request.latency},{"II",request.initiation_interval},
                {"diagnostic_period_ps",1500},{"formal_target_qualified",false},{"ports",std::move(ports)},{"nodes",semantic.nodes()},
                {"regions",std::move(regions)},{"units",std::move(group_rows)},{"state_source_info",std::move(source_info)},
                {"views",std::move(placements)},{"illegal_move_proposals",illegal},{"primitive_C_final_coverage",std::move(*coverage)},
                {"actual_C_final_state_materialization_checked",true},{"D_selection_completed",false},{"PPA_calls",0}});
            candidates.push_back(O{{"id",id},{"shape_sha256",shape},{"file",filename},{"sha256",rtlzz::arithmeticHash(read(out/filename))},{"views",static_cast<std::uint64_t>(views.size())}});
            ++total_candidates;total_views+=views.size();total_illegal+=illegal;
        });
        cases.push_back(O{{"case",name},{"source_sha256",rtlzz::arithmeticHash(*config.source_text)},{"L",request.latency},
            {"production_C_candidate_order",std::move(production_order)},{"production_C_selected",production_C.candidate_id},{"production_C_shape_parity_checked",true},
            {"eligible",enumeration.eligible},{"unique",enumeration.unique},{"candidates",std::move(candidates)}});
        std::cout<<name<<": "<<enumeration.unique<<" production candidates audited\n";
    }
    check(cases.size()==9&&total_candidates==21,"nine-case production policy must have 21 candidates");
    // Four independent occurrences have 81 configurations: exercise the real
    // policy cutoff separately from the nine-case maximum of nine candidates.
    pipelinev2::PipelineConfig cap;cap.source_name="D-policy-K16.cpp";std::string text="#include <fixint.hpp>\nbool iv,ov;\n#pragma input_port iv\n#pragma output_port ov\n";
    for(int i=0;i<4;++i)text+="Int<8>a"+std::to_string(i)+",b"+std::to_string(i)+";Int<16>y"+std::to_string(i)+";\n#pragma input_port a"+std::to_string(i)+"\n#pragma input_port b"+std::to_string(i)+"\n#pragma output_port y"+std::to_string(i)+"\n";
    text+="void top(){";for(int i=0;i<4;++i)text+="y"+std::to_string(i)+"=a"+std::to_string(i)+"*b"+std::to_string(i)+";";text+="}\n";
    cap.source_text=text;cap.top_function="top";cap.clang_args={"-std=c++20","-I"+(root/"vullib").string()};auto compiled_cap=pipelinev2::compile(cap);check(compiled_cap.ok()&&compiled_cap.beir_program.has_value(),"K16 source compilation");
    auto cap_graph=registerOriginalOperators(normalizeSelections(*compiled_cap.beir_program));auto cap_request=fixed;cap_request.candidate_policy="mul-candidates-v1";A cap_order;
    auto capped=visitCandidateGraphs(cap_graph,cap_request,[&](const Program&,const std::string& id,const std::string&){cap_order.push_back(id);});check(capped.unique==16,"production K16 cutoff");
    write(out/"K16.json",O{{"candidate_order",std::move(cap_order)},{"K",16},{"unique",capped.unique},{"PPA_calls",0}});
    write(out/"index.json",O{{"version","D-static-requirements-index-v1"},{"cases",std::move(cases)},{"candidate_count",total_candidates},
        {"view_count",total_views},{"illegal_move_proposals",total_illegal},{"PPA_calls",0},{"full_D_ready",false},
        {"placement_scope","actual C initial/final and recorded trajectory plus every one-unit structural neighbour of initial/final; future D trajectories are not exhaustively enumerated"}});
    std::cout<<"PASS 21 shared-production candidates, legal placement views and unmodified model refusals\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
