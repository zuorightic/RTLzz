// Public offline D prototype: real functions -> placement selection -> RTL.
#include "backend/arithmetic_fused_selection.hpp"
#include "backend/arithmetic_reduction.hpp"
#include "backend/arithmetic_shared_function_v2.hpp"
#include "backend/rtl_names.hpp"
#include "backend/rtlgen.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <sstream>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
namespace {
void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
std::string read(const std::filesystem::path& p){std::ifstream f(p);require(bool(f),"missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
V parse(const std::string& s){auto v=llvm::json::parse(s);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));return std::move(*v);}
void write(const std::filesystem::path& p,const std::string& s){std::ofstream f(p);require(bool(f),"cannot write "+p.string());f<<s;require(bool(f),"failed writing "+p.string());}
std::string json(V v){return llvm::formatv("{0:2}",v).str()+"\n";}
std::string text(const O& o,const char* key){auto s=o.getString(key);require(bool(s),std::string("missing source field ")+key);return s->str();}
double real(const std::string& s){std::size_t used=0;double t=std::stod(s,&used);require(used==s.size()&&std::isfinite(t)&&t>0,"positive finite numerical period required");return t;}
std::size_t integer(const std::string& s){require(!s.empty()&&s.find_first_not_of("0123456789")==std::string::npos,"positive integer required");auto n=std::stoull(s);require(n>0&&n<=1000000,"integer outside finite bound");return std::size_t(n);}
std::string join(const std::vector<std::string>& v){std::string s;for(const auto& x:v){if(!s.empty())s+=",";s+=x;}return s;}
std::string literal(int width,const std::vector<std::uint64_t>& words){std::ostringstream s;s<<width<<"'h"<<std::hex;if(words.empty())return s.str()+"0";for(std::size_t i=words.size();i-->0;){if(i+1!=words.size())s<<std::setfill('0')<<std::setw(16);s<<words[i];}return s.str();}
struct DataPort {std::string semantic,name;int width;std::vector<std::uint64_t> reset;};
O emit(const pipeline::Program& selected,const pipeline::Options& options,double target,const std::filesystem::path& out,bool fused=true){
    const auto& graph=selected.combinational;auto state=stateCostQuery(selected,target,true);std::vector<DataPort> inputs,outputs;A ports;
    for(const auto& p:graph.ports){if(p.name==options.input_valid_name||p.name==options.output_valid_name)continue;
        require(!p.type.isArray()&&p.element_nodes.size()==1,"D shell requires scalar ports");auto name=rtl_names::sanitizeIdentifier(p.name);
        require(name!="clk"&&name!="rstn"&&name!="iv"&&name!="ov"&&name!="iv_data"&&name!="iv_wen","D shell reserved port collision");
        std::vector<std::uint64_t> reset;
        if(p.direction==PortDirection::Input){auto r=options.input_reset_values.find(p.name);if(r!=options.input_reset_values.end())reset=r->second;inputs.push_back({p.name,name,p.type.width,reset});}
        else {auto s=std::find_if(state.slots.begin(),state.slots.end(),[&](const auto& s){return s.external_capture&&s.name==p.name;});require(s!=state.slots.end(),"D output capture reset missing");reset=s->reset_values;outputs.push_back({p.name,name,p.type.width,reset});}
        ports.push_back(O{{"semantic_name",p.name},{"name",name},{"width",p.type.width},{"direction",p.direction==PortDirection::Input?"input":"output"},{"reset_literal",literal(p.type.width,reset)}});
    }
    require(!inputs.empty()&&!outputs.empty(),"D shell requires data inputs and outputs");
    auto renamed=selected;renamed.rtl_program.function_name="D_core";
    write(out/"D_core.sv",fused?rtlgen::emitSystemVerilogWithReductionBoundaries(renamed,reductionRegions(selected.combinational,selected.stages,options.register_stages)):rtlgen::emitSystemVerilogWithArithmeticBoundaries(renamed));
    std::vector<std::string> top={"input wire clk","input wire rstn","input wire iv_data","input wire iv_wen","output wire ov"};
    std::vector<std::string> launch={"input wire clk","input wire rstn","input wire iv_data","input wire iv_wen","output reg iv"};
    std::vector<std::string> capture={"input wire clk","input wire rstn","input wire iv","output reg ov"};
    std::string lr="iv<=1'b0;",lu="if(iv_wen)iv<=iv_data;",cr="ov<=1'b0;",cu="ov<=iv;",body="wire liv,cov;\n";
    std::vector<std::string> lc={".clk(clk)",".rstn(rstn)",".iv_data(iv_data)",".iv_wen(iv_wen)",".iv(liv)"};
    std::vector<std::string> cc={".clk(clk)",".rstn(rstn)","."+rtl_names::sanitizeIdentifier(options.input_valid_name)+"(liv)","."+rtl_names::sanitizeIdentifier(options.output_valid_name)+"(cov)"};
    std::vector<std::string> ec={".clk(clk)",".rstn(rstn)",".iv(cov)",".ov(ov)"};
    for(const auto& p:inputs){auto range="["+std::to_string(p.width-1)+":0] ";
        top.push_back("input wire "+range+p.name+"_data");top.push_back("input wire "+p.name+"_wen");
        launch.push_back("input wire "+range+p.name+"_data");launch.push_back("input wire "+p.name+"_wen");launch.push_back("output reg "+range+p.name);
        lr+=p.name+"<="+literal(p.width,p.reset)+";";lu+="if("+p.name+"_wen)"+p.name+"<="+p.name+"_data;";body+="wire "+range+"l_"+p.name+";\n";
        lc.push_back("."+p.name+"_data("+p.name+"_data)");lc.push_back("."+p.name+"_wen("+p.name+"_wen)");lc.push_back("."+p.name+"(l_"+p.name+")");cc.push_back("."+p.name+"(l_"+p.name+")");
    }
    for(const auto& p:outputs){auto range="["+std::to_string(p.width-1)+":0] ";top.push_back("output wire "+range+p.name);capture.push_back("input wire "+range+p.name+"_d");capture.push_back("output reg "+range+p.name);
        cr+=p.name+"<="+literal(p.width,p.reset)+";";cu+=p.name+"<="+p.name+"_d;";body+="wire "+range+"c_"+p.name+";\n";cc.push_back("."+p.name+"(c_"+p.name+")");ec.push_back("."+p.name+"_d(c_"+p.name+")");ec.push_back("."+p.name+"("+p.name+")");}
    body+="(* keep_hierarchy=1 *) D_Launch launch("+join(lc)+");\n(* keep_hierarchy=1 *) D_core core("+join(cc)+");\n(* keep_hierarchy=1 *) D_Capture external_capture("+join(ec)+");\n";
    std::string shell="(* keep_hierarchy=1 *) module D_Launch("+join(launch)+");always @(posedge clk)if(!rstn)begin "+lr+"end else begin "+lu+"end endmodule\n";
    shell+="(* keep_hierarchy=1 *) module D_Capture("+join(capture)+");always @(posedge clk)if(!rstn)begin "+cr+"end else begin "+cu+"end endmodule\nmodule D("+join(top)+");\n"+body+"endmodule\n";
    write(out/"D.sv",read(out/"D_core.sv")+shell);write(out/"selected.pipeline.txt",pipeline::emitText(selected));
    A emitted_regions;if(fused)for(const auto& r:reductionRegions(selected.combinational,selected.stages,options.register_stages))emitted_regions.push_back(reductionRegionJson(r,target));
    return O{{"top","D"},{"core_top","D_core"},{"rtl_file","D.sv"},{"rtl_sha256",rtlzz::arithmeticHash(read(out/"D.sv"))},{"core_file","D_core.sv"},{"core_sha256",rtlzz::arithmeticHash(read(out/"D_core.sv"))},
        {"emitted_G_regions",std::move(emitted_regions)},{"effective_boundary_policy",fused?kReductionPolicy:"unsigned-op-hierarchy-v1"},
        {"ports",std::move(ports)},{"state_query",parse(stateCostQueryJson(state))},{"service_latency_cycles",1},{"core_latency_cycles",options.register_stages},{"external_capture_cycles",1},
        {"L_excludes_input_service_launch",true},{"service_protocol","independent input writes, held input valid, unconditional output capture"}};
}
}
int main(int argc,char** argv){std::filesystem::path out;try{
    if(argc==2&&std::string(argv[1])=="--help"){
        std::cout<<"rtlzz-fused-d --G-model JSON [--inspect-only yes] --root DIR --source-request JSON --node-model JSON --state-binding JSON --composition-policy JSON --period-ps T --latency L --output-scope prototype-no-reset-timing-v1 --out DIR [--assignment-limit N]\n";return 0;}
    std::map<std::string,std::string> args;const std::set<std::string> allowed={"--root","--source-request","--node-model","--state-binding","--composition-policy","--period-ps","--latency","--output-scope","--out","--assignment-limit","--G-model","--inspect-only"};
    for(int i=1;i<argc;i+=2){require(i+1<argc&&allowed.count(argv[i])&&!args.count(argv[i]),"unknown, repeated or incomplete option");args[argv[i]]=argv[i+1];}
    for(const auto& key:allowed)if(key!="--assignment-limit"&&key!="--inspect-only"&&key!="--G-model")require(args.count(key),"missing "+key);
    require(args["--output-scope"]=="prototype-no-reset-timing-v1","explicit supported D scope required");
    auto root=std::filesystem::absolute(args["--root"]);auto destination=std::filesystem::absolute(args["--out"]);require(!std::filesystem::exists(destination),"preserve existing D output");std::filesystem::create_directories(destination);out=destination;
    auto doc=parse(read(args["--source-request"]));require(doc.getAsObject(),"source request object required");auto row=doc.getAsObject()->getObject("source");require(row,"source metadata missing");
    auto period=real(args["--period-ps"]);auto L=integer(args["--latency"]);require(L>=2&&L<=16,"D latency requires 2..16 including external capture");
    pipelinev2::PipelineConfig c;c.source_name=text(*row,"source_label");c.source_text=read(text(*row,"source"));require(rtlzz::arithmeticHash(*c.source_text)==text(*row,"generated_source_sha256"),"D source hash mismatch");
    c.top_function=text(*row,"top");c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();auto& options=*c.pipeline;
    options.register_stages=int(L)-1;options.reset_enabled=true;options.input_valid_name=text(*row,"iv");options.output_valid_name=text(*row,"ov");
    auto resets=row->getObject("reset");require(resets,"source reset metadata required");for(const auto& item:*resets){auto a=item.second.getAsArray();require(a,"reset word array required");for(const auto& v:*a){auto s=v.getAsString();require(bool(s),"hex reset word required");std::size_t used=0;auto word=std::stoull(s->str(),&used,16);require(used==s->size(),"invalid reset word");options.input_reset_values[item.first.str()].push_back(word);}}
    rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";fixed.target_platform="asap7";
    fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));fixed.latency=int(L);fixed.target_period_ps=period;
    fixed.candidate_policy="core-original-v1";c.arithmetic_request=fixed;auto compiled=pipelinev2::compile(c);require(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);
    options.arithmetic_request=fixed;fixed.candidate_policy="mul-candidates-v1";
    auto inventory=enumerateConditionalViews(compiled.pipeline_program->combinational,options,fixed,args.count("--assignment-limit")?integer(args["--assignment-limit"]):1000000);
    if(args.count("--inspect-only")){
        A rows;
        for(const auto& view:inventory.views){
            auto p=pipeline::materializeSchedule(view.graph,options,view.placement);
            auto regions=reductionRegions(view.graph,view.placement,options.register_stages);
            A rs,where;for(const auto& r:regions)rs.push_back(reductionRegionJson(r,period));for(int at:view.placement)where.push_back(at);
            auto dest=out/view.id;std::filesystem::create_directories(dest);
            auto artifact=emit(p,options,period,dest,args["--inspect-only"]!="original");
            A ports;for(const auto& port:view.graph.ports){A ids;for(auto n:port.element_nodes)ids.push_back(n);ports.push_back(O{{"name",port.name},{"input",port.direction==PortDirection::Input},{"width",port.type.width},{"nodes",std::move(ids)}});}
            rows.push_back(O{{"view",view.id},{"candidate",view.candidate_id},{"regions",std::move(rs)},{"placement",std::move(where)},{"artifact",std::move(artifact)},
                {"graph",sharedFunctionGraphJsonV2(view.graph)},{"ports",std::move(ports)}});
        }
        write(out/"inventory.json",json(O{{"policy",kReductionPolicy},{"views",std::move(rows)},{"discovery_only",true},{"price_qualification",false}}));
        std::cout<<"G_DISCOVERY views="<<inventory.views.size()<<"\n";return 0;
    }
    require(args.count("--G-model"),"G model required for actual D selection");
    auto model=fixed;model.cost_mode="backend-model";model.model_bundle=std::filesystem::absolute(args["--node-model"]).string();model.model_bundle_sha256=rtlzz::arithmeticHash(read(model.model_bundle));
    FusedDContext context;context.G_model=std::filesystem::absolute(args["--G-model"]).string();context.G_model_sha256=rtlzz::arithmeticHash(read(context.G_model));context.S_binding=std::filesystem::absolute(args["--state-binding"]).string();context.S_binding_sha256=rtlzz::arithmeticHash(read(context.S_binding));context.composition_policy=std::filesystem::absolute(args["--composition-policy"]).string();context.composition_policy_sha256=rtlzz::arithmeticHash(read(context.composition_policy));context.output_scope=args["--output-scope"];
    auto selected=selectFusedD(inventory,options,model,context);auto report=parse(selected.report);auto& r=*report.getAsObject();r["source_sha256"]=text(*row,"generated_source_sha256");r["source_metadata_sha256"]=rtlzz::arithmeticHash(read(args["--source-request"]));
    r["platform_profile_sha256"]=fixed.platform_profile_sha256;r["fixed_structural_table_sha256"]=fixed.fixed_cost_table_sha256;
    write(out/"source.cpp",*c.source_text);write(out/"source.json",json(O(*row)));
    if(selected.pipeline)r["artifact"]=emit(*selected.pipeline,options,period,out);
    write(out/"selection.json",json(std::move(report)));std::cout<<"D_OUTPUT status="<<(selected.pipeline?"selected":"cost_uncovered")<<" views="<<inventory.views.size()<<" output="<<out.string()<<"\n";return selected.pipeline?0:2;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(!out.empty()&&std::filesystem::is_directory(out)&&!std::filesystem::exists(out/"selection.json"))write(out/"selection.json",json(O{{"version","fused-function-D-selection-error-v1"},{"selection_status","cost_uncovered"},{"reason",e.what()},{"prototype_D_selection",false},{"full_D_ready",false},{"candidate_PPA_calls",0}}));return 2;}}
