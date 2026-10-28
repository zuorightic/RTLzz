#include "backend/mul_split.hpp"
#include "backend/arithmetic_regions.hpp"
#include "backend/beopt_dce.hpp"
#include <set>
#include <stdexcept>

namespace pred::beir::mul_split {
namespace {
Operand ref(const Program& p, NodeId id) {
    Operand v;v.node=id;v.type=p.signal(id).type;v.text=p.signal(id).name;return v;
}
Operand zero(int width) {
    Operand v;v.kind=OperandKind::Literal;v.type.width=width;v.constant.width=width;v.constant.limbs={0};return v;
}
}
Program expand32(Program p) {
    MutableProgram facts(std::move(p));facts.ensureValueFacts();p=facts.finish();
    std::set<NodeId> owned;
    for(const auto& r:p.arithmetic_regions) owned.insert(r.members.begin(),r.members.end());
    std::vector<NodeId> targets;
    for(const auto& s:p.signals) {
        if(owned.count(s.id) || !s.driver) continue;
        const auto& op=*s.driver;
        if(op.kind!=OperationKind::Binary || op.op!=OpCode::Mul || s.type.width!=64 || s.type.isArray() || op.operands.size()!=2) continue;
        bool eligible=true;
        for(const auto& v:op.operands) {
            if(v.type.width<32 || v.type.isArray() || v.signed_view || v.constant.signed_view) {eligible=false;continue;}
            // BEOPT normalizes Mul operands to the result width. Accept that
            // zero extension only when verified facts prove every bit >=32 zero.
            for(int bit=32;bit<v.type.width;++bit) {
                const auto word=static_cast<std::size_t>(bit/64);const auto mask=std::uint64_t(1)<<(bit%64);
                if(v.kind==OperandKind::Literal) {
                    if(word<v.constant.limbs.size() && (v.constant.limbs[word]&mask)) eligible=false;
                } else if(v.kind==OperandKind::Symbol) {
                    const auto& f=p.signal(v.node).value;
                    if(!f.valid || word>=f.known_zero.size() || !(f.known_zero[word]&mask)) eligible=false;
                } else eligible=false;
            }
        }
        if(eligible) targets.push_back(s.id);
    }
    if(targets.empty()) throw std::runtime_error("mul split: no eligible unsigned 32x32 full64 operation after BEOPT");
    for(auto original:targets) {
        const auto op=*p.signal(original).driver;
        if(op.arithmetic_origin.empty()) throw std::runtime_error("mul split: missing stable operation identity");
        ArithmeticRegion r;r.id=op.arithmetic_origin;r.operation="mul_u32_full64";
        r.semantics="unsigned-32x32-full64-v1";r.template_version="mul32-split16-v1";
        r.sharing_policy="post-BEOPT-Mul-unique-members-DCE-facts-v1";
        r.internal_split_enabled=true;r.split_version="partial-middle-high-groups-v1";
        r.inputs=op.operands;r.legal_cut_groups.resize(3);
        auto add=[&](int group,int width,OperationKind kind,std::vector<Operand> operands,OpCode code=OpCode::None) {
            Signal s;s.id=p.signals.size();s.type.width=width;s.name="mul_split_"+std::to_string(original)+"_"+std::to_string(s.id);
            Operation d;d.kind=kind;d.op=code;d.type=s.type;d.operands=std::move(operands);
            s.driver=d;r.members.push_back(s.id);r.legal_cut_groups[group].push_back(s.id);p.signals.push_back(s);return s.id;
        };
        auto slice=[&](int group,Operand value,int hi,int lo) {
            auto n=add(group,hi-lo+1,OperationKind::Slice,{value});p.signal(n).driver->hi=hi;p.signal(n).driver->lo=lo;return n;
        };
        auto extend=[&](int group,NodeId value,int width) {
            auto n=add(group,width,OperationKind::ZExt,{ref(p,value)});p.signal(n).driver->to_width=width;return n;
        };
        auto carry=[&](int group,NodeId a,NodeId b,int width) {
            return add(group,width,OperationKind::AddCarry,{ref(p,a),ref(p,b),zero(1)});
        };
        auto a0=slice(0,op.operands[0],15,0),a1=slice(0,op.operands[0],31,16);
        auto b0=slice(0,op.operands[1],15,0),b1=slice(0,op.operands[1],31,16);
        auto p0=add(0,32,OperationKind::Binary,{ref(p,a0),ref(p,b0)},OpCode::Mul);
        auto p1=add(0,32,OperationKind::Binary,{ref(p,a0),ref(p,b1)},OpCode::Mul);
        auto p2=add(0,32,OperationKind::Binary,{ref(p,a1),ref(p,b0)},OpCode::Mul);
        auto p3=add(0,32,OperationKind::Binary,{ref(p,a1),ref(p,b1)},OpCode::Mul);
        auto low=slice(0,ref(p,p0),15,0),p0high=slice(0,ref(p,p0),31,16);
        auto p0ext=extend(1,p0high,32);
        auto m0=carry(1,p0ext,p1,33),p2ext=extend(1,p2,33);
        auto mid=carry(1,m0,p2ext,34);
        auto midhigh=slice(2,ref(p,mid),33,16),midext=extend(2,midhigh,32);
        auto high=carry(2,p3,midext,33);
        auto hi32=slice(2,ref(p,high),31,0),mi16=slice(2,ref(p,mid),15,0);
        auto result=add(2,64,OperationKind::Concat,{ref(p,hi32),ref(p,mi16),ref(p,low)});
        r.outputs={result};Operation alias;alias.type=p.signal(original).type;alias.operands={ref(p,result)};
        p.signal(original).driver=alias;p.signal(original).value={};p.arithmetic_regions.push_back(r);
    }
    p.temp_names_initialized=false;
    // Frozen cleanup preserves group boundaries: DCE + static facts only.
    MutableProgram graph(std::move(p));opt::eliminateDeadNodes(graph);graph.ensureValueFacts();
    p=graph.finish();regions::validate(p);return p;
}
}
