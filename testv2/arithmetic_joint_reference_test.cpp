#include "arithmetic_joint_reference_fixture.hpp"
int main(int argc,char** argv){try{
    check(argc==4,"root G-model out required");std::filesystem::path root=argv[1],out=argv[3];std::filesystem::create_directories(out);
    auto model=parsed(read(argv[2]));A rows;
    for(const auto& v:*model.getAsObject()->getArray("exact_domains")){
        const auto& d=*v.getAsObject();auto id=d.getString("id")->str();auto source=jointSource(d.getString("kind")->str(),integerArray(d,"input_widths"),integerArray(d,"output_widths"));
        auto p=compileJointReference(root,source);std::ofstream(out/(id+".cpp"))<<source;
        std::ofstream(out/(id+".beir"))<<emitText(p);std::ofstream(out/(id+".sv"))<<rtlgen::emitSystemVerilog(p);
        auto fingerprint=jointCoreFingerprint(p,"iv","ov");
        // Renaming all signals/ports and the function cannot change a price.
        auto renamed=p;renamed.function_name="Renamed";for(auto& s:renamed.signals)s.name="renamed_"+std::to_string(s.id);
        for(auto& port:renamed.ports)if(port.name!="iv"&&port.name!="ov")port.name="port_"+std::to_string(port.element_nodes.front());
        check(fingerprint==jointCoreFingerprint(renamed,"iv","ov"),"joint fingerprint depends on names");
        auto renumbered=p;const auto size=p.signals.size();
        for(const auto& signal:p.signals){auto moved=signal;moved.id=size-1-signal.id;
            if(moved.driver)for(auto& a:moved.driver->operands)if(a.kind==OperandKind::Symbol)a.node=size-1-a.node;
            renumbered.signals[moved.id]=std::move(moved);}
        for(auto& port:renumbered.ports)for(auto& id:port.element_nodes)id=size-1-id;
        check(fingerprint==jointCoreFingerprint(renumbered,"iv","ov"),"joint fingerprint depends on NodeIds");
        auto forged=p;for(auto& signal:forged.signals)signal.value={};
        check(fingerprint==jointCoreFingerprint(forged,"iv","ov"),"joint fingerprint trusts stale value facts");
        rows.push_back(O{{"domain",id},{"fingerprint",fingerprint},{"nodes",p.signals.size()},{"period_ps",*d.getNumber("period_ps")}});
    }
    write(out/"references.json",O{{"version","synthetic-G-native-shape-witness-v1"},{"rows",std::move(rows)},{"PPA_calls",0}});
    std::cout<<"PASS9 synthetic native reference DAGs, name/NodeId invariance and fresh value facts\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
