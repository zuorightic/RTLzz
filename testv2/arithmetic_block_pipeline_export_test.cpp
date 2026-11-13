// Test/export adapter reuses the frozen, functionally checked service emitter.
// Candidate construction and every input graph use the public successor API.
#define main frozen_fused_D_entry_for_block_test
#include "../main/arithmetic_fused_d.cpp"
#undef main
#include "backend/arithmetic_block_pipeline.hpp"

int main(int argc,char** argv){try{
    require(argc==6,"usage: export ROOT SOURCE_REQUEST L POLICY_JSON NEW_OUTPUT");
    std::filesystem::path root=std::filesystem::absolute(argv[1]),request=std::filesystem::absolute(argv[2]),out=std::filesystem::absolute(argv[5]);
    require(!std::filesystem::exists(out),"preserve existing block inventory");
    auto doc=parse(read(request));auto source=*doc.getAsObject()->getObject("source");
    std::filesystem::path filename=text(source,"source");if(filename.is_relative())filename=request.parent_path()/filename;
    require(rtlzz::arithmeticHash(read(filename))==text(source,"generated_source_sha256"),"source request identity mismatch");
    const auto L=integer(argv[3]);require(L>=2&&L<=16,"L must be 2..16");
    auto raw_policy=parse(read(argv[4]));const auto& policy_doc=*raw_policy.getAsObject();
    require(text(policy_doc,"version")==kBlockPipelinePolicy,"unknown structural policy");
    BlockPipelinePolicy policy;policy.block_widths.clear();
    for(const auto& w:*policy_doc.getArray("block_widths")){auto n=w.getAsInteger();require(n.has_value(),"integer block widths required");policy.block_widths.push_back(int(*n));}
    policy.max_structures=std::size_t(*policy_doc.getInteger("max_structures"));
    policy.max_views=std::size_t(*policy_doc.getInteger("max_views"));
    policy.max_assignments=std::size_t(*policy_doc.getInteger("max_assignments"));
    pipelinev2::PipelineConfig config;config.source_name=text(source,"source_label");config.source_text=read(filename);
    config.top_function=text(source,"top");config.clang_args={"-std=c++20","-I"+(root/"vullib").string()};config.use_circt=false;
    config.pipeline.emplace();auto& options=*config.pipeline;options.register_stages=int(L)-1;options.reset_enabled=true;
    options.input_valid_name=text(source,"iv");options.output_valid_name=text(source,"ov");
    for(const auto& entry:*source.getObject("reset"))for(const auto& word:*entry.second.getAsArray())
        options.input_reset_values[entry.first.str()].push_back(std::stoull(word.getAsString()->str(),nullptr,16));
    rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";
    fixed.target_platform="asap7";fixed.target_period_ps=1500;fixed.latency=int(L);fixed.candidate_policy="core-original-v1";
    fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));
    config.arithmetic_request=fixed;options.arithmetic_request=fixed;
    auto compiled=pipelinev2::compile(config);require(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);
    auto wrong_dispatch=fixed;wrong_dispatch.candidate_policy=kBlockPipelinePolicy;bool dispatch_rejected=false;
    try{visitCandidateGraphs(compiled.pipeline_program->combinational,wrong_dispatch,[](const Program&,const std::string&,const std::string&){});}
    catch(const rtlzz::ArithmeticError& e){dispatch_rejected=e.diagnostic.failure_code=="candidate_policy_requires_block_inventory";}
    require(dispatch_rejected,"new policy silently dispatched through predecessor registry");
    auto result=enumerateBlockPipelineViews(compiled.pipeline_program->combinational,options,policy);const auto& inventory=result.conditional;
    std::filesystem::create_directories(out/"graphs");std::filesystem::create_directories(out/"views");
    std::set<std::string> graph_ids,emitted_categories;std::map<std::string,O> descriptors;A rows,emitted;
    std::size_t extra_late_split_cases=0;
    for(std::size_t i=0;i<inventory.views.size();++i){const auto& view=inventory.views[i];
        const auto hash=rtlzz::arithmeticHash(emitText(view.graph));A ports;
        for(const auto& p:view.graph.ports){A ids;for(auto id:p.element_nodes)ids.push_back(id);ports.push_back(O{{"name",p.name},{"width",p.type.width},{"input",p.direction==PortDirection::Input},{"nodes",std::move(ids)}});}
        if(graph_ids.insert(hash).second)write(out/"graphs"/(hash+".json"),json(O{{"graph",sharedFunctionGraphJsonV2(view.graph)},{"ports",A(ports)}}));
        auto materialized=pipeline::materializeSchedule(view.graph,options,view.placement);pipeline::validateProgram(materialized);
        auto state=stateCostQuery(materialized,1500,true);A qrefs,where,counts,reductions;std::vector<NodeId> controls;
        for(const auto& p:view.graph.ports)if(p.name==options.input_valid_name||p.name==options.output_valid_name)controls.push_back(p.element_nodes.front());
        for(const auto& q:operatorQueries(view.graph,view.placement,int(L)-1,1500,fixed.platform_profile_sha256,fixed.operator_boundary_policy,controls)){
            const auto d=json(O(q.descriptor));const auto key=rtlzz::arithmeticHash(d);descriptors.emplace(key,O(q.descriptor));
            qrefs.push_back(O{{"node",q.origin},{"stage",q.stage},{"descriptor_sha256",key}});
        }
        for(int at:view.placement)where.push_back(at);
        std::string signature;bool first_stage_mul_only=true;std::set<int> tree_add_stages;
        for(const auto& r:view.graph.arithmetic_regions)if(r.internal_split_enabled)for(auto n:r.members){
            const auto& s=view.graph.signal(n);if(s.driver&&s.driver->kind==OperationKind::AddCarry)tree_add_stages.insert(view.placement[n]);
        }
        for(int at=0;at<int(L);++at){int mul=0,add=0;
            for(const auto& s:view.graph.signals)if(view.placement[s.id]==at&&s.driver&&!(s.value.valid&&s.value.constant)){
                if(s.driver->kind==OperationKind::Binary&&s.driver->op==OpCode::Mul)++mul;
                if(s.driver->kind==OperationKind::AddCarry||(s.driver->kind==OperationKind::Binary&&s.driver->op==OpCode::Add))++add;
            }
            counts.push_back(O{{"stage",at},{"mul",mul},{"add",add}});signature+="/"+std::to_string(mul)+":"+std::to_string(add);
            if(at==0)first_stage_mul_only&=mul>0&&add==0;
            else first_stage_mul_only&=mul==0&&add>0&&tree_add_stages.count(at);
        }
        for(const auto& r:reductionRegions(view.graph,view.placement,int(L)-1,controls))reductions.push_back(reductionRegionJson(r,1500));
        const auto relative="views/"+std::to_string(i)+".json";
        write(out/relative,json(O{{"view",view.id},{"candidate",view.candidate_id},{"graph_file","graphs/"+hash+".json"},
            {"placement",std::move(where)},{"state_query",parse(stateCostQueryJson(state))},{"queries",std::move(qrefs)},
            {"stage_counts",A(counts)},{"G_regions",std::move(reductions)}}));
        rows.push_back(O{{"view",view.id},{"candidate",view.candidate_id},{"file",relative},{"stage_counts",std::move(counts)}});
        // Include late split-tree cuts even after the ordinary 32-case sample
        // fills. This criterion uses arithmetic-region structure, never a case
        // name or desired winning placement. It affects functional tests only.
        const bool extra=first_stage_mul_only&&L>=3&&extra_late_split_cases<8;
        if((emitted.size()<32||extra)&&emitted_categories.insert(view.candidate_id+signature).second){
            if(emitted.size()>=32)++extra_late_split_cases;
            const auto relative="emitted/"+std::to_string(emitted.size());const auto dest=out/relative;std::filesystem::create_directories(dest);
            auto artifact=emit(materialized,options,1500,dest);
            emitted.push_back(O{{"view_file",rows.back().getAsObject()->getString("file")->str()},{"directory",relative},{"L",int(L)},{"artifact",std::move(artifact)}});
        }
    }
    O pooled;for(auto& [key,d]:descriptors)pooled[key]=std::move(d);write(out/"descriptors.json",json(std::move(pooled)));
    A rejected;for(const auto& r:result.rejected_structures)rejected.push_back(r);
    write(out/"inventory.json",json(O{{"version","block-pipeline-inventory-v1"},{"policy",blockPipelinePolicyJson(policy)},
        {"policy_sha256",rtlzz::arithmeticHash(read(argv[4]))},{"source_sha256",text(source,"generated_source_sha256")},{"L",int(L)},
        {"complete",inventory.complete},{"reason",inventory.reason},{"inventory_sha256",inventory.inventory_sha256},
        {"structures",std::uint64_t(inventory.structures)},{"assignments",std::uint64_t(inventory.assignments)},
        {"dependency_pruned",std::uint64_t(result.dependency_pruned)},{"rejected_structures",std::move(rejected)},
        {"legacy_policy_dispatch_rejected",dispatch_rejected},
        {"views",std::move(rows)},{"emitted",std::move(emitted)},{"model_coverage_granted",false},{"selection_performed",false}}));
    std::cout<<"BLOCK_PIPELINE_EXPORTED structures="<<inventory.structures<<" views="<<inventory.views.size()<<" assignments="<<inventory.assignments<<" complete="<<inventory.complete<<'\n';
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
