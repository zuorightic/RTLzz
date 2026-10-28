#include "backend/simple_cost.hpp"
#include <algorithm>
#include <stdexcept>
#include <set>

namespace pred::beir::simple_cost {
namespace {
unsigned h(std::uint64_t n) {unsigned result=0;do{++result;n>>=1;}while(n);return result;}
unsigned effective(const Program& p,const Operand& v) {
    int width=v.type.width;if(width<=0 || v.type.isArray()) throw std::runtime_error("simple cost: scalar value required");
    const ValueFacts* facts=nullptr;
    if(v.kind==OperandKind::Symbol) facts=&p.signal(v.node).value;
    for(int bit=width-1;bit>=0;--bit) {
        auto word=static_cast<std::size_t>(bit/64);auto mask=std::uint64_t(1)<<(bit%64);
        if(v.kind==OperandKind::Literal) {if(word<v.constant.limbs.size() && (v.constant.limbs[word]&mask)) return bit+1;}
        else if(!facts || !facts->valid || word>=facts->known_zero.size() || !(facts->known_zero[word]&mask)) return bit+1;
    }return 1;
}
Operand ref(const Signal& s) {Operand v;v.node=s.id;v.type=s.type;return v;}
}
Cost node(const Program& p,NodeId id) {
    const auto& s=p.signal(id);if(!s.driver) return {};
    const auto& d=*s.driver;const auto& vs=d.operands;
    auto fail=[&]() -> Cost {throw std::runtime_error("simple cost: unsupported or malformed normalized operation at #"+std::to_string(id));};
    // Validate operator dispatch before the transaction-constant exemption.
    switch(d.kind) {
    case OperationKind::Call:case OperationKind::F32MulRNE:return fail();
    case OperationKind::Binary:
        if(vs.size()!=2 || d.op<OpCode::Add || d.op>OpCode::Shr) return fail();break;
    case OperationKind::Unary:
        if(vs.size()!=1 || d.op<OpCode::LogicNot || d.op>OpCode::Neg) return fail();break;
    case OperationKind::AddCarry:if(vs.size()!=3) return fail();break;
    case OperationKind::Ite:if(vs.size()!=3) return fail();break;
    case OperationKind::Case:if(!hasValidCaseShape(d)) return fail();break;
    case OperationKind::Lookup:case OperationKind::ArrayAccess:
    case OperationKind::WriteSlice:case OperationKind::WriteBit:
    case OperationKind::DynamicBitSelect:case OperationKind::DynamicSlice:
        if(vs.size()!=2) return fail();break;
    case OperationKind::DynamicWriteSlice:case OperationKind::DynamicWriteBit:
        if(vs.size()!=3) return fail();break;
    case OperationKind::Assign:case OperationKind::PortRead:case OperationKind::Cast:
    case OperationKind::ZExt:case OperationKind::SExt:case OperationKind::Trunc:
    case OperationKind::Slice:case OperationKind::BitSelect:case OperationKind::Repeat:
    case OperationKind::ReduceOr:case OperationKind::ReduceAnd:case OperationKind::ReduceXor:
        if(vs.size()!=1) return fail();break;
    case OperationKind::Concat:case OperationKind::Aggregate:if(vs.empty()) return fail();break;
    default:return fail();
    }
    if(d.kind==OperationKind::Aggregate) return {};
    if(s.type.isArray()) return fail();
    if(s.value.valid && s.value.constant) return {};
    auto w=effective(p,ref(s));
    auto include=[&](const Operand& v){w=std::max(w,effective(p,v));};
    if(d.kind==OperationKind::Ite || d.kind==OperationKind::Case) {
        w=s.type.width;
        if(d.kind==OperationKind::Ite) {
            if(vs.size()!=3) return fail();include(vs[1]);include(vs[2]);return {1,w};
        }
        if(!hasValidCaseShape(d)) return fail();
        for(std::size_t i=1;i<vs.size();i+=2) include(vs[i]);include(vs.back());
        auto k=(vs.size()+1)/2;return {h(k-1),k==2?w:k*w};
    }
    if(d.kind==OperationKind::Lookup || d.kind==OperationKind::ArrayAccess) {
        // Lookup's operands in normalized BEIR are [table Aggregate, index].
        if(vs.size()!=2 || vs[0].kind!=OperandKind::Symbol) return fail();
        const auto& table=p.signal(vs[0].node);
        if(!table.driver || table.driver->kind!=OperationKind::Aggregate) return fail();
        auto n=table.driver->operands.size();if(!n) return fail();
        const auto& index=vs[1];
        if(index.type.width<=0 || index.type.isArray()) return fail();
        // Same explicit low-bit index cast used by the native RTL emitter.
        auto maxwidth=std::min(static_cast<unsigned>(index.type.width),h(n-1));
        if(maxwidth>=64) return fail();
        std::uint64_t zero=0,one=0;
        if(index.kind==OperandKind::Literal) {auto v=index.constant.toU64();zero=~v;one=v;}
        else if(index.kind==OperandKind::Symbol) {const auto& f=p.signal(index.node).value;
            if(f.valid) {if(!f.known_zero.empty()) zero=f.known_zero[0];if(!f.known_one.empty()) one=f.known_one[0];}}
        std::uint64_t count=0,mask=(std::uint64_t(1)<<maxwidth)-1;
        zero&=mask;one&=mask;
        if((mask&~zero)>=n || (one&zero)) return fail();
        for(std::uint64_t i=0;i<std::min<std::uint64_t>(n,mask+1);++i)
            if(!(i&zero) && (i&one)==one) ++count;
        if(count<2) return fail(); // singleton must have become wiring in normalization
        w=s.type.width;for(const auto& v:table.driver->operands) include(v);
        return {h(count-1),count==2?w:count*w};
    }
    switch(d.kind) {
    case OperationKind::Assign:case OperationKind::PortRead:case OperationKind::Cast:
    case OperationKind::ZExt:case OperationKind::SExt:case OperationKind::Trunc:
    case OperationKind::Slice:case OperationKind::BitSelect:case OperationKind::WriteSlice:
    case OperationKind::WriteBit:case OperationKind::Concat:case OperationKind::Repeat:
    case OperationKind::Aggregate:return {};
    default:break;
    }
    auto operands=vs.size();
    if(d.kind==OperationKind::AddCarry) operands=std::min<std::size_t>(2,operands);
    if(d.kind==OperationKind::Binary && (d.op==OpCode::Shl || d.op==OpCode::Shr)) operands=std::min<std::size_t>(1,operands);
    if(d.kind==OperationKind::DynamicBitSelect || d.kind==OperationKind::DynamicSlice) operands=std::min<std::size_t>(1,operands);
    if(d.kind==OperationKind::DynamicWriteSlice || d.kind==OperationKind::DynamicWriteBit) {
        include(vs[0]);include(vs[2]);
    } else for(std::size_t i=0;i<operands;++i) include(vs[i]);
    if(d.kind==OperationKind::AddCarry) return {h(w),w};
    if(d.kind==OperationKind::Binary || d.kind==OperationKind::Unary) switch(d.op) {
    case OpCode::Mul: {
        if(vs.size()!=2) return fail();auto m=effective(p,vs[0]),n=effective(p,vs[1]);
        return {3*h(std::max({m,n,static_cast<unsigned>(s.type.width)})),std::uint64_t(m)*n};
    }
    case OpCode::Div:case OpCode::Mod:return {std::uint64_t(w)*h(w),std::uint64_t(w)*w};
    case OpCode::Add:case OpCode::Sub:case OpCode::Neg:case OpCode::Eq:case OpCode::Ne:
    case OpCode::Lt:case OpCode::Le:case OpCode::Gt:case OpCode::Ge:return {h(w),w};
    case OpCode::BitAnd:case OpCode::BitOr:case OpCode::BitXor:case OpCode::LogicAnd:
    case OpCode::LogicOr:case OpCode::LogicNot:case OpCode::BitNot:return {1,w};
    case OpCode::Shl:case OpCode::Shr:if(vs.size()==2 && vs[1].kind==OperandKind::Literal) return {};return {h(w),w*h(w)};
    default:return fail();
    }
    switch(d.kind) {
    case OperationKind::DynamicBitSelect:case OperationKind::DynamicSlice:
    case OperationKind::DynamicWriteSlice:case OperationKind::DynamicWriteBit:
    case OperationKind::ReduceOr:case OperationKind::ReduceAnd:case OperationKind::ReduceXor:return {h(w),w*h(w)};
    default:return fail();
    }
}
}
