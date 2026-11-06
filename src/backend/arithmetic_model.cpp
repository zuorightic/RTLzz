#include "backend/arithmetic_model.hpp"
#include "backend/arithmetic_primitives.hpp"
#include "backend/arithmetic_composite.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <functional>

namespace pred::beir::arithmetic {
namespace {
using Object=llvm::json::Object;using Value=llvm::json::Value;using Array=llvm::json::Array;
[[noreturn]] void fail(const std::string& text){throw rtlzz::ArithmeticError({"cost","cost_uncovered",text,{},{}});}
double number(const Object& o,const char* key){auto v=o.getNumber(key);if(!v||!std::isfinite(*v))fail(std::string("invalid model number: ")+key);return *v;}
std::string string(const Object& o,const char* key){auto v=o.getString(key);if(!v)fail(std::string("missing model string: ")+key);return v->str();}
const Object& object(const Object& o,const char* key){auto v=o.getObject(key);if(!v)fail(std::string("missing model object: ")+key);return *v;}
const Array& array(const Object& o,const char* key){auto v=o.getArray(key);if(!v)fail(std::string("missing model array: ")+key);return *v;}
using Features=std::map<std::string,double>;
double predict(const Object& predictor,const Features& f){
    std::vector<double> x;std::set<std::string> names;for(const auto& v:array(predictor,"features")){auto name=v.getAsString();if(!name||!f.count(name->str())||!names.insert(name->str()).second)fail("unknown or duplicate physical model feature");x.push_back(f.at(name->str()));}
    auto kind=string(predictor,"kind");double value=0;
    if(kind=="linear"){
        const auto& coefficients=array(predictor,"coefficients");if(coefficients.size()!=x.size())fail("coefficient/feature shape mismatch");
        value=number(predictor,"intercept");for(std::size_t i=0;i<x.size();++i){auto c=coefficients[i].getAsNumber();if(!c||!std::isfinite(*c))fail("invalid model coefficient");value+=*c*x[i];}
    }else if(kind=="tree"){
        const auto& nodes=array(predictor,"nodes");std::set<std::int64_t> seen;std::int64_t at=0;
        for(;;){if(at<0||static_cast<std::size_t>(at)>=nodes.size()||!seen.insert(at).second)fail("invalid/cyclic portable tree");auto node=nodes[at].getAsObject();if(!node)fail("invalid portable tree node");
            if(node->get("value")){value=number(*node,"value");break;}
            auto index=node->getInteger("feature");if(!index||*index<0||static_cast<std::size_t>(*index)>=x.size())fail("invalid tree feature index");
            auto child=node->getInteger(x[*index]<=number(*node,"threshold")?"left":"right");if(!child)fail("invalid tree child");at=*child;
        }
    }else fail("unknown portable predictor");
    if(!std::isfinite(value))fail("nonfinite model prediction");return std::max(0.0,value);
}
bool zero(const Operand& a){return a.kind==OperandKind::Literal&&std::all_of(a.constant.limbs.begin(),a.constant.limbs.end(),[](auto v){return v==0;});}
std::string literalKind(const Operand& a){
    if(a.kind!=OperandKind::Literal)return "dynamic";
    if(zero(a))return "zero";
    bool one=true,ones=true;for(int bit=0;bit<a.type.width;++bit){bool v=static_cast<std::size_t>(bit/64)<a.constant.limbs.size()&&((a.constant.limbs[bit/64]>>(bit%64))&1);one&=v==(bit==0);ones&=v;}
    return one?"one":ones?"ones":"other";
}
bool wire(const Operation& d){switch(d.kind){
case OperationKind::Assign:case OperationKind::PortRead:case OperationKind::Cast:case OperationKind::ZExt:case OperationKind::SExt:case OperationKind::Trunc:case OperationKind::Slice:case OperationKind::BitSelect:case OperationKind::WriteSlice:case OperationKind::WriteBit:case OperationKind::Concat:case OperationKind::Repeat:case OperationKind::Aggregate:return true;
default:return d.kind==OperationKind::Binary&&(d.op==OpCode::Shl||d.op==OpCode::Shr)&&d.operands.size()==2&&d.operands[1].kind==OperandKind::Literal;}}
int zeroBits(const Program& p,const Operand& a){
    int count=0;for(int bit=0;bit<billingWidth(p,a);++bit){bool z=false;if(a.kind==OperandKind::Literal)z=static_cast<std::size_t>(bit/64)>=a.constant.limbs.size()||!((a.constant.limbs[bit/64]>>(bit%64))&1);
        else if(a.kind==OperandKind::Symbol){const auto& f=p.signal(a.node).value;z=f.valid&&static_cast<std::size_t>(bit/64)<f.known_zero.size()&&((f.known_zero[bit/64]>>(bit%64))&1);}count+=z;}return count;
}
bool knownOne(const Program& p,const Operand& a){
    if(a.kind!=OperandKind::Symbol)return false;const auto& f=p.signal(a.node).value;
    if(!f.valid)return false;for(int bit=0;bit<billingWidth(p,a);++bit)if(static_cast<std::size_t>(bit/64)<f.known_one.size()&&((f.known_one[bit/64]>>(bit%64))&1))return true;return false;
}
void validateDataSources(const Program& p,const std::vector<Operand>& inputs){
    std::set<NodeId> seen;std::function<void(const Operand&)> visit=[&](const Operand& a){
        if(a.kind!=OperandKind::Symbol||!seen.insert(a.node).second)return;const auto& d=*p.signal(a.node).driver;
        if(d.kind==OperationKind::SExt||d.kind==OperationKind::Repeat||d.kind==OperationKind::WriteBit||d.kind==OperationKind::WriteSlice)fail("unvalidated arithmetic input wiring pattern");
        if(d.kind==OperationKind::Concat){unsigned dynamic=0;for(const auto& v:d.operands)dynamic+=v.kind!=OperandKind::Literal;if(dynamic>1)fail("unvalidated multi-source arithmetic concatenation");}
        if(wire(d))for(const auto& v:d.operands)visit(v);
    };
    for(const auto& a:inputs)visit(a);
    auto root=[&](Operand a){std::set<NodeId> chain;while(a.kind==OperandKind::Symbol&&chain.insert(a.node).second){const auto& d=*p.signal(a.node).driver;if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast&&d.kind!=OperationKind::ZExt)||d.operands.size()!=1)break;a=d.operands[0];}return a;};
    if(inputs.size()==2){auto a=root(inputs[0]),b=root(inputs[1]);if(a.kind==OperandKind::Symbol&&b.kind==OperandKind::Symbol&&a.node==b.node)fail("unvalidated identical arithmetic operands");}
    // The current microbenchmarks have independent data operands. Shared
    // primary-input bits/derived correlations require their own support domain.
    std::set<NodeId> previous;
    for(const auto& input:inputs){
        std::set<NodeId> leaves,walked;std::function<void(const Operand&)> gather=[&](const Operand& a){
            if(a.kind!=OperandKind::Symbol||!walked.insert(a.node).second)return;
            const auto& d=*p.signal(a.node).driver;if(d.kind==OperationKind::PortRead){leaves.insert(a.node);return;}
            for(const auto& v:d.operands)gather(v);
        };gather(input);
        for(auto node:leaves)if(previous.count(node))fail("unvalidated correlated arithmetic operands");
        previous.insert(leaves.begin(),leaves.end());
    }
}
}
struct ModelCostProvider::Impl {
    Value document{nullptr};const Program* program=nullptr;rtlzz::ArithmeticRequest request;
    std::vector<unsigned> fanout;std::vector<int> fp_owner,interval_owner;
    std::map<NodeId,CostEstimate> estimates;
    const Object& bundle() const {auto p=document.getAsObject();if(!p)fail("invalid model bundle");return *p;}
    const Object& stage() const {return object(object(bundle(),"stages"),"grt");}
};
ModelCostProvider::ModelCostProvider(const Program& p,const rtlzz::ArithmeticRequest& r):CostProvider(costContext(r)),impl_(std::make_shared<Impl>()){
    auto file=llvm::MemoryBuffer::getFile(r.model_bundle);if(!file)fail("model bundle unavailable");auto bytes=(*file)->getBuffer().str();
    if(rtlzz::arithmeticHash(bytes)!=r.model_bundle_sha256)fail("model bundle content identity mismatch");
    auto value=llvm::json::parse(bytes);if(!value)fail("invalid model JSON");impl_->document=std::move(*value);impl_->request=r;impl_->program=&p;
    const auto& b=impl_->bundle();if(string(b,"version")!="arithmetic-backend-model-v1"||string(b,"feature_schema")!="arithmetic-physical-features-v2"||string(b,"target_platform")!=r.target_platform||string(b,"platform_profile_sha256")!=r.platform_profile_sha256||string(b,"quality_status")!="passed")fail("model platform/profile/schema/quality mismatch");
    context_.feature_schema="arithmetic-physical-features-v2";
    impl_->fanout.assign(p.signals.size(),0);impl_->fp_owner.assign(p.signals.size(),-1);
    impl_->interval_owner.assign(p.signals.size(),-1);
    for(const auto& s:p.signals)if(s.driver)for(const auto& v:s.driver->operands)if(v.kind==OperandKind::Symbol)++impl_->fanout.at(v.node);
    for(std::size_t i=0;i<p.arithmetic_regions.size();++i)if(p.arithmetic_regions[i].template_version=="softfloat3d-bitvector-mul-v1")for(auto id:p.arithmetic_regions[i].members)impl_->fp_owner.at(id)=static_cast<int>(i);
    if(auto model=impl_->stage().getObject("mul_interval_model")){
        const auto schema=string(*model,"version");
        if((schema!="arithmetic-mul-group-interval-model-v1"&&schema!="arithmetic-mul-group-interval-target-load-v1")||string(*model,"quality_status")!="passed")fail("template interval model is not qualified");
        if(schema=="arithmetic-mul-group-interval-target-load-v1"){
            const auto& joint=object(b,"joint_model_context");
            if(r.target_period_ps<number(joint,"sdc_min_ps")||r.target_period_ps>number(joint,"sdc_max_ps")||number(b,"mapping_abc_target_ps")!=number(joint,"abc_target_ps"))fail("outside joint ordinary/state/boundary target coverage");
            if(string(*model,"io_policy")!="proportional_0.2T")fail("unsupported target/load IO policy");
        }
        for(std::size_t i=0;i<p.arithmetic_regions.size();++i){const auto& region=p.arithmetic_regions[i];if(region.template_version!="mul-block-tree-v1")continue;
            arithmetic::validate(p,"model_template_identity");
            const auto& t=region.template_parameters;const int w=t.at("lhs_width"),block=t.at("block_width"),result=t.at("result_width");
            if(w!=t.at("rhs_width")||w%4||w<number(*model,"min_width")||w>number(*model,"max_width")||(block!=w/2&&block!=w/4)||(result!=w&&result!=2*w))fail("outside registered template interval geometry");
            std::set<NodeId> own(region.members.begin(),region.members.end());std::map<NodeId,int> levels;std::set<NodeId> visiting;
            std::function<int(NodeId)> level=[&](NodeId id){
                if(!own.count(id))return 0;if(levels.count(id))return levels.at(id);if(!visiting.insert(id).second)fail("cyclic measured template grouping");
                const auto& d=*p.signal(id).driver;int value=0;
                if(d.kind==OperationKind::ZExt)value=1+level(d.operands.at(0).node);
                else if(d.kind==OperationKind::AddCarry)value=std::max(level(d.operands.at(0).node),level(d.operands.at(1).node));
                else if(d.kind==OperationKind::Slice&&d.operands.at(0).kind==OperandKind::Symbol&&own.count(d.operands[0].node))value=level(d.operands[0].node);
                else if(d.kind!=OperationKind::Slice&&d.kind!=OperationKind::Concat&&!(d.kind==OperationKind::Binary&&d.op==OpCode::Mul))fail("unregistered measured template group operation");
                visiting.erase(id);levels[id]=value;return value;
            };
            for(std::size_t g=0;g<region.legal_cut_groups.size();++g)for(auto id:region.legal_cut_groups[g])if(level(id)!=static_cast<int>(g))fail("template legal groups differ from measured reduction levels");
            validateDataSources(p,region.inputs);
            for(const auto& input:region.inputs)if(input.kind==OperandKind::Literal||zeroBits(p,input)||knownOne(p,input))fail("unvalidated original template input constant pattern");
            for(auto id:region.members)impl_->interval_owner.at(id)=static_cast<int>(i);
        }
    }
}
CostEstimate ModelCostProvider::node(const Program& p,NodeId id) const {
    if(&p!=impl_->program)fail("model provider queried with another graph");if(impl_->estimates.count(id))return impl_->estimates.at(id);
    CostEstimate c;c.delay_unit="ps";c.area_unit="um2";c.source=string(impl_->bundle(),"id");c.state_per_bit=number(object(impl_->stage(),"state_model"),"bit_area_um2");
    const auto& signal=p.signal(id);const auto& d=*signal.driver;std::string op;int lhs=0,rhs=0,lz=0,rz=0;
    if(signal.value.valid&&signal.value.constant)return c;
    if(impl_->interval_owner[id]>=0){
        // Every physical amount is returned by the actual stage interval.
        // Wiring is kept in the graph; arithmetic is explicitly deferred.
        c.deferred_to_stage=!wire(d);c.physical_operation=c.deferred_to_stage?"mul-template-stage-member":"";
        return c;
    }
    if(impl_->fp_owner[id]>=0){const auto& r=p.arithmetic_regions[impl_->fp_owner[id]];if(r.outputs.front()!=id)return c;op="fp32";lhs=rhs=32;}
    else {
        if(wire(d)){
            if(d.kind==OperationKind::SExt||d.kind==OperationKind::Repeat||d.kind==OperationKind::WriteBit||d.kind==OperationKind::WriteSlice||
               (d.kind==OperationKind::Trunc&&d.signed_truncation))fail("unvalidated output/state wiring pattern");
            if(d.kind==OperationKind::Concat){unsigned sources=0;for(const auto& v:d.operands)sources+=v.kind!=OperandKind::Literal;if(sources>1)fail("unvalidated multi-source output/state concatenation");}
            return c;
        }
        if(d.operands.empty())fail("physical operation has no operand");
        lhs=billingWidth(p,d.operands[0]);lz=zeroBits(p,d.operands[0]);
        if(d.operands.size()>1){rhs=billingWidth(p,d.operands[1]);rz=zeroBits(p,d.operands[1]);}
        if(d.kind==OperationKind::Ite){lhs=std::max(billingWidth(p,d.operands[1]),billingWidth(p,d.operands[2]));rhs=0;lz=rz=0;for(int k:{1,2})if(d.operands[k].kind!=OperandKind::Literal){lz=std::max(lz,zeroBits(p,d.operands[k]));if(knownOne(p,d.operands[k]))fail("unvalidated known-one Mux data pattern");}auto a=literalKind(d.operands[1]),b=literalKind(d.operands[2]);op="mux";if(a!="dynamic"||b!="dynamic"){auto k=a!="dynamic"?a:b;if(k=="other")fail("uncovered Mux constant pattern");op+="_"+k;}}
        else if(d.kind==OperationKind::AddCarry){op="add";if(!zero(d.operands[2])){
            if(d.operands[2].kind!=OperandKind::Symbol||zeroBits(p,d.operands[2])||knownOne(p,d.operands[2]))fail("unvalidated constant/known carry input");
            op="addcarry";
        }}
        else if(d.kind==OperationKind::ReduceOr)op="reduce_or";
        else if(d.kind==OperationKind::ReduceAnd)op="reduce_and";
        else if(d.kind==OperationKind::ReduceXor)op="reduce_xor";
        else if(d.kind==OperationKind::Unary){rhs=0;rz=0;op=d.op==OpCode::LogicNot?"eq_zero":d.op==OpCode::BitNot?"not":"neg";}
        else if(d.kind==OperationKind::Binary){switch(d.op){
        case OpCode::Add:op="add";break;case OpCode::Sub:op="sub";break;case OpCode::Mul:op="mul";break;case OpCode::Div:op="div";break;case OpCode::Mod:op="mod";break;
        case OpCode::Eq:case OpCode::Ne:op=(zero(d.operands[0])||zero(d.operands[1]))?"eq_zero":"compare_eq";break;
        case OpCode::Lt:case OpCode::Le:case OpCode::Gt:case OpCode::Ge:op="compare_rel";break;
        case OpCode::BitAnd:case OpCode::LogicAnd:op="and";break;case OpCode::BitOr:case OpCode::LogicOr:op="or";break;case OpCode::BitXor:op="xor";break;
        case OpCode::Shl:op="shl";break;case OpCode::Shr:op="shr";break;default:fail("unsupported physical opcode");}}
        else fail("unsupported physical operation kind");
        if(op=="eq_zero") {const auto& input=d.operands.size()>1&&zero(d.operands[0])?d.operands[1]:d.operands[0];lhs=billingWidth(p,input);rhs=0;lz=zeroBits(p,input);rz=0;}
    }
    c.billing_width=std::max(lhs,rhs);const double abc=number(impl_->bundle(),"mapping_abc_target_ps");
    if((op=="mul"||op=="add"||op=="addcarry")&&lhs!=rhs)fail("unvalidated asymmetric physical operand widths");
    Features f{{"lhs_width",static_cast<double>(lhs)},{"rhs_width",static_cast<double>(rhs)},{"result_width",static_cast<double>(signal.type.width)},{"log_width",std::log2(std::max({lhs,rhs,1}))},{"width_product",static_cast<double>(lhs)*std::max(rhs,1)},{"fanout",static_cast<double>(std::max(1u,impl_->fanout[id]))},{"lhs_zero_bits",static_cast<double>(lz)},{"rhs_zero_bits",static_cast<double>(rz)},{"abc_target_ps",abc},{"sdc_period_ps",impl_->request.target_period_ps}};
    double terms=0;for(int i=0;i<lhs;++i)terms+=std::max(0,std::min(rhs,signal.type.width-i));
    f["width_result_product"]=static_cast<double>(std::max(lhs,rhs))*signal.type.width;f["retained_product_terms"]=terms;
    const auto& operations=object(impl_->stage(),"operations");auto model=operations.getObject(op);if(!model||string(*model,"quality_status")!="passed")fail("uncovered physical operation/quality: "+op);
    const auto& domain=object(*model,"domain");
    if((domain.get("known_zero_policy")&&domain.getString("known_zero_policy").value_or("")!="none")||
       (domain.get("constant_operand_policy")&&domain.getString("constant_operand_policy").value_or("")!="dynamic_only"))fail("unsupported physical operand-domain policy");
    if(lz||rz)fail("unvalidated known-zero operand pattern: "+op);
    if(impl_->fp_owner[id]<0){
        std::vector<Operand> data;
        if(d.kind==OperationKind::Ite){for(int k:{1,2})if(d.operands[k].kind!=OperandKind::Literal)data.push_back(d.operands[k]);}
        else for(std::size_t k=0;k<std::min<std::size_t>(d.kind==OperationKind::AddCarry?3:2,d.operands.size());++k)if(d.operands[k].kind!=OperandKind::Literal)data.push_back(d.operands[k]);
        validateDataSources(p,data);
    }
    if(impl_->fp_owner[id]<0&&d.kind!=OperationKind::Ite)for(std::size_t k=0;k<std::min<std::size_t>(2,d.operands.size());++k)if(knownOne(p,d.operands[k]))fail("unvalidated known-one operand pattern: "+op);
    if(d.kind!=OperationKind::Ite&&op!="eq_zero"&&impl_->fp_owner[id]<0)
        for(std::size_t k=0;k<std::min<std::size_t>(2,d.operands.size());++k)
            if(d.operands[k].kind==OperandKind::Literal)fail("unvalidated literal data operand: "+op);
    bool covered=false;for(const auto& v:array(object(*model,"domain"),"conditions")){auto a=v.getAsObject();if(!a)fail("invalid model support condition");auto shape=string(*a,"result_shape");int result=signal.type.width;bool valid_shape=(shape=="full_mul"&&result==lhs+rhs)||(shape=="low_mul"&&result==std::max(lhs,rhs))||(shape=="same_width"&&result==lhs)||(shape=="boolean"&&result==1)||(shape=="carry"&&result==std::max(lhs,rhs)+1)||(shape=="fp37"&&lhs==32&&rhs==32&&result==37);
        covered|=valid_shape&&lhs>=number(*a,"lhs_min")&&lhs<=number(*a,"lhs_max")&&rhs>=number(*a,"rhs_min")&&rhs<=number(*a,"rhs_max")&&f.at("fanout")<=number(*a,"fanout_max")&&abc==number(*a,"abc_target_ps")&&f.at("sdc_period_ps")>=number(*a,"sdc_min_ps")&&f.at("sdc_period_ps")<=number(*a,"sdc_max_ps");}
    if(!covered)fail("outside_joint_shape_width_target_domain: "+op+" width="+std::to_string(lhs)+" result="+std::to_string(signal.type.width));
    c.physical_operation=op;c.physical_features=f;
    auto margin=number(*model,"delay_margin_ps");if(margin<0)fail("negative physical delay margin");
    c.delay=predict(object(*model,"delay"),f)+margin;c.combinational=predict(object(*model,"area"),f);
    if(!std::isfinite(c.delay)||!std::isfinite(c.combinational))fail("physical cost overflow");impl_->estimates[id]=c;return c;
}
double ModelCostProvider::stateArea(std::uint64_t bits,std::size_t signals,std::size_t maximum_width,std::size_t minimum_variable_bits) const {
    const auto& s=object(impl_->stage(),"state_model");
    const auto& domain=object(s,"domain");
    if(!maximum_width||bits>number(domain,"max_bits")||signals>number(domain,"max_signals")||maximum_width>number(domain,"max_data_word_width")||minimum_variable_bits<number(domain,"min_variable_data_word_bits"))fail("outside validated state/count/word-width domain");
    const double bit=number(s,"bit_area_um2"),clock=number(s,"clock_area_per_bit_um2"),signal=number(s,"signal_area_um2");
    if(bit<0||clock<0||signal<0)fail("negative state/clock area model");
    const double result=bits*(bit+clock)+signals*signal;if(!std::isfinite(result))fail("state area overflow");return result;
}
void ModelCostProvider::validateStateProtocol(bool reset,bool valid,bool compatible_reset) const {
    const auto& s=object(impl_->stage(),"state_model");
    if(string(s,"protocol_id")!="sync_zero_reset_valid_enable_v1"||!reset||!valid||!compatible_reset)fail("unsupported state/reset/enable protocol");
}
void ModelCostProvider::validateInterface(const Program& p,const std::string& valid_output) const {
    std::size_t data_outputs=0;
    for(const auto& port:p.ports)if(port.direction==PortDirection::Output&&port.name!=valid_output){
        if(port.type.isArray())fail("unvalidated array output interface");++data_outputs;
    }
    if(data_outputs!=1)fail("local stage/state model requires one data output plus valid");
}
double ModelCostProvider::dataBudget(bool input) const {
    const auto& b=object(impl_->stage(),"boundary_model");
    const double io_fraction=number(b,"io_delay_fraction"),overhead=number(b,input?"input_overhead_ps":"register_overhead_ps");
    if(io_fraction<0||io_fraction>=1||overhead<0)fail("invalid boundary budget model");
    return impl_->request.target_period_ps*(input?1-io_fraction:1)-overhead;
}
double ModelCostProvider::stageDelay(double base,std::size_t operations,std::uint64_t bits,const std::vector<NodeId>& members) const {
    if(auto value=templateIntervalCost(members,false)){if(*value<0)fail("uncovered native template stage interval");return *value;}
    if(!operations)return base;const auto& s=object(impl_->stage(),"stage_correction");
    if(!stageCovered(operations,bits))fail("outside stage graph model support");
    if(auto schema=s.getString("composition_schema")){
        if(*schema!="cardinality-specific-stage-v1")fail("unknown stage composition model");
        if(operations==1){
            std::optional<CostEstimate> query;for(auto id:members){auto c=node(*impl_->program,id);if(c.delay>0){if(query)fail("single-operation stage membership mismatch");query=c;}}
            if(!query)fail("single-operation stage members missing");
            const auto& model=object(object(impl_->stage(),"single_operation_models"),query->physical_operation.c_str());
            double margin=number(model,"delay_margin_ps"),scale=number(model,"validation_delay_scale");
            if(margin<0||scale<1)fail("invalid single-operation stage margin");
            return (predict(object(model,"delay"),query->physical_features)+margin)*scale;
        }
    }
    double scale=number(s,"delay_base_scale"),intercept=number(s,"intercept_ps"),op=number(s,"per_operation_ps"),bit=number(s,"per_result_bit_ps");
    if(scale<0||intercept<0||op<0||bit<0)fail("non-monotone stage correction");return scale*base+intercept+op*operations+bit*bits;
}
double ModelCostProvider::stageArea(double base,std::size_t operations,std::uint64_t bits,const std::vector<NodeId>& members) const {
    if(auto value=templateIntervalCost(members,true)){if(*value<0)fail("uncovered native template stage interval");return *value;}
    if(!operations)return base;const auto& s=object(impl_->stage(),"stage_correction");
    if(!stageCovered(operations,bits))fail("outside stage graph model support");
    if(auto schema=s.getString("composition_schema")){
        if(*schema!="cardinality-specific-stage-v1")fail("unknown stage composition model");
        if(operations==1){
            std::optional<CostEstimate> query;for(auto id:members){auto c=node(*impl_->program,id);if(c.delay>0){if(query)fail("single-operation stage membership mismatch");query=c;}}
            if(!query)fail("single-operation stage members missing");
            const auto& model=object(object(impl_->stage(),"single_operation_models"),query->physical_operation.c_str());
            return predict(object(model,"area"),query->physical_features);
        }
    }
    const double scale=number(s,"area_base_scale"),a=number(s,"intercept_um2"),b=number(s,"per_operation_um2"),c=number(s,"per_result_bit_um2");
    if(scale<0||a<0||b<0||c<0)fail("negative stage area correction");return scale*base+a+b*operations+c*bits;
}
bool ModelCostProvider::stageCovered(std::size_t operations,std::uint64_t bits) const {
    if(auto model=impl_->stage().getObject("mul_interval_model"))if(operations<=number(*model,"max_operations")&&bits<=number(*model,"max_result_bits"))return true;
    const auto& s=object(impl_->stage(),"stage_correction");return operations<=number(s,"max_operations")&&bits<=number(s,"max_live_result_bits");
}
bool ModelCostProvider::monotoneGroupBounds() const {
    const auto& s=object(impl_->stage(),"stage_correction");
    if(s.get("composition_schema")){
        auto flag=s.getBoolean("monotone_group_lower_bound");if(!flag||*flag)fail("cardinality-specific stage has no standalone-group lower-bound guarantee");return false;
    }
    return true;
}
bool ModelCostProvider::stageShapeCovered(const std::vector<NodeId>& members) const {
    if(auto value=templateIntervalCost(members,false))return *value>=0;
    const auto& s=object(impl_->stage(),"stage_correction");const auto& domain=object(s,"graph_domain");
    if(string(domain,"version")!="independent-single-output-arithmetic-tree-v1")fail("unknown stage graph domain");
    const auto& p=*impl_->program;std::set<NodeId> inside(members.begin(),members.end()),paid;std::size_t muls=0;
    for(auto id:members){auto c=node(p,id);if(c.delay>0){paid.insert(id);muls+=c.physical_operation=="mul";}}
    if(paid.empty())return true;
    std::uint64_t result_bits=0;for(auto id:paid)result_bits+=p.signal(id).type.width;
    if(paid.size()>number(s,"max_operations")||result_bits>number(s,"max_live_result_bits"))return false;
    if(muls>number(domain,"max_multiplications"))return false;
    std::map<NodeId,unsigned> users;
    for(auto id:paid){auto c=node(p,id);if(paid.size()>1&&c.physical_operation!="mul"&&c.physical_operation!="add")return false;
        std::set<NodeId> dependencies,seen;std::function<void(const Operand&)> upstream=[&](const Operand& v){
            if(v.kind!=OperandKind::Symbol||!inside.count(v.node)||!seen.insert(v.node).second)return;
            if(paid.count(v.node)){dependencies.insert(v.node);return;}
            for(const auto& input:p.signal(v.node).driver->operands)upstream(input);
        };for(const auto& input:p.signal(id).driver->operands)upstream(input);
        for(auto dependency:dependencies)if(++users[dependency]>1)return false;
    }
    unsigned roots=0;for(auto id:paid)roots+=users[id]==0;
    return roots<=number(domain,"max_output_roots");
}
std::optional<double> ModelCostProvider::templateIntervalCost(const std::vector<NodeId>& members,bool area) const {
    const auto& p=*impl_->program;std::set<NodeId> inside(members.begin(),members.end());int owner=-1;
    for(auto id:members)if(impl_->interval_owner.at(id)>=0){int next=impl_->interval_owner[id];if(owner>=0&&owner!=next)return -1.;owner=next;}
    if(owner<0)return {};
    // The validated scope is one template interval, with free surrounding
    // aliases. Multiple regions and paid external operations need new samples.
    for(auto id:members)if(impl_->interval_owner[id]<0){auto c=node(p,id);if(c.delay||c.combinational||c.deferred_to_stage)return -1.;}
    const auto& r=p.arithmetic_regions.at(owner);int first=-1,last=-1;
    auto active=[&](NodeId id){const auto& f=p.signal(id).value;return !(f.valid&&f.constant);};
    for(std::size_t g=0;g<r.legal_cut_groups.size();++g){bool any=false,all=true;for(auto id:r.legal_cut_groups[g])if(active(id)){any|=inside.count(id);all&=inside.count(id);}
        if(any){if(!all||(last>=0&&static_cast<int>(g)!=last+1))return -1.;if(first<0)first=g;last=g;}}
    if(first<0)return 0.;
    std::set<NodeId> interval;for(int g=first;g<=last;++g)interval.insert(r.legal_cut_groups[g].begin(),r.legal_cut_groups[g].end());
    std::set<NodeId> inputs,outputs;
    for(auto id:interval)for(const auto& v:p.signal(id).driver->operands)if(v.kind==OperandKind::Symbol&&!interval.count(v.node))inputs.insert(v.node);
    for(const auto& signal:p.signals)if(!interval.count(signal.id))for(const auto& v:signal.driver->operands)if(v.kind==OperandKind::Symbol&&interval.count(v.node))outputs.insert(v.node);
    auto unknown=[&](NodeId id,int bit){const auto& f=p.signal(id).value;auto i=static_cast<std::size_t>(bit/64);auto mask=std::uint64_t(1)<<(bit%64);return !(f.valid&&((i<f.known_zero.size()&&(f.known_zero[i]&mask))||(i<f.known_one.size()&&(f.known_one[i]&mask))));};
    std::map<NodeId,std::set<int>> needed;std::vector<NodeId> pending;unsigned input_bits=0,capture_bits=0;
    for(auto id:outputs){for(int b=0;b<p.signal(id).type.width;++b)if(unknown(id,b)){needed[id].insert(b);++capture_bits;}pending.push_back(id);}
    while(!pending.empty()){
        auto id=pending.back();pending.pop_back();if(!interval.count(id)||needed[id].empty())continue;const auto& d=*p.signal(id).driver;
        for(std::size_t k=0;k<d.operands.size();++k){const auto& v=d.operands[k];if(v.kind!=OperandKind::Symbol)continue;std::set<int> demand;
            if(d.kind==OperationKind::AddCarry||(d.kind==OperationKind::Binary&&d.op==OpCode::Mul)){int last_bit=std::min(v.type.width-1,*needed[id].rbegin());for(int b=0;b<=last_bit;++b)demand.insert(b);}
            else if(d.kind==OperationKind::Slice){for(int b:needed[id])demand.insert(b+d.lo);}
            else if(d.kind==OperationKind::Concat){int offset=0;for(std::size_t j=k+1;j<d.operands.size();++j)offset+=d.operands[j].type.width;for(int b:needed[id])if(b>=offset&&b-offset<v.type.width)demand.insert(b-offset);}
            else if(d.kind==OperationKind::Assign||d.kind==OperationKind::Cast||d.kind==OperationKind::ZExt||d.kind==OperationKind::Trunc){for(int b:needed[id])if(b<v.type.width)demand.insert(b);}
            else fail("unregistered template bit dependency");
            bool changed=false;for(int b:demand)if(unknown(v.node,b))changed|=needed[v.node].insert(b).second;if(changed)pending.push_back(v.node);
        }
    }
    for(auto id:inputs)input_bits+=needed[id].size();
    const auto& t=r.template_parameters;int width=t.at("lhs_width");std::string configuration=t.at("block_width")==width/2?"half":"quarter",shape=t.at("result_width")==2*width?"full":"low";
    const auto& model=object(impl_->stage(),"mul_interval_model");const double abc=number(impl_->bundle(),"mapping_abc_target_ps");
    const bool target_load=string(model,"version")=="arithmetic-mul-group-interval-target-load-v1";
    if((!target_load&&impl_->request.target_period_ps!=number(model,"sdc_period_ps"))||abc!=number(model,"abc_target_ps"))return -1.;
    std::string key=configuration+"/"+shape+"/"+std::to_string(first)+"/"+std::to_string(last);auto entry=object(model,"intervals").getObject(key);if(!entry||string(*entry,"quality_status")!="passed")return -1.;
    Features f{{"width",static_cast<double>(width)},{"width_squared",static_cast<double>(width)*width},{"log_width",std::log2(width)},{"capture_bits",static_cast<double>(capture_bits)},{"input_bits",static_cast<double>(input_bits)},{"block_width",static_cast<double>(t.at("block_width"))},{"first",static_cast<double>(first)},{"last",static_cast<double>(last)}};
    if(target_load){
        const auto& reference=object(object(object(model,"reference"),"intervals"),key.c_str());
        const auto& anchors=array(reference,"delay_anchors");if(anchors.size()!=3)fail("invalid target reference anchors");
        double previous_width=-1,d=-1;
        for(std::size_t i=0;i<anchors.size();++i){
            auto point=anchors[i].getAsArray();if(!point||point->size()!=2)fail("invalid target reference point");
            auto w=(*point)[0].getAsNumber(),delay=(*point)[1].getAsNumber();
            if(!w||!delay||!std::isfinite(*w)||!std::isfinite(*delay)||*w<=previous_width||*delay<=0)fail("invalid target reference value");
            previous_width=*w;
            if(i){auto left=anchors[i-1].getAsArray();double w0=*(*left)[0].getAsNumber(),d0=*(*left)[1].getAsNumber();
                if(d<0&&w0<=width&&width<=*w)d=d0+(*delay-d0)*(width-w0)/(*w-w0);}
        }
        const double h=number(reference,"overhead_ps"),period=impl_->request.target_period_ps;
        if(d<=0||h<0||period<=h)return -1.;
        const double pressure=(period-h)/d,tight=std::max(0.0,1.5-pressure);const auto& domain=object(*entry,"domain");
        if(number(domain,"production_capture_fanout")!=1||pressure<number(domain,"reference_pressure_min")||period>number(domain,"period_max_ps"))return -1.;
        f.insert({{"capture_fanout",1.},{"width_fanout",static_cast<double>(width)},{"log_width_fanout",std::log2(width)},
                  {"sdc_period_ps",period},{"reference_delay_ps",d},{"reference_overhead_ps",h},{"pressure",pressure},
                  {"tightness",tight},{"width_tightness",width*tight},{"load_tightness",tight},{"width_squared_tightness",static_cast<double>(width)*width*tight}});
    }
    double value=predict(object(*entry,area?"area":"delay"),f);
    if(!area){double scale=number(*entry,"validation_delay_scale");if(scale<1)fail("invalid template validation reserve");value*=scale;}
    return value;
}
std::string ModelCostProvider::modelIdentity() const{return string(impl_->bundle(),"id");}
CostEstimate ModelCostProvider::estimateGroup(const Program& p,const std::vector<NodeId>& members) const {
    if(auto delay=templateIntervalCost(members,false)){
        auto area=templateIntervalCost(members,true);if(*delay<0||!area||*area<0)fail("uncovered composite cost group");
        CostEstimate c;c.delay=*delay;c.combinational=*area;c.delay_unit="ps";c.area_unit="um2";c.source=modelIdentity();c.physical_operation="mul-template-interval";return c;
    }
    return CostProvider::estimateGroup(p,members);
}
double CostProvider::dataBudget(bool) const{return std::numeric_limits<double>::infinity();}
std::unique_ptr<CostProvider> makeCostProvider(const Program& p,const rtlzz::ArithmeticRequest& r){
    if(r.cost_mode=="fixed")return std::make_unique<FixedCostProvider>(costContext(r));
    if(r.cost_mode=="backend-model"){
        auto file=llvm::MemoryBuffer::getFile(r.model_bundle);if(!file)fail("model bundle unavailable");
        auto parsed=llvm::json::parse((*file)->getBuffer());
        if(!parsed){llvm::consumeError(parsed.takeError());fail("invalid model JSON");}
        auto bundle=parsed->getAsObject();if(!bundle)fail("model bundle must be an object");
        if(bundle->getString("version")==kPublishedPrimitiveBundle)return std::make_unique<PublishedPrimitiveCostProvider>(p,r);
        if(bundle->getString("version")==kComponentManifest)return std::make_unique<CompositeCostProvider>(p,r);
        return std::make_unique<ModelCostProvider>(p,r);
    }
    fail("unknown cost mode");
}
}
