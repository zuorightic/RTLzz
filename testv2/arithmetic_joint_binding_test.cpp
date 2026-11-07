#include "arithmetic_joint_reference_fixture.hpp"
#include "backend/mul_split.hpp"
#include <chrono>
#include <set>
#include <tuple>

static rtlzz::ArithmeticRequest request(const std::filesystem::path& root,const std::filesystem::path& manifest,double period){
    rtlzz::ArithmeticRequest r;r.cost_mode="backend-model";r.target_platform="asap7";r.target_period_ps=period;r.latency=2;
    r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));
    r.model_bundle=manifest.string();r.model_bundle_sha256=rtlzz::arithmeticHash(read(manifest));r.candidate_policy="core-original-v1";return r;
}
static Fixture placement(Program p,int form){
    Fixture f;f.p=std::move(p);f.options.register_stages=1;f.options.reset_enabled=true;f.options.input_valid_name="iv";f.options.output_valid_name="ov";
    f.stages.assign(f.p.signals.size(),0);f.service.mode=1;f.service.valid_write_enable="we_data";f.service.external_data_unconditional=f.service.external_valid_unconditional=true;
    for(const auto& port:f.p.ports){if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)f.stages[id]=1;
        else{auto w=port.type.width;std::uint64_t mask=w==64?UINT64_MAX:(std::uint64_t(1)<<w)-1;
            f.options.input_reset_values[port.name]={port.name=="iv"?0:(form==0?0:form==1?mask:0xa935c571ULL&mask)};
            if(port.name!="iv")f.service.data_write_enables[port.name]="we_data";}}
    return f;
}
static Fixture multiplication(int width,int divisor,bool low,int form){
    Program p;p.function_name="ActualInterval";auto a=input(p,width,"a"),b=input(p,width,"b"),iv=input(p,1,"iv");
    auto mul=add(p,low?width:2*width,OperationKind::Binary,{ref(p,a),ref(p,b)},OpCode::Mul);p.signal(mul).driver->arithmetic_origin="registered-real-native-mul";
    output(p,mul,"y");output(p,iv,"ov");p=registerOriginalOperators(std::move(p));auto region=p.arithmetic_regions.front().id;
    p=mul_split::expand(std::move(p),{{region,width/divisor}});auto f=placement(std::move(p),form);return f;
}
static void addmuxWrapper(const pipeline::Program& p,const Fixture& f,const std::filesystem::path& out,const std::string& name){
    std::ofstream(out/(name+"-core.sv"))<<rtlgen::emitSystemVerilog(p);
    auto q=generalStateCostQuery(p,1253,f.service);std::map<std::string,std::uint64_t> reset;
    for(const auto& s:q.slots)if(s.owner=="external_state"&&!s.state.valid)reset[s.state.name]=s.state.reset_values.at(0);
    std::ofstream w(out/(name+"-wrapper.sv"));
    w<<"module Wrapper(input wire clk,rstn,iv,we_data,input wire [31:0] x0,x1,x2,input wire x3,output reg [31:0] value0,value1,output reg ov);\n";
    w<<"reg [31:0] r0,r1,r2;reg r3,lv;wire [31:0] n0,n1;wire nv;\nalways @(posedge clk)if(!rstn)begin lv<=0;";
    for(int j=0;j<4;++j)w<<"r"<<j<<"<="<<(j==3?1:32)<<"'h"<<std::hex<<f.options.input_reset_values.at("x"+std::to_string(j)).at(0)<<std::dec<<";";
    w<<"end else if(we_data)begin lv<=iv;r0<=x0;r1<=x1;r2<=x2;r3<=x3;end\n";
    w<<p.combinational.function_name<<" core(.clk(clk),.rstn(rstn),.x0(r0),.x1(r1),.x2(r2),.x3(r3),.iv(lv),.value0(n0),.value1(n1),.ov(nv));\n";
    w<<"always @(posedge clk)if(!rstn)begin ov<=0;value0<=32'h"<<std::hex<<reset.at("value0")<<";value1<=32'h"<<reset.at("value1")<<std::dec<<";end else begin value0<=n0;value1<=n1;ov<=nv;end\nendmodule\n";
}
int main(int argc,char** argv){try{
    check(argc==4,"root joint-manifest out required");std::filesystem::path root=argv[1],manifest=argv[2],out=argv[3];std::filesystem::create_directories(out);
    auto publication=parsed(read(root/"experiments/arithmetic/evidence/step7_g_component_v2/G/model.json"));A rows,rejections,intervals,materialized;int complete_count=0;
    auto refused=[&](const std::string& name,auto action){bool no=false;try{action();}catch(const rtlzz::ArithmeticError& e){no=e.diagnostic.failure_code=="cost_uncovered";}
        check(no,"missing G/I rejection: "+name);rejections.push_back(name);};
    for(const auto& entry:*publication.getAsObject()->getArray("exact_domains")){
        const auto& d=*entry.getAsObject();auto id=d.getString("id")->str();auto source=jointSource(d.getString("kind")->str(),integerArray(d,"input_widths"),integerArray(d,"output_widths"));
        auto p=compileJointReference(root,source);auto r=request(root,manifest,*d.getNumber("period_ps"));rtlzz::validateArithmeticRequest(r,1);
        auto provider=makeCostProvider(p,r);check(provider->requiresCompletePlacementCost(),"real production factory G/I dispatch");
        for(int form=0;form<3;++form){auto f=placement(p,form);auto q=completePlacementCostQuery(f.p,f.options,f.stages,r.target_period_ps,f.service);
            auto e=evaluateCompletePlacement(f.p,q,*provider);check(e.price.stages.front().qualified_domain==id,"actual recognized domain differs");++complete_count;
            O quantity{{"arithmetic_QD",e.price.stages.front().QD_ps},{"arithmetic_area",e.price.stages.front().combinational_area_um2},
                {"input_service_area",e.price.functional_state_area_um2.at("input_service")},{"native_state_area",e.price.functional_state_area_um2.at("native_state")},
                {"external_state_area",e.price.functional_state_area_um2.at("external_state")},{"shared_physical_and_clock_area",e.price.shared_physical_area_um2}};
            rows.push_back(O{{"domain",id},{"form",form},{"query",parsed(completePlacementCostQueryJson(q))},{"quantities",std::move(quantity)},
                {"evaluation",parsed(completePlacementEvaluationJson(e))}});
            auto stale=q;stale.state.owner_features["native_state"]["data_bits"]+=1;
            refused(id+"/forged-state",[&]{provider->completePlacementCost(f.p,stale);});
            stale=q;stale.stages[0].members.clear();refused(id+"/forged-stage",[&]{provider->completePlacementCost(f.p,stale);});
            auto bad=f.service;bad.mode=0;bad.valid_write_enable="we_valid";
            for(auto& [name,en]:bad.data_write_enables)en="we_"+name;
            refused(id+"/independent-service",[&]{provider->completePlacementCost(f.p,completePlacementCostQuery(f.p,f.options,f.stages,r.target_period_ps,bad));});
            auto deep=f.options;deep.register_stages=2;auto places=f.stages;for(auto& at:places)if(at==1)at=2;
            refused(id+"/unqualified-depth",[&]{provider->completePlacementCost(f.p,completePlacementCostQuery(f.p,deep,places,r.target_period_ps,f.service));});
            if(id=="addmux"){
                auto options=f.options;auto selection_request=r;selection_request.candidate_policy="mul-candidates-v1";
                options.arithmetic_request=selection_request;options.service_state_protocol=std::make_shared<ServiceStateProtocol>(f.service);
                options.cost_search_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
                auto selected=select(f.p,options);auto final=evaluateCompletePlacement(selected.pipeline,r.target_period_ps,f.service,*provider);
                check(final.feasible&&selected.pipeline.report.model_feasible,"actual G production select must be feasible");
                auto projected=completePlacementCostQuery(f.p,options,selected.pipeline.stages,r.target_period_ps,f.service);
                check(evaluateCompletePlacement(f.p,projected,*provider).total_area_um2==final.total_area_um2,"actual final vs projected price mismatch");
                auto name="addmux-"+std::to_string(form);addmuxWrapper(selected.pipeline,f,out,name);
                materialized.push_back(O{{"name",name},{"query",parsed(completePlacementCostQueryJson(completePlacementCostQuery(selected.pipeline,r.target_period_ps,f.service)))},
                    {"evaluation",parsed(completePlacementEvaluationJson(final))},{"selection",parsed(selected.report)},
                    {"L",2},{"II",1},{"moves",selected.pipeline.moves.size()}});
            }
        }
        auto changed=p;bool altered=false;for(auto& s:changed.signals)if(s.driver&&s.driver->kind==OperationKind::Binary&&s.driver->op==OpCode::BitXor){s.driver->op=OpCode::BitOr;altered=true;break;}
        check(altered,"synthetic mutation point");auto f=placement(changed,0);
        refused(id+"/same-width-foreign-operator",[&]{provider->completePlacementCost(f.p,completePlacementCostQuery(f.p,f.options,f.stages,r.target_period_ps,f.service));});
    }
    for(int form=0;form<3;++form){std::set<std::string> seen;
        for(auto shape:std::vector<std::tuple<int,int,bool>>{{16,2,false},{16,4,false},{32,2,false},{32,4,false},{32,2,true},{32,4,true}}){
            auto [width,divisor,low]=shape;auto f=multiplication(width,divisor,low,form);const auto& region=f.p.arithmetic_regions.front();
            auto r=request(root,manifest,width==16?1119:low?1253:1431);auto base=makeCostProvider(f.p,r);auto* provider=dynamic_cast<JointCompositeCostProvider*>(base.get());check(provider,"actual joint provider type");
            int count=region.legal_cut_groups.size();for(int boundary=0;boundary<=count;++boundary){auto places=f.stages;std::set<NodeId> group_members;
                for(int k=0;k<count;++k)for(auto id:region.legal_cut_groups[k]){places[id]=k<boundary?0:1;group_members.insert(id);}
                bool changed=true;for(std::size_t pass=0;changed&&pass<f.p.signals.size();++pass){changed=false;for(const auto& s:f.p.signals)if(!group_members.count(s.id)&&s.driver){
                    int at=places[s.id];for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol)at=std::max(at,places[a.node]);if(at!=places[s.id]){places[s.id]=at;changed=true;}}}
                auto q=completePlacementCostQuery(f.p,f.options,places,r.target_period_ps,f.service);
                for(auto price:provider->intervalPrices(f.p,q))if(seen.insert(price.domain_id).second)intervals.push_back(O{{"form",form},{"price",parsed(exactIntervalPricesJson({price}))}});
                refused("I-no-joint-stage-bypass",[&]{provider->completePlacementCost(f.p,q);});
            }
        }check(seen.size()==42,"all42 genuine typed interval domains priced");
    }
    auto f=placement(compileJointReference(root,jointSource("addmux",{32,32,32,1},{32,32})),0);auto r=request(root,manifest,1253);
    auto bad=r;bad.model_bundle_sha256="bad";refused("manifest-hash",[&]{makeCostProvider(f.p,bad);});
    bad=r;bad.platform_profile_sha256="bad";refused("profile-hash",[&]{makeCostProvider(f.p,bad);});
    bad=r;bad.target_period_ps=1254;auto provider=makeCostProvider(f.p,bad);refused("exact-target",[&]{provider->completePlacementCost(f.p,completePlacementCostQuery(f.p,f.options,f.stages,1254,f.service));});
    write(out/"binding.json",O{{"version","production-joint-G-I-binding-v1"},{"G_complete_queries",complete_count},{"I_queries",intervals.size()},
        {"G_rows",std::move(rows)},{"I_rows",std::move(intervals)},{"actual_D_materializations",std::move(materialized)},
        {"rejections",std::move(rejections)},{"PPA_calls",0},{"full_D_ready",false}});
    std::cout<<"PASS production G27/I126 queries,3 actual D selections/materializations, exact-domain/context refusals\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
