#include "backend/arithmetic_limb_split.hpp"
#include "backend/arithmetic_core.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
Operand reference(const Program& p, NodeId n) {
    Operand a; a.node=n; a.type=p.signal(n).type; return a;
}
Operand bitConstant(bool one) {
    Operand a; a.kind=OperandKind::Literal; a.type.width=1;
    a.constant.width=1; a.constant.limbs={std::uint64_t(one)}; return a;
}
}
LimbSplitResult splitUnsignedLimbs(Program p, int limb) {
    if(limb<0||limb>4096)throw std::runtime_error("limb width must be 0..4096");
    validate(p,"limb_split_input");
    LimbSplitResult result;
    if(!limb){result.graph=std::move(p);return result;}
    std::set<NodeId> protected_nodes;
    for(const auto& r:p.arithmetic_regions)protected_nodes.insert(r.members.begin(),r.members.end());
    const auto original=p.signals.size();
    for(NodeId root=0;root<original;++root){
        const auto op=*p.signal(root).driver;
        const bool sub=op.kind==OperationKind::Binary&&op.op==OpCode::Sub;
        const bool add=op.kind==OperationKind::Binary&&op.op==OpCode::Add;
        if((!sub&&!add&&op.kind!=OperationKind::AddCarry)||protected_nodes.count(root))continue;
        const int width=p.signal(root).type.width;
        if(width<=limb||(p.signal(root).value.valid&&p.signal(root).value.constant))continue;
        if(width>4096)throw std::runtime_error("limb result exceeds 4096-bit software domain");
        if(p.signal(root).type.isArray())throw std::runtime_error("limb split requires scalar arithmetic");
        for(const auto& a:op.operands)if(a.signed_view||a.constant.signed_view||a.type.isArray())
            throw std::runtime_error("limb split requires unsigned scalar operands");
        auto append=[&](int w,OperationKind kind,std::vector<Operand> operands,OpCode code=OpCode::None){
            Signal s;s.id=p.signals.size();s.name="limb_"+std::to_string(root)+"_"+std::to_string(s.id);s.type.width=w;
            Operation d;d.kind=kind;d.op=code;d.type=s.type;d.operands=std::move(operands);s.driver=std::move(d);
            p.signals.push_back(std::move(s));return p.signals.back().id;
        };
        auto resized=[&](Operand a){
            if(a.type.width==width)return a;
            auto n=append(width,a.type.width>width?OperationKind::Trunc:OperationKind::ZExt,{a});
            p.signal(n).driver->to_width=width;return reference(p,n);
        };
        auto lhs=resized(op.operands.at(0)),rhs=resized(op.operands.at(1));
        Operand carry=sub?bitConstant(true):op.kind==OperationKind::AddCarry?op.operands.at(2):bitConstant(false);
        std::vector<Operand> chunks;llvm::json::Array segments;
        for(int lo=0;lo<width;lo+=limb){
            const int w=std::min(limb,width-lo);
            auto slice=[&](Operand a){auto n=append(w,OperationKind::Slice,{a});p.signal(n).driver->lo=lo;p.signal(n).driver->hi=lo+w-1;return reference(p,n);};
            auto a=slice(lhs),b=slice(rhs);
            if(sub)b=reference(p,append(w,OperationKind::Unary,{b},OpCode::BitNot));
            const auto sum=append(w+1,OperationKind::AddCarry,{a,b,carry});
            auto value=append(w,OperationKind::Slice,{reference(p,sum)});
            p.signal(value).driver->lo=0;p.signal(value).driver->hi=w-1;
            chunks.push_back(reference(p,value));
            NodeId next=kInvalidNodeId;
            if(lo+w<width){next=append(1,OperationKind::BitSelect,{reference(p,sum)});p.signal(next).driver->bit=w;carry=reference(p,next);}
            segments.push_back(llvm::json::Object{{"lo",lo},{"width",w},{"sum",sum},{"value",value},
                {"carry_out",next==kInvalidNodeId?llvm::json::Value(nullptr):llvm::json::Value(next)}});
        }
        std::reverse(chunks.begin(),chunks.end());
        const auto joined=append(width,OperationKind::Concat,std::move(chunks));
        Operation alias;alias.type=p.signal(root).type;alias.operands={reference(p,joined)};
        p.signal(root).driver=std::move(alias);p.signal(root).value={};
        result.expansions.push_back(llvm::json::Object{{"origin",root},{"operation",sub?"sub":op.kind==OperationKind::AddCarry?"adc":"add"},
            {"result_width",width},{"limb_width",limb},{"segments",std::move(segments)},
            {"subtraction","a + bitwise_not(b) + 1 modulo declared result width"}});
    }
    p.temp_names_initialized=false;
    MutableProgram facts(std::move(p));facts.ensureValueFacts();p=facts.finish();
    validate(p,"limb_split_output");result.graph=std::move(p);return result;
}
}
