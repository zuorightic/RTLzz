#include "backend/arithmetic_limb_split.hpp"
#include "backend/arithmetic_mul_precision.hpp"
#include <llvm/ADT/APInt.h>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
using namespace pred::beir;
using namespace pred::beir::arithmetic;
namespace {
constexpr unsigned Bits=8192;
void check(bool b){if(!b)throw std::runtime_error("unsigned limb/precision arithmetic mismatch");}
Operand ref(const Program& p,NodeId n){Operand a;a.node=n;a.type=p.signal(n).type;return a;}
NodeId add(Program& p,int width,OperationKind kind,std::vector<Operand> args,OpCode code=OpCode::None){
    Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=width;
    Operation d;d.kind=kind;d.op=code;d.type=s.type;d.operands=std::move(args);s.driver=std::move(d);p.signals.push_back(s);return s.id;
}
NodeId port(Program& p,const std::string& name,int width){Operand a;a.kind=OperandKind::Port;a.text=name;a.type.width=width;
    auto n=add(p,width,OperationKind::PortRead,{a});p.signal(n).port_name=name;
    Port x;x.name=name;x.direction=PortDirection::Input;x.type.width=width;x.element_nodes={n};p.ports.push_back(x);p.inputs.push_back(name);return n;
}
NodeId output(Program& p,NodeId value){auto n=add(p,p.signal(value).type.width,OperationKind::Assign,{ref(p,value)});
    p.signal(n).port_name="y";Port x;x.name="y";x.direction=PortDirection::Output;x.type=p.signal(n).type;x.element_nodes={n};p.ports.push_back(x);p.outputs.push_back("y");return n;
}
llvm::APInt evaluate(const Program& p,NodeId out,const std::map<NodeId,llvm::APInt>& inputs){
    std::map<NodeId,llvm::APInt> cache;
    std::function<llvm::APInt(NodeId)> node;
    auto operand=[&](const Operand& a){if(a.kind==OperandKind::Symbol)return node(a.node);
        llvm::APInt result(Bits,0);for(std::size_t i=0;i<a.constant.limbs.size();++i)result|=llvm::APInt(Bits,a.constant.limbs[i]).shl(64*i);return result;};
    node=[&](NodeId n)->llvm::APInt{
        if(auto it=cache.find(n);it!=cache.end())return it->second;
        if(auto it=inputs.find(n);it!=inputs.end())return it->second;
        const auto& s=p.signal(n);const auto& d=*s.driver;std::vector<llvm::APInt> a;
        for(const auto& v:d.operands)a.push_back(operand(v));llvm::APInt value(Bits,0);
        if(d.kind==OperationKind::Assign||d.kind==OperationKind::ZExt||d.kind==OperationKind::Trunc)value=a[0];
        else if(d.kind==OperationKind::Slice)value=a[0].lshr(d.lo);
        else if(d.kind==OperationKind::BitSelect)value=a[0].lshr(d.bit);
        else if(d.kind==OperationKind::Concat){for(std::size_t i=0;i<a.size();++i)value=value.shl(d.operands[i].type.width)|a[i];}
        else if(d.kind==OperationKind::AddCarry)value=a[0]+a[1]+a[2];
        else if(d.kind==OperationKind::Unary&&d.op==OpCode::BitNot)value=~a[0];
        else if(d.kind==OperationKind::Binary){switch(d.op){case OpCode::Add:value=a[0]+a[1];break;case OpCode::Sub:value=a[0]-a[1];break;
            case OpCode::Mul:value=a[0]*a[1];break;case OpCode::BitAnd:value=a[0]&a[1];break;default:throw std::runtime_error("unexpected test operation");}}
        else throw std::runtime_error("unexpected test node");
        value&=llvm::APInt::getLowBitsSet(Bits,s.type.width);cache.emplace(n,value);return value;
    };return node(out);
}
llvm::APInt randomWord(std::mt19937_64& rng,int width){llvm::APInt v(Bits,0);for(int i=0;i<width;i+=64)v|=llvm::APInt(Bits,rng()).shl(i);return v&llvm::APInt::getLowBitsSet(Bits,width);}
}
int main(){try{
    std::mt19937_64 rng(61573);std::size_t checks=0;
    for(auto shape:std::vector<std::vector<int>>{{7,9,17,4},{65,127,129,17},{512,512,513,64},{1025,769,1025,127},{4096,65,33,7}})
    for(int mode=0;mode<3;++mode){
        int wa=shape[0],wb=shape[1],r=shape[2],limb=shape[3];Program p;p.function_name="arithmetic_unit";
        auto a=port(p,"a",wa),b=port(p,"b",wb),c=port(p,"cin",1);
        auto args=std::vector<Operand>{ref(p,a),ref(p,b)};if(mode==2)args.push_back(ref(p,c));
        auto value=add(p,r,mode==2?OperationKind::AddCarry:OperationKind::Binary,args,mode==2?OpCode::None:mode==1?OpCode::Sub:OpCode::Add);
        auto y=output(p,value);auto expanded=splitUnsignedLimbs(p,limb);check(expanded.expansions.size()==1);
        for(int seed=0;seed<40;++seed){auto av=randomWord(rng,wa),bv=randomWord(rng,wb);bool cin=seed%2;
            if(seed==0){av=llvm::APInt::getLowBitsSet(Bits,wa);bv=llvm::APInt(Bits,1);}
            if(seed==1){av=llvm::APInt(Bits,0);bv=llvm::APInt(Bits,1);}
            llvm::APInt expected=mode==1?av-bv:av+bv+llvm::APInt(Bits,mode==2&&cin);
            expected&=llvm::APInt::getLowBitsSet(Bits,r);
            check(evaluate(expanded.graph,y,{{a,av},{b,bv},{c,llvm::APInt(Bits,cin)}})==expected);++checks;
        }
    }
    Program p;p.function_name="masked_mul";auto a=port(p,"a",193),b=port(p,"b",65);
    Operand mask;mask.kind=OperandKind::Literal;mask.type.width=193;mask.constant.width=193;mask.constant.limbs={UINT64_MAX,UINT64_MAX,0,0};
    auto masked=add(p,193,OperationKind::Binary,{ref(p,a),mask},OpCode::BitAnd);
    auto extended=add(p,193,OperationKind::ZExt,{ref(p,b)});
    auto product=add(p,386,OperationKind::Binary,{ref(p,masked),ref(p,extended)},OpCode::Mul);auto y=output(p,product);
    auto canonical=canonicalizeUnsignedMulPrecision(p);
    bool exact=false;for(const auto& s:canonical.signals)if(s.driver->kind==OperationKind::Binary&&s.driver->op==OpCode::Mul)
        exact|=s.type.width==193&&s.driver->operands[0].type.width==128&&s.driver->operands[1].type.width==65;
    check(exact);
    for(int seed=0;seed<40;++seed){auto av=randomWord(rng,193),bv=randomWord(rng,65);
        check(evaluate(canonical,y,{{a,av},{b,bv}})==(av&llvm::APInt::getLowBitsSet(Bits,128))*bv);++checks;}
    std::cout<<"UNSIGNED_LIMB_AND_PRECISION checks="<<checks<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
