#include "backend/arithmetic_joint.hpp"
#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
namespace {
[[noreturn]] void fail(const std::string& s){throw rtlzz::ArithmeticError({"joint_topology","cost_uncovered",s,{},{}});}
std::string constant(const Operand& a){
    std::ostringstream s;s<<"constant/"<<a.type.width<<'/'<<a.signed_view<<'/'<<a.constant.signed_view;
    for(int b=0;b<a.type.width;++b)s<<((std::size_t(b/64)<a.constant.limbs.size()&&(a.constant.limbs[b/64]>>(b%64)&1))?'1':'0');
    return rtlzz::arithmeticHash(s.str());
}
}
std::string jointCoreFingerprint(const Program& source,const std::string& iv,const std::string& ov){
    // Cached frontend facts are not an authority for topology admission.
    // Reconstruct constants from the actual operations before folding aliases.
    MutableProgram rebuilt(source);rebuilt.ensureValueFacts();auto fresh=rebuilt.finish();const auto& p=fresh;
    std::vector<NodeId> inputs,outputs;
    for(const auto& port:p.ports){
        if(port.type.isArray()||port.element_nodes.size()!=1)fail("joint core requires scalar word ports");
        if(port.direction==PortDirection::Input&&port.name!=iv)inputs.push_back(port.element_nodes.front());
        if(port.direction==PortDirection::Output&&port.name!=ov)outputs.push_back(port.element_nodes.front());
    }
    if(inputs.empty()||inputs.size()>5||outputs.empty())fail("joint core port capacity outside registered shells");
    std::vector<int> labels(inputs.size());for(std::size_t i=0;i<labels.size();++i)labels[i]=i;
    std::string best;
    // At most5! root relabelings; no host/source port name enters the witness.
    do{
        std::map<NodeId,std::string> cache;std::set<NodeId> visiting,paid;std::vector<std::string> roots;
        for(std::size_t i=0;i<inputs.size();++i){std::string token="root/"+std::to_string(p.signal(inputs[i]).type.width)+"/"+std::to_string(labels[i]);
            cache[inputs[i]]=rtlzz::arithmeticHash(token);roots.push_back(token);}
        std::function<std::string(NodeId)> get;std::function<std::string(const Operand&)> operand;
        operand=[&](const Operand& a){if(a.kind==OperandKind::Literal)return constant(a);
            if(a.kind!=OperandKind::Symbol)fail("raw port escaped joint root boundary");
            return rtlzz::arithmeticHash(std::to_string(a.type.width)+"/"+std::to_string(a.signed_view)+"/"+get(a.node));};
        get=[&](NodeId id)->std::string{
            if(auto it=cache.find(id);it!=cache.end())return it->second;
            if(!visiting.insert(id).second)fail("cyclic joint core");
            const auto& s=p.signal(id);if(!s.driver)fail("undriven joint core");const auto& d=*s.driver;
            if(s.value.valid&&s.value.constant){Operand a;a.kind=OperandKind::Literal;a.type=s.type;a.constant=s.value.value;visiting.erase(id);return cache[id]=constant(a);}
            if((d.kind==OperationKind::Assign||d.kind==OperationKind::Cast)&&d.operands.size()==1&&
               !d.signed_truncation&&!d.operands[0].signed_view&&sameType(s.type,d.operands[0].type)){
                if(d.operands[0].kind==OperandKind::Symbol){auto token=get(d.operands[0].node);visiting.erase(id);return cache[id]=token;}
                if(d.operands[0].kind==OperandKind::Literal){auto token=constant(d.operands[0]);visiting.erase(id);return cache[id]=token;}
            }
            std::ostringstream key;key<<int(d.kind)<<'/'<<int(d.op)<<'/'<<s.type.width<<'/'<<d.to_width<<'/'<<d.hi<<'/'<<d.lo<<'/'<<d.bit<<'/'<<d.times<<'/'<<d.signed_truncation;
            for(const auto& a:d.operands)key<<'/'<<operand(a);
            paid.insert(id);visiting.erase(id);return cache[id]=rtlzz::arithmeticHash(key.str());
        };
        std::vector<std::string> exits,nodes;
        for(auto id:outputs)exits.push_back(std::to_string(p.signal(id).type.width)+"/"+get(id));
        for(auto id:paid)nodes.push_back(cache.at(id));
        std::sort(exits.begin(),exits.end());std::sort(nodes.begin(),nodes.end());std::sort(roots.begin(),roots.end());
        std::ostringstream text;text<<"typed-joint-DAG-v1";for(const auto& x:roots)text<<"/R:"<<x;
        for(const auto& x:exits)text<<"/Y:"<<x;for(const auto& x:nodes)text<<"/N:"<<x;
        auto token=rtlzz::arithmeticHash(text.str());if(best.empty()||token<best)best=token;
    }while(std::next_permutation(labels.begin(),labels.end()));
    return best;
}
}
