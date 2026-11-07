#include "arithmetic_general_state_fixture.hpp"
int main(int argc,char** argv){try{
    check(argc==4||argc==5,"root, general component manifest, out, optional public cases required");
    std::filesystem::path root=argv[1],manifest=argv[2],out=argv[3];std::filesystem::create_directories(out);
    rtlzz::ArithmeticRequest request;request.cost_mode="backend-model";request.target_platform="asap7";request.target_period_ps=1119;request.latency=2;
    request.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();request.platform_profile_sha256=rtlzz::arithmeticHash(read(request.platform_profile));
    request.model_bundle=manifest.string();request.model_bundle_sha256=rtlzz::arithmeticHash(read(manifest));request.candidate_policy="mul-candidates-v1";
    const auto model=root/"experiments/arithmetic/evidence/step7_general_state_v1/asap7-general-state.json";auto hash=rtlzz::arithmeticHash(read(model));
    A rows,rejections;int actual_rows=0;
    auto refused=[&](const std::string& name,auto action){bool denied=false;std::string reason;
        try{action();}catch(const rtlzz::ArithmeticError& e){denied=e.diagnostic.failure_code=="cost_uncovered";reason=e.diagnostic.failure_reason;}
        check(denied,"missing refusal: "+name);rejections.push_back(O{{"test",name},{"reason",reason}});};
    for(int depth:{1,2})for(int mode:{0,1,2}){
        auto f=fixture(depth,mode);auto af=fixture(depth,mode,3);auto actual=pipeline::build(af.p,af.options);pipeline::validateProgram(actual);
        for(double clock:{1119.,1253.,1431.,3141.,3385.,4162.,4528.,11913.,14897.}){
            auto r=request;r.target_period_ps=clock;r.latency=depth+1;auto provider=makeCostProvider(f.p,r);
            auto q=generalStateCostQuery(f.p,f.options,f.stages,clock,f.service);
            if(depth==1&&mode==0&&clock==1119){write(out/"projected-first-query.json",parsed(generalStateCostQueryJson(q)));write(out/"materialized-first-query.json",parsed(generalStateCostQueryJson(generalStateCostQuery(actual,clock,f.service))));}
            auto price=provider->generalStateCost(f.p,q);
            rows.push_back(O{{"case","projected-N"+std::to_string(depth)+"-mode"+std::to_string(mode)},
                {"query",parsed(generalStateCostQueryJson(q))},{"price",parsed(generalStateCostEstimateJson(price))}});
            GeneralStateModel state(model.string(),hash,r);auto qa=generalStateCostQuery(actual,clock,f.service);
            auto pa=state.price(actual,f.service);
            check(qa.core.materialized_checked,"actual state geometry checked");
            auto projected=generalStateCostQuery(actual.combinational,actual.options,actual.stages,clock,f.service);
            auto pp=state.price(actual.combinational,projected);
            check(pa.layout_sha256==pp.layout_sha256&&pa.owned_state_area_um2==pp.owned_state_area_um2,"materialized and projected state price differ");
            rows.push_back(O{{"case","materialized-N"+std::to_string(depth)+"-mode"+std::to_string(mode)},
                {"query",parsed(generalStateCostQueryJson(qa))},{"price",parsed(generalStateCostEstimateJson(pa))}});++actual_rows;
        }
        wrapper(actual,af,out,"N"+std::to_string(depth)+"-mode"+std::to_string(mode));
    }
    auto f=fixture(2,0);auto provider=makeCostProvider(f.p,request);auto q=generalStateCostQuery(f.p,f.options,f.stages,1119,f.service);
    auto forged=q;forged.owner_features["input_service"]["data_bits"]+=1;refused("forged-owner-count",[&]{provider->generalStateCost(f.p,forged);});
    forged=q;forged.control_features["native_enable_bits_max"]+=1;refused("forged-control-load",[&]{provider->generalStateCost(f.p,forged);});
    forged=q;forged.slots.front().state.reset_values.front()^=1;refused("forged-service-reset",[&]{provider->generalStateCost(f.p,forged);});
    forged=q;forged.slots.back().state.enable_stage=2;refused("forged-external-gating",[&]{provider->generalStateCost(f.p,forged);});
    forged=q;forged.core.placement[f.data.front()]=1;refused("stale-placement",[&]{provider->generalStateCost(f.p,forged);});
    auto protocol=f.service;protocol.external_data_unconditional=false;refused("external-data-valid-gated",[&]{generalStateCostQuery(f.p,f.options,f.stages,1119,protocol);});
    protocol=f.service;protocol.external_valid_unconditional=false;refused("external-valid-gated",[&]{generalStateCostQuery(f.p,f.options,f.stages,1119,protocol);});
    protocol=f.service;protocol.data_write_enables.erase("a");refused("missing-service-owner",[&]{generalStateCostQuery(f.p,f.options,f.stages,1119,protocol);});
    protocol=f.service;protocol.data_write_enables["a"]="we_iv";refused("independent-service-alias",[&]{generalStateCostQuery(f.p,f.options,f.stages,1119,protocol);});
    protocol=f.service;protocol.data_write_enables["a"]="1'b1";refused("constant-service-enable",[&]{generalStateCostQuery(f.p,f.options,f.stages,1119,protocol);});
    protocol=f.service;protocol.valid_write_enable="we;unexpected";refused("invalid-service-identifier",[&]{generalStateCostQuery(f.p,f.options,f.stages,1119,protocol);});
    auto bad=f.options;bad.input_reset_values["iv"]={1};refused("nonzero-valid-reset",[&]{auto x=generalStateCostQuery(f.p,bad,f.stages,1119,f.service);provider->generalStateCost(f.p,x);});
    auto arithmetic=fixture(2,0,1);refused("arithmetic-correlation-needs-G",[&]{provider->generalStateCost(arithmetic.p,generalStateCostQuery(arithmetic.p,arithmetic.options,arithmetic.stages,1119,arithmetic.service));});
    auto ones=fixture(2,0,2);refused("known-one-padding",[&]{provider->generalStateCost(ones.p,generalStateCostQuery(ones.p,ones.options,ones.stages,1119,ones.service));});
    auto r=request;r.target_period_ps=1500;refused("unregistered-target",[&]{makeCostProvider(f.p,r);});r=request;r.target_period_ps=std::numeric_limits<double>::quiet_NaN();refused("nonfinite-target",[&]{makeCostProvider(f.p,r);});
    r=request;r.target_period_ps=1e100;refused("huge-target",[&]{makeCostProvider(f.p,r);});
    r=request;r.model_bundle_sha256="bad";refused("manifest-hash",[&]{makeCostProvider(f.p,r);});r=request;r.platform_profile_sha256="bad";refused("profile-identity",[&]{makeCostProvider(f.p,r);});
    refused("old-state-query-bypass",[&]{provider->stateCost(f.p,q.core);});refused("scalar-state-bypass",[&]{provider->stateArea(1,1,1,1);});
    refused("scalar-boundary-bypass",[&]{provider->dataBudget(true);});refused("joint-G-unqualified",[&]{provider->validateInterface(f.p,"ov");});
    refused("whole-stage-missing",[&]{provider->stageArea(1,1,1);});
    int mutation=0;
    auto mutated=[&](const std::string& label,bool state,auto change){
        auto doc=parsed(read(manifest));auto& b=*doc.getAsObject();
        for(auto& entry:*b.getObject("components")){auto& ref=*entry.second.getAsObject();auto path=std::filesystem::path(ref.getString("path")->str());ref["path"]=(manifest.parent_path()/path).lexically_normal().string();}
        auto& parent=*b.getObject("state_parent");parent["path"]=(manifest.parent_path()/parent.getString("path")->str()).lexically_normal().string();
        if(state){auto value=parsed(read(model));change(*value.getAsObject());auto file=out/("changed-state-"+std::to_string(mutation)+".json");write(file,std::move(value));
            auto& ref=*b.getObject("components")->getObject("general_state");ref["path"]=file.string();ref["sha256"]=rtlzz::arithmeticHash(read(file));}
        else change(b);
        auto file=out/("changed-manifest-"+std::to_string(mutation++)+".json");write(file,std::move(doc));auto request_copy=request;request_copy.model_bundle=file.string();request_copy.model_bundle_sha256=rtlzz::arithmeticHash(read(file));
        refused(label,[&]{makeCostProvider(f.p,request_copy);});
    };
    mutated("false-whole-D-claim",false,[](O& b){b["full_D_ready"]=true;});
    mutated("shared-physical-claim",false,[](O& b){b["shared_physical_area_qualified"]=true;});
    mutated("state-ancestor-identity",false,[](O& b){(*b.getObject("state_parent"))["sha256"]="changed";});
    mutated("state-domain-extension",true,[](O& b){(*b.getObject("domain"))["max_input_data_bits"]=256;});
    mutated("state-stage-identity",true,[](O& b){b["measurement_static_identity_sha256"]="changed";});
    mutated("state-arbitrary-correlation-claim",true,[](O& b){b["arbitrary_arithmetic_source_correlation_qualified"]=true;});
    mutated("state-reserve-change",true,[](O& b){auto* x=b.getObject("stages")->getObject("grt")->getObject("boundary_model")->getObject("control_models")->getObject("native_enable");(*x)["reserve_ps"]=51;});
    auto actual=pipeline::build(f.p,f.options);auto wrong=actual;auto at=std::find_if(wrong.registers.begin(),wrong.registers.end(),[](const auto& x){return !x.is_valid;});
    check(at!=wrong.registers.end(),"materialized data FF required");at->enable=wrong.valid_nodes.back();bool rejected=false;
    try{generalStateCostQuery(wrong,1119,f.service);}catch(const std::exception&){rejected=true;}check(rejected,"actual wrong enable rejected");
    rejections.push_back(O{{"test","actual-wrong-native-enable"},{"reason","materialized protocol mismatch"}});
    // A genuine public compile uses this factory and keeps C/D candidate order.
    pipelinev2::PipelineConfig config;config.source_name="general-S-D.cpp";config.source_text="#include <fixint.hpp>\nInt<32>a,b;Int<64>y;bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid top(){y=a*b;}\n";
    config.top_function="top";config.clang_args={"-std=c++20","-I"+(root/"vullib").string()};config.use_circt=false;config.arithmetic_request=request;
    config.pipeline.emplace();config.pipeline->register_stages=1;config.pipeline->reset_enabled=true;config.pipeline->input_valid_name="iv";config.pipeline->output_valid_name="ov";config.pipeline->input_reset_values={{"a",{0}},{"b",{0}},{"iv",{0}}};
    auto d=pipelinev2::compile(config);check(!d.ok()&&d.arithmetic_report.find("general_components_missing_joint_G_and_shared_physical_qualification")!=std::string::npos,"production D remains blocked by joint G");
    std::ofstream(out/"D-diagnostic.json")<<d.arithmetic_report;
    if(argc==5){auto input=parsed(read(argv[4]));A public_rows;
        for(const auto& item:*input.getAsObject()->getArray("cases")){const auto& entry=*item.getAsObject();
            pipelinev2::PipelineConfig c;c.source_name=entry.getString("source")->str();c.top_function=entry.getString("top")->str();c.clang_args=config.clang_args;c.use_circt=false;
            c.pipeline.emplace();auto& o=*c.pipeline;o.register_stages=*entry.getInteger("N");o.reset_enabled=true;o.input_valid_name=entry.getString("iv")->str();o.output_valid_name=entry.getString("ov")->str();
            for(const auto& reset:*entry.getObject("reset")){std::vector<std::uint64_t> limbs;for(const auto& w:*reset.second.getAsArray())limbs.push_back(std::stoull(w.getAsString()->str(),nullptr,16));o.input_reset_values[reset.first.str()]=limbs;}
            auto compiled=pipelinev2::compile(c);check(compiled.ok()&&compiled.pipeline_program.has_value(),"public pipeline compile");const auto& p=*compiled.pipeline_program;
            ServiceStateProtocol public_protocol;public_protocol.mode=1;public_protocol.valid_write_enable="push__vld__";public_protocol.external_data_unconditional=public_protocol.external_valid_unconditional=true;
            for(const auto& port:p.combinational.ports)if(port.direction==PortDirection::Input&&port.name!=o.input_valid_name)public_protocol.data_write_enables[port.name]="push__vld__";
            auto query=generalStateCostQuery(p,1119,public_protocol);O result{{"case",entry.getString("name")->str()},{"query",parsed(generalStateCostQueryJson(query))}};
            GeneralStateModel state(model.string(),hash,request);try{auto cost=state.price(p,public_protocol);result["status"]="predicted_in_domain";result["price"]=parsed(generalStateCostEstimateJson(cost));}
            catch(const rtlzz::ArithmeticError& e){result["status"]="cost_uncovered";result["reason"]=e.diagnostic.failure_reason;}
            public_rows.push_back(std::move(result));
        }write(out/"public-banks.json",O{{"rows",std::move(public_rows)},{"PPA_calls",0},{"full_D_ready",false}});
    }
    write(out/"queries.json",O{{"version","general-state-binding-v1"},{"rows",std::move(rows)},{"materialized_queries",actual_rows},
        {"rejections",std::move(rejections)},{"PPA_calls",0},{"full_D_ready",false}});
    std::cout<<"PASS general-state production factory, three owners, conditional bounds and explicit protocol guards\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
