#include "backend/arithmetic_core.hpp"
#include "backend/arithmetic_regions.hpp"
#include "backend/pipeline.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <cmath>
#include <set>
#include <map>
namespace pred::beir::arithmetic {
namespace {
[[noreturn]] void bad(const std::string& phase,const std::string& code,const std::string& text,NodeId id=kInvalidNodeId) {
    throw rtlzz::ArithmeticError({phase,code,text,{},id==kInvalidNodeId?"":std::to_string(id)});
}
Operand ref(const Program& p,NodeId id){Operand v;v.node=id;v.type=p.signal(id).type;return v;}
std::uint64_t elements(const ValueType& t) {
    if(t.width<=0)bad("graph_validation","width_contract_violation","non-positive width");
    std::uint64_t n=1;for(int d:t.array_dims){if(d<=0 || n>UINT64_MAX/static_cast<unsigned>(d))bad("graph_validation","width_contract_violation","invalid/overflowed shape");n*=d;}return n;
}
bool scalar(const ValueType& t){return !t.isArray();}
}
void validate(const Program& p,const std::string& phase,bool circt,const std::vector<std::string>& auxiliary_ports) {
    auto fail=[&](const std::string& code,const std::string& text,NodeId id){
        std::string region;for(const auto& r:p.arithmetic_regions)if(std::count(r.members.begin(),r.members.end(),id))region=r.id;
        throw rtlzz::ArithmeticError({phase,code,text,region,std::to_string(id)});
    };
    std::set<NodeId> fp_members;
    for(const auto& r:p.arithmetic_regions)if(r.template_version=="softfloat3d-bitvector-mul-v1")fp_members.insert(r.members.begin(),r.members.end());
    for(std::size_t id=0;id<p.signals.size();++id) {
        const auto& s=p.signals[id];elements(s.type);
        if(s.id!=id)fail("invalid_expansion_graph","non-dense node identity",id);
        if(!s.driver)fail("invalid_expansion_graph","undriven node",id);
        const auto& d=*s.driver;const auto& v=d.operands;
        if(!sameType(s.type,d.type))fail("width_contract_violation","driver/result type mismatch",id);
        auto need=[&](bool condition,const char* text){if(!condition)fail("width_contract_violation",text,id);};
        auto count=[&](std::size_t n){if(v.size()!=n)fail("invalid_expansion_graph","wrong operand count",id);};
        auto index_bounded=[&](const Operand& index,int maximum) {
            if(maximum<0)fail("width_contract_violation","dynamic selection exceeds data width",id);
            std::uint64_t possible=0;
            for(int bit=0;bit<index.type.width;++bit) {
                bool zero=false;
                if(index.kind==OperandKind::Literal)zero=static_cast<std::size_t>(bit/64)>=index.constant.limbs.size()||!((index.constant.limbs[bit/64]>>(bit%64))&1);
                else if(index.kind==OperandKind::Symbol){const auto& f=p.signal(index.node).value;zero=f.valid&&static_cast<std::size_t>(bit/64)<f.known_zero.size()&&((f.known_zero[bit/64]>>(bit%64))&1);}
                if(!zero){if(bit>=63)fail("template_ineligible","dynamic index range is not proven safe",id);possible|=std::uint64_t(1)<<bit;}
            }
            if(possible>static_cast<unsigned>(maximum))fail("template_ineligible","dynamic index range is not proven safe",id);
        };
        for(const auto& a:v) {
            elements(a.type);
            if(a.kind==OperandKind::Symbol){if(a.node>=p.signals.size())fail("invalid_expansion_graph","dangling reference",id);need(sameType(a.type,p.signal(a.node).type),"reference/type mismatch");}
            else if(a.kind==OperandKind::Literal) {
                need(!a.type.isArray() && a.constant.width==a.type.width,"literal type/width mismatch");
                auto words=(static_cast<std::size_t>(a.type.width)+63)/64;
                need(a.constant.limbs.size()<=words,"too many constant limbs");
                if(a.constant.limbs.size()==words && a.type.width%64)need((a.constant.limbs.back()>>(a.type.width%64))==0,"non-zero constant padding");
            } else if(a.kind==OperandKind::Port){need(d.kind==OperationKind::PortRead,"raw port outside PortRead");}
            else fail("invalid_expansion_graph","unknown operand kind",id);
        }
        if(!fp_members.count(id) && (d.kind==OperationKind::SExt || std::any_of(v.begin(),v.end(),[](const auto& a){return a.signed_view||a.constant.signed_view;})))
            fail("template_ineligible","signed numerical semantics outside the registered binary32 implementation are not in the unsigned core profile",id);
        switch(d.kind) {
        case OperationKind::F32MulRNE:case OperationKind::Call:
            fail("unsupported_backend_op","unexpanded semantic marker or opaque Call",id);
        case OperationKind::Binary:
            count(2);if(d.op<OpCode::Add || d.op>OpCode::Shr)fail("unsupported_backend_op","unsupported binary opcode",id);
            need(scalar(s.type)&&scalar(v[0].type)&&scalar(v[1].type),"binary scalar shape");
            if(d.op>=OpCode::LogicAnd && d.op<=OpCode::Ge)need(s.type.width==1,"boolean result width");break;
        case OperationKind::Unary:
            count(1);if(d.op<OpCode::LogicNot || d.op>OpCode::Neg)fail("unsupported_backend_op","unsupported unary opcode",id);
            need(scalar(s.type)&&scalar(v[0].type),"unary scalar shape");if(d.op==OpCode::LogicNot)need(s.type.width==1,"logical not result width");break;
        case OperationKind::AddCarry:
            count(3);need(scalar(s.type)&&scalar(v[0].type)&&scalar(v[1].type)&&scalar(v[2].type)&&v[2].type.width==1,"AddCarry scalar operands and one-bit carry");break;
        case OperationKind::Ite:
            count(3);need(scalar(v[0].type)&&v[0].type.width==1,"Mux selector must be one bit");
            need(v[1].type.array_dims==s.type.array_dims&&v[2].type.array_dims==s.type.array_dims,"Mux data/result array shape mismatch");break;
        case OperationKind::Case:
            if(!hasValidCaseShape(d))fail("invalid_expansion_graph","invalid ordered Case",id);
            for(std::size_t k=0;k+1<v.size();k+=2){need(v[k].type.width==1&&scalar(v[k].type),"Case selector width");need(v[k+1].type.array_dims==s.type.array_dims,"Case branch shape");}need(v.back().type.array_dims==s.type.array_dims,"Case default shape");break;
        case OperationKind::Lookup:case OperationKind::ArrayAccess:
            count(2);need(v[0].type.isArray()&&scalar(v[1].type),"selection table/index shape");break;
        case OperationKind::Aggregate:
            need(!v.empty()&&s.type.isArray()&&elements(s.type)==v.size(),"Aggregate array shape");
            for(const auto& a:v)need(scalar(a.type)&&a.type.width==s.type.width,"Aggregate element width");break;
        case OperationKind::PortRead:
            if(v.size()!=1&&v.size()!=2)fail("invalid_expansion_graph","PortRead operand count",id);
            need(v[0].kind==OperandKind::Port,"PortRead source kind");
            {auto port=std::find_if(p.ports.begin(),p.ports.end(),[&](const auto& x){return x.name==v[0].text&&x.direction==PortDirection::Input;});
            if(port==p.ports.end())fail("invalid_expansion_graph","PortRead has no declared input port",id);
            need(sameType(port->type,v[0].type),"PortRead declared source shape mismatch");}
            break;
        case OperationKind::Assign:case OperationKind::Cast:
            count(1);need(s.type.array_dims==v[0].type.array_dims,"assign/cast shape mismatch");break;
        case OperationKind::ZExt:case OperationKind::SExt:case OperationKind::Trunc:
            count(1);need(scalar(s.type)&&scalar(v[0].type),"extension scalar shape");
            need(d.to_width==0||d.to_width==s.type.width,"extension target width");
            need(d.kind==OperationKind::Trunc?s.type.width<=v[0].type.width:s.type.width>=v[0].type.width,"extension direction");break;
        case OperationKind::Slice:
            count(1);need(scalar(s.type)&&scalar(v[0].type),"Slice scalar shape");need(d.lo>=0&&d.hi>=d.lo&&d.hi<v[0].type.width&&s.type.width==d.hi-d.lo+1,"Slice range/width");break;
        case OperationKind::BitSelect:
            count(1);need(scalar(s.type)&&scalar(v[0].type),"BitSelect scalar shape");need(d.bit>=0&&d.bit<v[0].type.width&&s.type.width==1,"BitSelect range/width");break;
        case OperationKind::WriteSlice:case OperationKind::WriteBit:
            count(2);need(scalar(s.type)&&scalar(v[0].type)&&scalar(v[1].type),"write scalar shape");need(sameType(s.type,v[0].type),"write base width");
            if(d.kind==OperationKind::WriteBit)need(d.bit>=0&&d.bit<s.type.width&&v[1].type.width==1,"WriteBit range/width");
            else need(d.lo>=0&&d.hi>=d.lo&&d.hi<s.type.width&&v[1].type.width==d.hi-d.lo+1,"WriteSlice range/width");break;
        case OperationKind::Concat: {
            need(!v.empty()&&scalar(s.type),"Concat shape");std::uint64_t width=0;for(const auto& a:v){need(scalar(a.type),"Concat scalar input");width+=a.type.width;}need(width==static_cast<unsigned>(s.type.width),"Concat sum width");break;
        }
        case OperationKind::Repeat:
            count(1);need(scalar(s.type)&&scalar(v[0].type),"Repeat scalar shape");need(d.times>0&&static_cast<std::uint64_t>(v[0].type.width)*d.times==static_cast<unsigned>(s.type.width),"Repeat width");break;
        case OperationKind::ReduceOr:case OperationKind::ReduceAnd:case OperationKind::ReduceXor:
            count(1);need(s.type.width==1&&scalar(s.type)&&scalar(v[0].type),"reduction shape");break;
        case OperationKind::DynamicBitSelect:case OperationKind::DynamicSlice:
            count(2);need(scalar(s.type)&&scalar(v[0].type)&&scalar(v[1].type),"dynamic select scalar shape");
            if(d.kind==OperationKind::DynamicBitSelect)need(s.type.width==1,"dynamic bit width");index_bounded(v[1],v[0].type.width-s.type.width);break;
        case OperationKind::DynamicWriteSlice:case OperationKind::DynamicWriteBit:
            count(3);need(sameType(s.type,v[0].type)&&scalar(v[1].type)&&scalar(v[2].type),"dynamic write shape");
            if(d.kind==OperationKind::DynamicWriteBit)need(v[2].type.width==1,"dynamic bit write width");index_bounded(v[1],v[0].type.width-v[2].type.width);break;
        default:fail("unsupported_backend_op","unregistered BEIR operation kind",id);
        }
        if(d.kind!=OperationKind::Binary&&d.kind!=OperationKind::Unary && d.op!=OpCode::None)
            fail("unsupported_backend_op","opcode attached to non-arithmetic operation",id);
        (void)circt; // Current core lowered op set is shared; CIRCT retains its own final lowering checks.
    }
    for(const auto& port:p.ports) {
        if(port.element_nodes.empty() && std::find(auxiliary_ports.begin(),auxiliary_ports.end(),port.name)!=auxiliary_ports.end()) {
            if(port.direction!=PortDirection::Input||port.type.width!=1||port.type.isArray())bad(phase,"width_contract_violation","invalid auxiliary clock/reset port");
            continue;
        }
        if(port.direction==PortDirection::Unknown||port.element_nodes.size()!=elements(port.type))bad(phase,"width_contract_violation","port shape/element count");
        for(auto id:port.element_nodes)if(id>=p.signals.size()||p.signal(id).type.width!=port.type.width)bad(phase,"width_contract_violation","port node width/reference");
    }
    try{regions::validate(p);}catch(const std::exception& e){bad(phase,"invalid_expansion_graph",e.what());}
    for(const auto& r:p.arithmetic_regions) {
        auto reject=[&](const std::string& message){throw rtlzz::ArithmeticError({phase,"invalid_expansion_graph",message,r.id,{}});};
        if(r.template_version=="mul-original-v1") {
            if(r.operation!="mul_unsigned"||r.semantics!="unsigned-mod-result-v2"||r.internal_split_enabled||r.members.size()!=1||r.outputs!=r.members||r.inputs.size()!=2)
                reject("malformed original Mul region contract");
            const auto& op=*p.signal(r.members[0]).driver;
            if(op.kind!=OperationKind::Binary||op.op!=OpCode::Mul)reject("original Mul region lost its operation");
            const std::map<std::string,int> expected{{"lhs_width",billingWidth(p,op.operands[0])},{"rhs_width",billingWidth(p,op.operands[1])},{"result_width",p.signal(r.outputs[0]).type.width}};
            if(r.template_parameters!=expected)reject("Mul template width provenance mismatch");
            for(std::size_t k=0;k<2;++k) {
                const auto& a=r.inputs[k];const auto& b=op.operands[k];
                if(a.kind!=b.kind||!sameType(a.type,b.type)||a.signed_view!=b.signed_view||
                   (a.kind==OperandKind::Symbol && a.node!=b.node)||
                   (a.kind==OperandKind::Literal && (a.constant.width!=b.constant.width||a.constant.limbs!=b.constant.limbs)))reject("Mul region input mapping mismatch");
            }
        } else if(r.template_version=="softfloat3d-bitvector-mul-v1") {
            if(r.operation!="fp32_mul_rne"||r.semantics!="binary32-rne-tininess-after-canonicalNaN-v1"||r.internal_split_enabled||!r.template_parameters.empty()||r.inputs.size()!=2||r.outputs.size()!=1)
                reject("malformed binary32 atomic region contract");
            for(const auto& v:r.inputs)if(v.type.width!=32||v.type.isArray())reject("binary32 input format mismatch");
            if(p.signal(r.outputs[0]).type.width!=37)reject("binary32 value/flags output format mismatch");
        } else throw rtlzz::ArithmeticError({phase,"template_ineligible","unregistered region template: "+r.template_version,r.id,{}});
    }

}
int billingWidth(const Program& p,const Operand& input) {
    Operand a=input;std::set<NodeId> seen;
    while(a.kind==OperandKind::Symbol && seen.insert(a.node).second && !a.signed_view) {
        const auto& s=p.signal(a.node);if(!s.driver||s.driver->operands.size()!=1)break;
        const auto& d=*s.driver;const auto& v=d.operands[0];
        if((d.kind!=OperationKind::ZExt && d.kind!=OperationKind::Assign && d.kind!=OperationKind::Cast) || v.signed_view || v.constant.signed_view || v.type.width>a.type.width)break;
        a=v;
    }return a.type.width;
}
CostContext costContext(const rtlzz::ArithmeticRequest& r) {
    CostContext c;c.target_platform=r.target_platform;c.platform_profile_sha256=r.platform_profile_sha256;c.cost_mode=r.cost_mode;c.model_bundle_sha256=r.model_bundle_sha256;return c;
}
CostEstimate FixedCostProvider::node(const Program& p,NodeId id) const {
    const auto& s=p.signal(id);const auto& d=*s.driver;CostEstimate c;
    auto uncovered=[&](const std::string& text){c.covered=false;c.reason=text;return c;};
    if(context_.cost_mode!="fixed")return uncovered("fixed provider cannot substitute for the requested model mode");
    switch(d.kind) {
    case OperationKind::Call:case OperationKind::F32MulRNE:case OperationKind::Case:case OperationKind::Lookup:case OperationKind::ArrayAccess:
        return uncovered("operator must be lowered before cost query");
    case OperationKind::Assign:case OperationKind::PortRead:case OperationKind::Cast:case OperationKind::ZExt:case OperationKind::SExt:case OperationKind::Trunc:case OperationKind::Slice:case OperationKind::BitSelect:case OperationKind::WriteSlice:case OperationKind::WriteBit:case OperationKind::Concat:case OperationKind::Repeat:case OperationKind::Aggregate:return c;
    case OperationKind::Binary:
        if(d.op<OpCode::Add || d.op>OpCode::Shr)return uncovered("unknown binary opcode");break;
    case OperationKind::Unary:
        if(d.op<OpCode::LogicNot || d.op>OpCode::Neg)return uncovered("unknown unary opcode");break;
    case OperationKind::AddCarry:case OperationKind::Ite:case OperationKind::ReduceOr:case OperationKind::ReduceAnd:case OperationKind::ReduceXor:case OperationKind::DynamicBitSelect:case OperationKind::DynamicSlice:case OperationKind::DynamicWriteSlice:case OperationKind::DynamicWriteBit:break;
    default:return uncovered("unknown operation kind");
    }
    if(s.type.isArray())return uncovered("non-wiring array operation must lower to a scalar graph");
    if(s.value.valid&&s.value.constant)return c;
    if(d.kind==OperationKind::Binary&&(d.op==OpCode::Mul||d.op==OpCode::Div||d.op==OpCode::Mod)) {
        c.billing_width=std::max(billingWidth(p,d.operands[0]),billingWidth(p,d.operands[1]));
        if(c.billing_width>128)return uncovered("arithmetic input billing width exceeds 128");
        int bucket=1;while(bucket<c.billing_width)bucket*=2;
        static const unsigned mul[]={1,1,2,4,8,16,32,64};unsigned pos=0;for(int b=bucket;b>1;b/=2)++pos;
        c.delay=c.combinational=d.op==OpCode::Mul?mul[pos]:2*bucket;return c;
    }
    bool dynamic=(d.kind>=OperationKind::DynamicBitSelect&&d.kind<=OperationKind::DynamicWriteBit);
    if(d.kind==OperationKind::Binary&&(d.op==OpCode::Shl||d.op==OpCode::Shr)) {
        const auto& shift=d.operands[1];if(shift.kind==OperandKind::Literal||(shift.kind==OperandKind::Symbol&&p.signal(shift.node).value.valid&&p.signal(shift.node).value.constant))return c;
        dynamic=true;
    }
    c.delay=c.combinational=dynamic?2:1;return c;
}
CostEstimate CostProvider::group(const Program& p,const std::vector<NodeId>& members) const {
    std::set<NodeId> ids(members.begin(),members.end());if(ids.size()!=members.size())bad("cost","invalid_expansion_graph","duplicate group member");
    for(const auto& r:p.arithmetic_regions) {
        auto whole=[&](const std::vector<NodeId>& group){auto count=std::count_if(group.begin(),group.end(),[&](auto id){return ids.count(id);});if(count&&count!=static_cast<int>(group.size()))bad("cost","invalid_expansion_graph","cost group cuts an indivisible arithmetic unit");};
        if(r.internal_split_enabled)for(const auto& group:r.legal_cut_groups)whole(group);else whole(r.members);
    }
    auto result=estimateGroup(p,members);
    if(!result.covered)bad("cost","cost_uncovered",result.reason);
    if(!std::isfinite(result.delay)||result.delay<0||!std::isfinite(result.combinational)||result.combinational<0)bad("cost","cost_uncovered","invalid group provider quantity");
    return result;
}
CostEstimate CostProvider::estimateGroup(const Program& p,const std::vector<NodeId>& members) const {
    std::set<NodeId> ids(members.begin(),members.end());
    CostEstimate total;bool units_set=false;std::map<NodeId,double> cache;std::set<NodeId> visiting;
    std::function<double(NodeId)> arrival=[&](NodeId id){if(!ids.count(id))return 0.0;if(cache.count(id))return cache[id];if(!visiting.insert(id).second)bad("cost","invalid_expansion_graph","group cycle",id);
        auto c=node(p,id);if(!c.covered)bad("cost","cost_uncovered",c.reason,id);
        if(!std::isfinite(c.delay)||c.delay<0||!std::isfinite(c.combinational)||c.combinational<0)bad("cost","cost_uncovered","non-finite or negative provider quantity",id);
        if(units_set && (total.delay_unit!=c.delay_unit||total.area_unit!=c.area_unit||total.source!=c.source))bad("cost","cost_uncovered","inconsistent group cost units or source",id);
        total.delay_unit=c.delay_unit;total.area_unit=c.area_unit;total.source=c.source;units_set=true;double before=0;
        for(const auto& a:p.signal(id).driver->operands)if(a.kind==OperandKind::Symbol)before=std::max(before,arrival(a.node));
        if(total.combinational>UINT64_MAX-c.combinational)bad("cost","cost_uncovered","area overflow");total.combinational+=c.combinational;visiting.erase(id);return cache[id]=before+c.delay;};
    for(auto id:members)total.delay=std::max(total.delay,arrival(id));return total;
}
Program registerOriginalMultipliers(Program p) {
    std::set<NodeId> owned;for(const auto& r:p.arithmetic_regions)owned.insert(r.members.begin(),r.members.end());
    const auto size=p.signals.size();for(NodeId id=0;id<size;++id) {
        if(owned.count(id)||!p.signal(id).driver)continue;auto op=*p.signal(id).driver;
        if(op.kind!=OperationKind::Binary||op.op!=OpCode::Mul)continue;
        for(const auto& v:op.operands)if(v.signed_view||v.constant.signed_view)bad("expansion","template_ineligible","unsigned core cannot register signed multiplication",id);
        if(op.arithmetic_origin.empty())bad("expansion","template_ineligible","Mul has no stable pre-optimization semantic occurrence",id);
        ArithmeticRegion r;r.id=op.arithmetic_origin;r.operation="mul_unsigned";r.semantics="unsigned-mod-result-v2";r.template_version="mul-original-v1";r.sharing_policy="post-BEOPT-original-atomic-v1";r.inputs=op.operands;
        r.template_parameters={{"lhs_width",billingWidth(p,op.operands[0])},{"rhs_width",billingWidth(p,op.operands[1])},{"result_width",p.signal(id).type.width}};
        Signal member=p.signal(id);member.id=p.signals.size();member.name="arith_original_"+std::to_string(member.id);member.port_name.clear();member.port_element_index=-1;member.debug.derived_nodes.clear();r.members={member.id};r.outputs=r.members;p.signals.push_back(member);
        Operation alias;alias.type=p.signal(id).type;alias.operands={ref(p,member.id)};p.signal(id).driver=alias;p.signal(id).value={};p.arithmetic_regions.push_back(r);
    }p.temp_names_initialized=false;validate(p,"expansion_validation");return p;
}
Program normalizeSelections(Program p) {
    validate(p,"pre_normalization");MutableProgram facts(std::move(p));facts.ensureValueFacts();p=facts.finish();
    const auto original=p.signals.size();
    for(NodeId id=0;id<original;++id) {
        auto d=*p.signal(id).driver;
        if(d.kind!=OperationKind::Case&&d.kind!=OperationKind::Lookup&&d.kind!=OperationKind::ArrayAccess)continue;
        std::size_t owner=p.arithmetic_regions.size();for(std::size_t r=0;r<p.arithmetic_regions.size();++r)if(std::count(p.arithmetic_regions[r].members.begin(),p.arithmetic_regions[r].members.end(),id))owner=r;
        auto add=[&](Operation op){Signal s;s.id=p.signals.size();s.name="arith_selection_"+std::to_string(s.id);s.type=op.type;s.driver=std::move(op);p.signals.push_back(s);
            if(owner<p.arithmetic_regions.size()) {auto& r=p.arithmetic_regions[owner];if(r.internal_split_enabled)bad("normalization","invalid_expansion_graph","selection lowering after split-group registration",id);r.members.push_back(s.id);}return ref(p,s.id);};
        auto mux=[&](Operand select,Operand yes,Operand no){Operation x;x.kind=OperationKind::Ite;x.type=p.signal(id).type;x.operands={select,yes,no};return add(x);};
        Operand result;
        if(d.kind==OperationKind::Case) {
            result=d.operands.back();for(int k=static_cast<int>(d.operands.size())-3;k>=0;k-=2)result=mux(d.operands[k],d.operands[k+1],result);
        } else {
            const auto& table=d.operands[0];auto index=d.operands[1];
            if(table.kind!=OperandKind::Symbol||!p.signal(table.node).driver||p.signal(table.node).driver->kind!=OperationKind::Aggregate||table.type.array_dims.size()!=1)
                bad("normalization","template_ineligible","selection requires an explicit one-dimensional Aggregate",id);
            auto entries=p.signal(table.node).driver->operands;
            int bits=index.type.width;
            auto knownZero=[&](int bit){if(index.kind==OperandKind::Literal)return !((bit/64<static_cast<int>(index.constant.limbs.size())?index.constant.limbs[bit/64]:0)>>(bit%64)&1);
                if(index.kind!=OperandKind::Symbol)return false;const auto& f=p.signal(index.node).value;return f.valid&&static_cast<std::size_t>(bit/64)<f.known_zero.size()&&((f.known_zero[bit/64]>>(bit%64))&1);};
            while(bits>0&&knownZero(bits-1))--bits;
            if(bits>12||entries.size()>4096)bad("normalization","template_ineligible","selection network exceeds 4096-entry generation limit",id);
            auto size=std::size_t(1)<<bits;
            if(size>entries.size())bad("normalization","template_ineligible","selection has reachable undefined indices; explicit default/wrapper required",id);
            entries.resize(size);
            for(int bit=0;bit<bits;++bit) {
                Operation op;op.kind=OperationKind::BitSelect;op.type.width=1;op.bit=bit;op.operands={index};auto select=add(op);
                std::vector<Operand> next;for(std::size_t k=0;k<entries.size();k+=2)next.push_back(mux(select,entries[k+1],entries[k]));entries=std::move(next);
            }result=entries[0];
        }
        Operation alias;alias.type=p.signal(id).type;alias.operands={result};p.signal(id).driver=alias;p.signal(id).value={};
    }
    p.temp_names_initialized=false;validate(p,"post_normalization");return p;
}
std::string report(const Program& p,const rtlzz::ArithmeticRequest& request,const rtlzz::PipelineReport* schedule,const std::vector<int>& stages,const std::string& rtl,const pipeline::Options* settings) {
    using namespace llvm::json;
    FixedCostProvider costs(costContext(request));Array nodes,regionsJson;
    for(const auto& s:p.signals) {
        Array operands;for(const auto& v:s.driver->operands) {
            Array dims;for(int dim:v.type.array_dims)dims.push_back(dim);Array limbs;for(auto word:v.constant.limbs)limbs.push_back(std::to_string(word));
            operands.push_back(Object{{"kind",static_cast<int>(v.kind)},{"node",v.kind==OperandKind::Symbol?std::to_string(v.node):""},{"width",v.type.width},{"signed",v.signed_view},{"constant_signed",v.constant.signed_view},{"constant_width",v.constant.width},{"dims",std::move(dims)},{"limbs",std::move(limbs)}});
        }
        Array dims;for(int dim:s.type.array_dims)dims.push_back(dim);
        auto cost=costs.node(p,s.id);
        nodes.push_back(Object{{"id",std::to_string(s.id)},{"kind",static_cast<int>(s.driver->kind)},{"opcode",static_cast<int>(s.driver->op)},
            {"width",s.type.width},{"dims",std::move(dims)},{"hi",s.driver->hi},{"lo",s.driver->lo},{"bit",s.driver->bit},{"times",s.driver->times},{"to_width",s.driver->to_width},{"signed_truncation",s.driver->signed_truncation},{"operands",std::move(operands)},{"billing_width",cost.billing_width},{"delay",cost.delay},{"comb",static_cast<std::int64_t>(cost.combinational)},
            {"covered",cost.covered},{"transaction_constant",s.value.valid&&s.value.constant},{"cost_source",cost.source}});
    }
    for(const auto& r:p.arithmetic_regions) {
        Array members,inputs,outputs,groups;for(auto n:r.members)members.push_back(std::to_string(n));for(auto n:r.outputs)outputs.push_back(std::to_string(n));
        for(const auto& v:r.inputs){Array limbs;for(auto word:v.constant.limbs)limbs.push_back(std::to_string(word));inputs.push_back(Object{{"node",v.kind==OperandKind::Symbol?std::to_string(v.node):"literal"},{"width",v.type.width},{"billing_width",billingWidth(p,v)},{"signed_view",v.signed_view},{"constant_signed",v.constant.signed_view},{"constant_width",v.constant.width},{"constant_limbs",std::move(limbs)}});}
        for(const auto& g:r.legal_cut_groups){Array ids;for(auto n:g)ids.push_back(std::to_string(n));groups.push_back(std::move(ids));}
        auto cost=costs.group(p,r.members);Object parameters;for(const auto& kv:r.template_parameters)parameters[kv.first]=kv.second;
        regionsJson.push_back(Object{{"id",r.id},{"operation",r.operation},{"semantic_version",r.semantics},{"template_id",r.template_version},{"parameters",std::move(parameters)},
            {"sharing_policy",r.sharing_policy},{"members",std::move(members)},{"inputs",std::move(inputs)},{"outputs",std::move(outputs)},{"legal_groups",std::move(groups)},
            {"generated",true},{"numerically_validated_by_this_compile",false},{"internally_cuttable",r.internal_split_enabled},{"C_selected",false},{"D_covered",false},{"D_selected",false},{"physically_evaluated",false},
            {"delay",cost.delay},{"comb",static_cast<std::int64_t>(cost.combinational)}});
    }
    Array ports;for(const auto& port:p.ports){Array dims,ids;for(int dim:port.type.array_dims)dims.push_back(dim);for(auto id:port.element_nodes)ids.push_back(std::to_string(id));ports.push_back(Object{{"name",port.name},{"direction",static_cast<int>(port.direction)},{"width",port.type.width},{"dims",std::move(dims)},{"nodes",std::move(ids)}});}
    Object graph{{"nodes",std::move(nodes)},{"regions",std::move(regionsJson)},{"ports",std::move(ports)}};
    Value graphValue(std::move(graph));auto graphIdentity=llvm::formatv("{0}",graphValue).str();auto graphHash=rtlzz::arithmeticHash(graphIdentity);Array levels;for(int n:stages)levels.push_back(n);
    Value levelValue(std::move(levels));Object protocol;
    if(settings) {
        Object resets;for(const auto& entry:settings->input_reset_values){Array limbs;for(auto word:entry.second)limbs.push_back(std::to_string(word));resets[entry.first]=std::move(limbs);}
        protocol=Object{{"register_stages",settings->register_stages},{"clock_name",settings->clock_name},{"reset_enabled",settings->reset_enabled},{"reset_name",settings->reset_name},{"reset_active_low",settings->reset_active_low},
            {"input_valid_name",settings->input_valid_name},{"output_valid_name",settings->output_valid_name},{"input_reset_values",std::move(resets)}};
    }
    Value protocolValue(std::move(protocol));Value scheduleValue(Object{{"stages",levelValue},{"protocol",protocolValue}});
    auto stageIdentity=llvm::formatv("{0}",scheduleValue).str();auto stageHash=rtlzz::arithmeticHash(graphHash+stageIdentity);
    Object result{{"version","arithmetic-structure-report-v2"},{"backend_contract",kBackendContract},{"status","passed"},{"graph",std::move(graphValue)},
        {"graph_identity_text",graphIdentity},{"schedule_identity_text",stageIdentity},{"graph_sha256",graphHash},{"schedule_sha256",stageHash},{"rtl_sha256",rtlzz::arithmeticHash(rtl)},{"stages",std::move(levelValue)},{"protocol",std::move(protocolValue)},
        {"request",Object{{"target_platform",request.target_platform},{"platform_profile_sha256",request.platform_profile_sha256},{"cost_mode",request.cost_mode},{"cost_source","fixed-op-width-v1"},
            {"cost_source_sha256",request.fixed_cost_table_sha256},{"model_bundle_sha256",request.model_bundle_sha256},{"candidate_policy",request.candidate_policy},{"semantic_profile",request.semantic_profile},
            {"L",request.latency},{"II",request.initiation_interval},{"target_period_ps",request.target_period_ps}}},
        {"delay_unit","abstract"},{"comb_unit","abstract"},{"state_unit","saved_bits"},{"D_inference_implemented",false},{"cost_query_context",Object{{"target_platform",costs.context().target_platform},{"platform_profile_sha256",costs.context().platform_profile_sha256},{"evaluation_stage",costs.context().evaluation_stage},{"feature_schema",costs.context().feature_schema},{"stage_index",costs.context().stage_index},{"model_bundle_sha256",costs.context().model_bundle_sha256}}},
        {"capabilities",Object{{"original_unsigned_mul","generated; atomic"},{"binary32_mul_rne","generated; atomic"},{"mul_half_quarter","reserved_step5"},{"backend_model","reserved_step7; requests fail closed"},{"AES_Sigmoid_fixedpoint","reserved; requests rejected"}}}};
    if(schedule)result["pipeline"]=Object{{"baseline_delay",schedule->baseline_delay},{"allowed_delay",schedule->allowed_delay},{"final_delay",schedule->final_delay},{"comb",static_cast<std::int64_t>(schedule->combinational_cost)},
        {"register_bits",schedule->register_bits},{"register_count",static_cast<std::uint64_t>(schedule->register_count)}};
    return llvm::formatv("{0:2}",Value(std::move(result))).str()+"\n";
}
}
