// Public software entry point: actual C++ frontend -> limb split -> finite schedules
// -> materialized native RTL and prospective graph/state demand. No EDA runner.
#include "backend/arithmetic_limb_split.hpp"
#include "backend/arithmetic_bounded_search.hpp"
#include "backend/arithmetic_state.hpp"
#include "backend/arithmetic_core.hpp"
#include "backend/mul_split.hpp"
#include "backend/arithmetic_mul_precision.hpp"
#include "backend/arithmetic_workload_provider.hpp"
#include "backend/rtlgen.hpp"
#include "backend/rtl_names.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <chrono>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
namespace {
void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
std::string read(const std::filesystem::path& p){std::ifstream f(p);require(bool(f),"missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
V parse(const std::string& text){auto v=llvm::json::parse(text);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));return std::move(*v);}
void write(const std::filesystem::path& p,const std::string& text){std::ofstream f(p);require(bool(f),"cannot write "+p.string());f<<text;require(bool(f),"write failed");}
std::string json(V v){return llvm::formatv("{0:2}",v).str()+"\n";}
std::string text(const O& o,const char* key){auto p=o.getString(key);require(bool(p),std::string("missing ")+key);return p->str();}
int integer(const char* s,int low,int high){std::string raw(s);require(!raw.empty()&&raw.find_first_not_of("0123456789")==std::string::npos,"integer required");auto n=std::stoull(raw);require(n>=std::uint64_t(low)&&n<=std::uint64_t(high),"integer out of bounds");return int(n);}
std::string hex(const std::vector<std::uint64_t>& words){std::ostringstream s;s<<std::hex;for(std::size_t i=words.size();i-->0;){if(i+1!=words.size())s<<std::setw(16)<<std::setfill('0');s<<words[i];}return words.empty()?"0":s.str();}
std::string join(const std::vector<std::string>& v){std::string s;for(const auto& a:v){if(!s.empty())s+=",";s+=a;}return s;}
std::string operation(const Operation& d){
    if(d.kind==OperationKind::AddCarry)return "adc";
    if(d.kind==OperationKind::Binary){switch(d.op){case OpCode::Add:return "add";case OpCode::Sub:return "sub";case OpCode::Mul:return "mul";
    case OpCode::Lt:return "ult";case OpCode::Le:return "ule";case OpCode::Eq:return "eq";
    case OpCode::Gt:return "ugt";case OpCode::Ge:return "uge";case OpCode::Ne:return "ne";
    case OpCode::BitAnd:return "and";case OpCode::BitOr:return "or";case OpCode::BitXor:return "xor";
    case OpCode::LogicAnd:return "land";case OpCode::LogicOr:return "lor";
    case OpCode::Shl:return "shl";case OpCode::Shr:return "shr";default:return "unsupported_binary";}}
    if(d.kind==OperationKind::Unary&&d.op==OpCode::BitNot)return "not";
    if(d.kind==OperationKind::Unary&&d.op==OpCode::LogicNot)return "lnot";
    if(d.kind==OperationKind::Unary&&d.op==OpCode::Neg)return "neg";
    switch(d.kind){case OperationKind::Ite:return "mux";case OperationKind::PortRead:return "input";
    case OperationKind::Slice:return "slice";case OperationKind::BitSelect:return "bitselect";case OperationKind::Concat:return "concat";
    case OperationKind::Assign:case OperationKind::Cast:case OperationKind::ZExt:case OperationKind::Trunc:return "wire";default:return "unsupported";}
}
O graphJson(const Program& p){
    A nodes,ports,regions;
    for(const auto& s:p.signals){const auto& d=*s.driver;A operands;
        for(const auto& a:d.operands)operands.push_back(O{{"node",a.kind==OperandKind::Symbol?V(a.node):V(nullptr)},
            {"width",a.type.width},{"literal_hex",a.kind==OperandKind::Literal?V(hex(a.constant.limbs)):V(nullptr)},
            {"signed",a.signed_view||a.constant.signed_view}});
        nodes.push_back(O{{"id",s.id},{"width",s.type.width},{"operation",operation(d)},{"operands",std::move(operands)},
            {"lo",d.lo},{"hi",d.hi},{"bit",d.bit},{"known_zero_hex",s.value.valid?hex(s.value.known_zero):"0"},
            {"known_one_hex",s.value.valid?hex(s.value.known_one):"0"},{"constant",s.value.valid&&s.value.constant}});
    }
    for(const auto& p:p.ports){A ids;for(auto n:p.element_nodes)ids.push_back(n);ports.push_back(O{{"name",p.name},{"width",p.type.width},
        {"input",p.direction==PortDirection::Input},{"nodes",std::move(ids)}});}
    for(const auto& r:p.arithmetic_regions){A members,groups;for(auto n:r.members)members.push_back(n);
        for(const auto& group:r.legal_cut_groups){A ids;for(auto n:group)ids.push_back(n);groups.push_back(std::move(ids));}
        regions.push_back(O{{"template",r.template_version},{"members",std::move(members)},
            {"legal_cut_groups",std::move(groups)},{"internally_cuttable",r.internal_split_enabled}});}
    return O{{"version","prospective-workload-graph-v1"},{"nodes",std::move(nodes)},{"ports",std::move(ports)},{"regions",std::move(regions)}};
}
// Same held-input service and output-capture behavior as the public D shell;
// native emission deliberately grants no operator hierarchy/model identity.
O emitNative(const pipeline::Program& p,const std::filesystem::path& out,double target,const WorkloadPlan* owners=nullptr){
    auto core=p;core.rtl_program.function_name="WorkloadCore";
    auto state=stateCostQuery(p,target,true);const auto& opts=p.options;
    std::vector<std::string> ports{"input wire clk","input wire rstn","input wire iv_data","input wire iv_wen","output reg ov"};
    std::vector<std::string> bindings{"."+opts.input_valid_name+"(liv)","."+opts.output_valid_name+"(cov)"};
    if(opts.register_stages>0){bindings.push_back(".clk(clk)");bindings.push_back(".rstn(rstn)");}
    std::string declarations="reg liv;wire cov;\n",reset="liv<=1'b0;ov<=1'b0;",update="if(iv_wen)liv<=iv_data;ov<=cov;";
    A metadata;
    for(const auto& port:p.combinational.ports){
        if(port.name==opts.input_valid_name||port.name==opts.output_valid_name)continue;
        require(!port.type.isArray()&&port.element_nodes.size()==1,"scalar workload ports required");
        auto name=rtl_names::sanitizeIdentifier(port.name);require(name==port.name,"source ports must be safe RTL identifiers");
        require(name!="clk"&&name!="rstn"&&name!="iv_data"&&name!="iv_wen"&&name!="liv"&&name!="cov"&&name!="ov","reserved shell port");
        auto range="["+std::to_string(port.type.width-1)+":0] ";bool input=port.direction==PortDirection::Input;
        std::vector<std::uint64_t> words;
        if(input){auto found=opts.input_reset_values.find(port.name);if(found!=opts.input_reset_values.end())words=found->second;
            ports.push_back("input wire "+range+name+"_data");ports.push_back("input wire "+name+"_wen");
            declarations+="reg "+range+"l_"+name+";\n";bindings.push_back("."+name+"(l_"+name+")");
            reset+="l_"+name+"<="+std::to_string(port.type.width)+"'h"+hex(words)+";";
            update+="if("+name+"_wen)l_"+name+"<="+name+"_data;";
        }else{for(const auto& slot:state.slots)if(slot.external_capture&&slot.name==port.name)words=slot.reset_values;
            ports.push_back("output reg "+range+name);declarations+="wire "+range+"c_"+name+";\n";
            bindings.push_back("."+name+"(c_"+name+")");reset+=name+"<="+std::to_string(port.type.width)+"'h"+hex(words)+";";update+=name+"<=c_"+name+";";
        }
        metadata.push_back(O{{"name",name},{"width",port.type.width},{"input",input},{"reset_hex",hex(words)}});
    }
    auto rtl=(owners?rtlgen::emitSystemVerilogWithWorkloadOwners(core,*owners):rtlgen::emitSystemVerilog(core))+"\nmodule Workload("+join(ports)+");\n"+declarations+
        "WorkloadCore core("+join(bindings)+");\nalways @(posedge clk)if(!rstn)begin "+reset+"end else begin "+update+"end\nendmodule\n";
    write(out/"workload.sv",rtl);
    return O{{"top","Workload"},{"rtl","workload.sv"},{"sha256",rtlzz::arithmeticHash(rtl)},
        {"ports",std::move(metadata)},{"internal_stages",opts.register_stages},{"L_including_capture",opts.register_stages+1},
        {"additional_input_service_cycles",1},{"model_boundary_qualification",false},
        {"emitter",owners?"unsigned-workload-owners-v2; physical qualification pending":"native diagnostic; hierarchy qualification pending"}};
}
}
int main(int argc,char** argv){try{
    require(argc>=8&&argc<=12,"usage: export REPO SOURCE_REQUEST L LIMB_WIDTH BEAM_WIDTH MAX_EXTENSIONS NEW_OUTPUT [MUL_BLOCK_WIDTH [T_PS [native|owned [MODEL_BINDINGS]]]]");
    auto repo=std::filesystem::absolute(argv[1]),request=std::filesystem::absolute(argv[2]),out=std::filesystem::absolute(argv[7]);
    require(!std::filesystem::exists(out),"preserve existing output");
    auto doc=parse(read(request));require(doc.getAsObject()&&doc.getAsObject()->getObject("source"),"source metadata required");
    const auto& source=*doc.getAsObject()->getObject("source");std::filesystem::path path=text(source,"source");if(path.is_relative())path=request.parent_path()/path;
    require(rtlzz::arithmeticHash(read(path))==text(source,"generated_source_sha256"),"source SHA mismatch");
    const int L=integer(argv[3],1,256),limb=integer(argv[4],0,4096);
    double target=1500.;if(argc>=10){std::string raw(argv[9]);std::size_t used=0;target=std::stod(raw,&used);
        require(used==raw.size()&&std::isfinite(target)&&target>0&&target<1e9,"finite positive numerical T required");}
    const std::string mode=argc>=11?argv[10]:"native";require(mode=="native"||mode=="owned","unknown workload boundary mode");
    O models;std::string model_hash;
    if(argc==12){require(mode=="owned","model bindings require owned boundaries");auto contents=read(argv[11]);auto parsed=parse(contents);require(parsed.getAsObject(),"model bindings must be an object");models=O(*parsed.getAsObject());model_hash=rtlzz::arithmeticHash(contents);}
    BoundedSchedulePolicy policy;policy.beam_width=integer(argv[5],1,4096);policy.max_extensions=integer(argv[6],1,10000000);
    pipelinev2::PipelineConfig config;config.source_name=text(source,"source_label");config.source_text=read(path);config.top_function=text(source,"top");
    config.clang_args={"-std=c++20","-I"+(repo/"vullib").string()};config.use_circt=false;
    pipeline::Options options;options.register_stages=L-1;options.reset_enabled=true;
    options.input_valid_name=text(source,"iv");options.output_valid_name=text(source,"ov");
    for(const auto& entry:*source.getObject("reset"))for(const auto& word:*entry.second.getAsArray())
        options.input_reset_values[entry.first.str()].push_back(std::stoull(word.getAsString()->str(),nullptr,16));
    rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";
    fixed.target_platform="asap7";fixed.target_period_ps=target;fixed.latency=L;fixed.candidate_policy="core-original-v1";
    fixed.platform_profile=(repo/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(repo/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));
    config.output_kind=pipelinev2::OutputKind::Beir;options.arithmetic_request=fixed;
    std::filesystem::create_directories(out);
    auto compiled=pipelinev2::compile(config);
    if(!compiled.ok()){
        write(out/"error.debug.txt",compiled.error_debug_text);
        write(out/"failure.json",json(O{{"status","software_compile_failed"},{"reason",compiled.error},{"new_EDA_calls",0}}));
    }
    require(compiled.ok()&&compiled.beir_program.has_value(),compiled.error);
    const int mul_block=argc>=9?integer(argv[8],0,128):0;
    auto graph=canonicalizeUnsignedMulPrecision(*compiled.beir_program);
    graph=registerOriginalOperators(normalizeSelections(std::move(graph)));
    if(mul_block){std::vector<mul_split::Configuration> choices;
        for(const auto& region:graph.arithmetic_regions)if(region.template_version=="mul-original-v1")
            choices.push_back({region.id,mul_block});
        graph=mul_split::expand(std::move(graph),choices);
    }
    auto split=splitUnsignedLimbs(std::move(graph),limb);
    auto search_start=std::chrono::steady_clock::now();
    auto schedules=boundedSchedules(split.graph,options,policy);
    double search_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-search_start).count();
    std::filesystem::create_directories(out);write(out/"graph.json",json(graphJson(split.graph)));A views;
    for(std::size_t i=0;i<schedules.placements.size();++i){
        const auto directory=std::to_string(i);std::filesystem::create_directories(out/directory);
        auto actual=pipeline::materializeSchedule(split.graph,options,schedules.placements[i]);pipeline::validateProgram(actual);
        A stages;for(int n:actual.stages)stages.push_back(n);
        auto state=stateCostQuery(actual,target,true);std::optional<WorkloadPlan> owners;
        if(mode=="owned"){
            owners=workloadPlan(actual,target,fixed.platform_profile_sha256);
            write(out/directory/"owners.json",json(workloadPlanJson(*owners)));
            WorkloadCostProvider public_costs(actual,*owners,models,false),diagnostic_costs(actual,*owners,models,true);
            write(out/directory/"costs-public.json",public_costs.componentReport(actual.combinational));
            write(out/directory/"costs-diagnostic.json",diagnostic_costs.componentReport(actual.combinational));
        }
        auto artifact=emitNative(actual,out/directory,target,owners?&*owners:nullptr);
        write(out/directory/"state.json",stateCostQueryJson(state));
        views.push_back(O{{"directory",directory},{"placement",std::move(stages)},{"artifact",std::move(artifact)}});
    }
    write(out/"inventory.json",json(O{{"version","workload-software-inventory-v1"},{"source_sha256",text(source,"generated_source_sha256")},
        {"profile_sha256",fixed.platform_profile_sha256},{"L",L},{"II",1},{"split_policy",kLimbSplitPolicy},
        {"input_valid_port",options.input_valid_name},{"output_valid_port",options.output_valid_name},
        {"mul_block_width",mul_block},
        {"boundary_mode",mode},{"model_bindings_sha256",model_hash},
        {"expansions",std::move(split.expansions)},{"search",boundedScheduleReport(schedules,policy)},{"views",std::move(views)},
        {"search_ms",search_ms},
        {"source_role","application_development_not_training_or_independent"},{"diagnostic_T_ps",target},
        {"mul_precision_policy","proven-zero-high-bits; explicit result zero-extension before region registration"},
        {"application_T_L_frozen_by_export",false},{"candidate_PPA_calls",0},{"model_selection_performed",false},{"full_D_ready",false}}));
    std::cout<<"WORKLOAD_EXPORT views="<<schedules.placements.size()<<" extensions="<<schedules.extensions<<"\n";
    return schedules.placements.empty()?2:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
