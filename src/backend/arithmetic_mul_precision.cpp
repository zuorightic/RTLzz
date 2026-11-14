#include "backend/arithmetic_mul_precision.hpp"
#include "backend/arithmetic_core.hpp"
#include <algorithm>
#include <stdexcept>

namespace pred::beir::arithmetic {
Program canonicalizeUnsignedMulPrecision(Program p){
    if(!p.arithmetic_regions.empty())throw std::runtime_error("Mul precision canonicalization precedes region registration");
    MutableProgram facts(std::move(p));facts.ensureValueFacts();p=facts.finish();
    auto ref=[&](NodeId id){Operand a;a.node=id;a.type=p.signal(id).type;return a;};
    auto append=[&](int width,Operation op){Signal s;s.id=p.signals.size();s.name="mul_precision_"+std::to_string(s.id);
        s.type.width=width;op.type=s.type;s.driver=std::move(op);p.signals.push_back(std::move(s));return p.signals.back().id;};
    const auto count=p.signals.size();
    for(NodeId i=0;i<count;++i){
        auto op=*p.signal(i).driver;
        if(op.kind!=OperationKind::Binary||op.op!=OpCode::Mul)continue;
        const int declared=p.signal(i).type.width;
        int widths[2];
        for(int k=0;k<2;++k){auto& a=op.operands[k];
            if(a.signed_view||a.constant.signed_view||a.type.isArray())throw std::runtime_error("unsigned scalar Mul precision required");
            int width=std::min(declared,a.type.width);
            auto zero=[&](int bit){
                if(a.kind==OperandKind::Literal)return std::size_t(bit/64)>=a.constant.limbs.size()||!((a.constant.limbs[bit/64]>>(bit%64))&1);
                if(a.kind!=OperandKind::Symbol)return false;const auto& f=p.signal(a.node).value;
                return f.valid&&std::size_t(bit/64)<f.known_zero.size()&&((f.known_zero[bit/64]>>(bit%64))&1);
            };
            while(width>1&&zero(width-1))--width;
            widths[k]=width;
            if(width!=a.type.width){Operation slice;slice.kind=OperationKind::Slice;slice.lo=0;slice.hi=width-1;slice.operands={a};
                a=ref(append(width,std::move(slice)));}
        }
        int result=std::min(declared,widths[0]+widths[1]);
        if(result<declared){auto n=append(result,std::move(op));Operation extend;extend.kind=OperationKind::ZExt;
            extend.type=p.signal(i).type;extend.to_width=declared;extend.operands={ref(n)};p.signal(i).driver=std::move(extend);
        }else{op.type=p.signal(i).type;p.signal(i).driver=std::move(op);}
        p.signal(i).value={};
    }
    p.temp_names_initialized=false;MutableProgram done(std::move(p));done.ensureValueFacts();p=done.finish();
    validate(p,"unsigned_mul_precision");return p;
}
}
