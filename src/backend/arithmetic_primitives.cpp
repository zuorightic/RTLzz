#include "backend/arithmetic_primitives.hpp"
#include "backend/arithmetic_mux_context.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
namespace {
using Object=llvm::json::Object;
using Value=llvm::json::Value;
using Features=std::map<std::string,double>;
[[noreturn]] void fail(const std::string& why,NodeId id=kInvalidNodeId) {
    throw rtlzz::ArithmeticError({"cost","cost_uncovered",why,{},id==kInvalidNodeId?"":std::to_string(id)});
}
const Object& object(const Object& o,const char* key) {auto v=o.getObject(key);if(!v)fail(std::string("missing primitive model object: ")+key);return *v;}
const llvm::json::Array& array(const Object& o,const char* key) {auto v=o.getArray(key);if(!v)fail(std::string("missing primitive model array: ")+key);return *v;}
std::string text(const Object& o,const char* key) {auto v=o.getString(key);if(!v)fail(std::string("missing primitive model string: ")+key);return v->str();}
double number(const Object& o,const char* key) {auto v=o.getNumber(key);if(!v||!std::isfinite(*v))fail(std::string("invalid primitive model number: ")+key);return *v;}
std::string numericKey(double value) {std::ostringstream s;s<<std::setprecision(17)<<value;return s.str();}
double numeric(const Object& p,const Features& f) {
    std::vector<double> x;std::set<std::string> seen;
    for(const auto& v:array(p,"features")) {auto key=v.getAsString();if(!key||!f.count(key->str())||!seen.insert(key->str()).second)fail("unknown/duplicate primitive feature");x.push_back(f.at(key->str()));}
    double result=0;auto kind=text(p,"kind");
    if(kind=="linear") {
        const auto& coefficients=array(p,"coefficients");if(coefficients.size()!=x.size())fail("primitive coefficient shape mismatch");
        result=number(p,"intercept");for(std::size_t i=0;i<x.size();++i) {auto c=coefficients[i].getAsNumber();if(!c||!std::isfinite(*c))fail("invalid primitive coefficient");result+=*c*x[i];}
    } else if(kind=="tree") {
        const auto& nodes=array(p,"nodes");std::int64_t at=0;std::set<std::int64_t> seenNodes;
        for(;;) {
            if(at<0||std::size_t(at)>=nodes.size()||!seenNodes.insert(at).second)fail("invalid/cyclic primitive tree");
            auto node=nodes[at].getAsObject();if(!node)fail("invalid primitive tree node");
            if(node->get("value")) {result=number(*node,"value");break;}
            auto feature=node->getInteger("feature");if(!feature||*feature<0||std::size_t(*feature)>=x.size())fail("invalid primitive tree feature");
            auto child=node->getInteger(x[*feature]<=number(*node,"threshold")?"left":"right");if(!child)fail("invalid primitive tree child");at=*child;
        }
    } else fail("unknown primitive predictor");
    if(!std::isfinite(result))fail("nonfinite primitive prediction");return std::max(0.,result);
}
double quantity(const Object& model,const Features& f,bool delay) {
    const Object* entry=&model;auto kind=model.getString("kind");
    if(kind=="legacy12")return quantity(object(model,"model"),f,delay);
    if(kind=="exact_width_slices"||kind=="exact_width_target_slices") {
        auto key=numericKey(f.at("lhs_width"))+"/"+numericKey(f.at("rhs_width"))+"/"+numericKey(f.at("result_width"));
        if(kind=="exact_width_target_slices")key+="/"+numericKey(f.at("sdc_period_ps"));
        entry=object(model,"slices").getObject(key);if(!entry)fail("missing exact primitive width/target slice");
    } else if(kind&&kind!="pooled")fail("unknown primitive quantity schema");
    auto value=numeric(object(*entry,"predictor"),f);
    if(delay) {
        double scale=1,margin=0;
        if(entry->get("delay_scale"))scale=number(*entry,"delay_scale");
        if(entry->get("delay_margin_ps"))margin=number(*entry,"delay_margin_ps");
        if(scale<1||margin<0)fail("invalid frozen primitive reserve");value=value*scale+margin;
    }
    if(!std::isfinite(value))fail("primitive prediction overflow");return value;
}
std::string literal(const Operand& a) {
    if(a.kind!=OperandKind::Literal)return "dynamic";
    bool z=true,one=true,ones=true;
    for(int i=0;i<a.type.width;++i) {bool bit=std::size_t(i/64)<a.constant.limbs.size()&&((a.constant.limbs[i/64]>>(i%64))&1);z&=!bit;one&=bit==(i==0);ones&=bit;}
    return z?"zero":one?"one":ones?"ones":"other";
}
bool wiring(const Operation& d) {
    switch(d.kind) {
    case OperationKind::Assign:case OperationKind::PortRead:case OperationKind::Cast:
    case OperationKind::ZExt:case OperationKind::Trunc:case OperationKind::Slice:case OperationKind::BitSelect:case OperationKind::Concat:return !d.signed_truncation;
    default:return d.kind==OperationKind::Binary&&(d.op==OpCode::Shl||d.op==OpCode::Shr)&&d.operands.size()==2&&d.operands[1].kind==OperandKind::Literal;
    }
}
struct Description {std::string operation;int lhs=0,rhs=0;std::vector<Operand> data;};
NodeId alias(const Program& p,NodeId id) {
    std::set<NodeId> seen;
    while(seen.insert(id).second) {const auto& s=p.signal(id);const auto& d=*s.driver;
        if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast)||d.operands.size()!=1||d.operands[0].kind!=OperandKind::Symbol||d.operands[0].type.width!=s.type.width)return id;
        id=d.operands[0].node;
    }
    fail("cyclic primitive boundary alias");
}
void validateInputs(const Program& p,const std::vector<Operand>& data,NodeId id) {
    std::set<NodeId> prior;
    for(const auto& a:data) {
        if(a.kind!=OperandKind::Symbol||a.signed_view||a.type.isArray())fail("unqualified literal/signed primitive operand",id);
        const auto& value=p.signal(a.node).value;bool patterned=value.valid&&value.constant;
        if(value.valid)for(int bit=0;bit<billingWidth(p,a);++bit){auto word=std::size_t(bit/64);auto mask=std::uint64_t(1)<<(bit%64);
            patterned|=(word<value.known_zero.size()&&(value.known_zero[word]&mask))||(word<value.known_one.size()&&(value.known_one[word]&mask));}
        if(patterned)fail("unqualified primitive operand known bits",id);
        std::set<NodeId> leaves,walked;
        std::function<void(NodeId)> gather=[&](NodeId n) {if(!walked.insert(n).second)return;const auto& d=*p.signal(n).driver;
            if(d.kind==OperationKind::PortRead){leaves.insert(n);return;}
            for(const auto& input:d.operands)if(input.kind==OperandKind::Symbol)gather(input.node);
        };gather(a.node);
        for(auto n:leaves)if(prior.count(n))fail("unqualified correlated primitive operands",id);
        prior.insert(leaves.begin(),leaves.end());
    }
}
}
struct PublishedPrimitiveCostProvider::Impl {
    Value document{nullptr};const Program* program=nullptr;rtlzz::ArithmeticRequest request;
    std::map<NodeId,const ArithmeticRegion*> fp;
    std::set<NodeId> template_members;
    const Object& bundle() const {return *document.getAsObject();}
    const Object& operations() const {return object(object(object(bundle(),"stages"),"grt"),"operations");}
    Description describe(NodeId id) const {
        const auto& p=*program;const auto& s=p.signal(id);if(!s.driver||s.type.isArray())fail("unqualified primitive node/type",id);
        if(template_members.count(id))fail("missing_qualified_mul_interval_model: template members cannot use ordinary Mul prices",id);
        if(auto it=fp.find(id);it!=fp.end()) {
            if(it->second->outputs.empty()||it->second->outputs.front()!=id)return {};
            validateInputs(p,it->second->inputs,id);
            return {"fp32",32,32,it->second->inputs};
        }
        const auto& d=*s.driver;if(s.value.valid&&s.value.constant)return {};
        if(wiring(d)) {
            if(d.kind==OperationKind::Concat) {unsigned dynamic=0;for(const auto& a:d.operands)dynamic+=a.kind!=OperandKind::Literal;if(dynamic>1)fail("unqualified multiple-source primitive wiring",id);}
            return {};
        }
        Description x;
        if(d.kind==OperationKind::Ite) {
            if(d.operands.size()!=3||d.operands[0].kind!=OperandKind::Symbol||d.operands[0].type.width!=1)fail("unqualified primitive selector",id);
            auto left=literal(d.operands[1]),right=literal(d.operands[2]);
            if(left!="dynamic"||right=="other")fail("unqualified reversed/constant primitive Mux",id);
            x.operation=right=="dynamic"?"mux":"mux_"+right;x.lhs=billingWidth(p,d.operands[1]);x.data={d.operands[1]};
            if(right=="dynamic")x.data.push_back(d.operands[2]);
            x.data.push_back(d.operands[0]);
            if(d.operands[2].type.width!=d.operands[1].type.width)fail("asymmetric primitive Mux",id);
        } else {
            if(d.operands.empty())fail("primitive lacks operands",id);
            x.lhs=billingWidth(p,d.operands[0]);
            if(d.operands.size()>1)x.rhs=billingWidth(p,d.operands[1]);
            if(d.kind==OperationKind::AddCarry) {
                if(d.operands.size()!=3)fail("invalid carry primitive",id);
                x.operation=literal(d.operands[2])=="zero"?"add":"addcarry";
                if(x.operation=="addcarry"&&d.operands[2].kind!=OperandKind::Symbol)fail("unqualified constant carry",id);
            } else if(d.kind==OperationKind::Unary&&d.op==OpCode::LogicNot) {x.operation="eq_zero";x.rhs=0;}
            else if(d.kind==OperationKind::Binary) {
                switch(d.op) {
                case OpCode::Add:x.operation="add";break;case OpCode::Mul:x.operation="mul";break;
                case OpCode::Div:x.operation="div";break;case OpCode::Mod:x.operation="mod";break;
                case OpCode::BitOr:x.operation="or";break;
                case OpCode::Eq:
                    if(literal(d.operands[0])=="zero"||literal(d.operands[1])=="zero") {
                        auto a=literal(d.operands[0])=="zero"?d.operands[1]:d.operands[0];x.operation="eq_zero";x.lhs=billingWidth(p,a);x.rhs=0;x.data={a};
                    }
                    break;
                default:break;
                }
            }
            if(x.operation.empty())fail("unqualified primitive operation",id);
            if(x.data.empty())for(std::size_t k=0;k<d.operands.size();++k) {
                if(d.kind==OperationKind::AddCarry&&k==2&&literal(d.operands[k])=="zero")continue;
                x.data.push_back(d.operands[k]);
            }
        }
        if((x.operation=="mul"||x.operation=="add"||x.operation=="addcarry"||x.operation=="div"||x.operation=="mod")&&x.lhs!=x.rhs)fail("asymmetric primitive operands",id);
        validateInputs(p,x.data,id);
        return x;
    }
};
PublishedPrimitiveCostProvider::PublishedPrimitiveCostProvider(const Program& p,const rtlzz::ArithmeticRequest& r):CostProvider(costContext(r)),impl_(std::make_shared<Impl>()) {
    auto file=llvm::MemoryBuffer::getFile(r.model_bundle);if(!file)fail("published primitive bundle unavailable");
    auto bytes=(*file)->getBuffer().str();if(rtlzz::arithmeticHash(bytes)!=r.model_bundle_sha256)fail("published primitive bundle hash mismatch");
    auto parsed=llvm::json::parse(bytes);if(!parsed){llvm::consumeError(parsed.takeError());fail("invalid primitive model JSON");}
    impl_->document=std::move(*parsed);impl_->program=&p;impl_->request=r;
    if(!impl_->document.getAsObject())fail("primitive bundle must be an object");const auto& b=impl_->bundle();
    if(text(b,"version")!=kPublishedPrimitiveBundle||text(b,"quality_status")!="passed"||text(b,"target_platform")!="asap7"||r.target_platform!="asap7"||
       text(b,"platform_profile_sha256")!=r.platform_profile_sha256||text(b,"mux_feature_schema")!=kMuxStaticFeatureSchema||number(b,"ABC_ps")!=1500||text(b,"IO")!="0.2T")fail("published primitive platform/profile/schema/quality mismatch");
    const std::set<std::string> required={"add","addcarry","div","eq_zero","fp32","mod","mul","mux","mux_one","mux_ones","mux_zero","or"};
    if(impl_->operations().size()!=required.size())fail("incomplete published primitive set");
    for(const auto& name:required)if(text(object(impl_->operations(),name.c_str()),"quality_status")!="passed")fail("unqualified published primitive: "+name);
    for(const auto& region:p.arithmetic_regions) {
        if(region.template_version=="softfloat3d-bitvector-mul-v1")for(auto id:region.members)impl_->fp[id]=&region;
        if(region.template_version=="mul-block-tree-v1")impl_->template_members.insert(region.members.begin(),region.members.end());
    }
    context_.feature_schema=kMuxStaticFeatureSchema;
}
std::string PublishedPrimitiveCostProvider::modelIdentity() const {return "asap7-published-primitives/"+impl_->request.model_bundle_sha256;}
CostEstimate PublishedPrimitiveCostProvider::node(const Program& p,NodeId id) const {
    if(&p!=impl_->program)fail("published primitive queried with another graph",id);
    CostEstimate c;c.delay_unit="ps";c.area_unit="um2";c.state_per_bit=0;c.source=modelIdentity();
    auto d=impl_->describe(id);c.physical_operation=d.operation;c.billing_width=std::max(d.lhs,d.rhs);
    if(!d.operation.empty()) {
        c.deferred_to_stage=true;c.reason="actual_stage_cut_and_logical_capture_required";
        c.physical_features={{"lhs_width",double(d.lhs)},{"rhs_width",double(d.rhs)},
            {"result_width",double(p.signal(id).type.width)},{"abc_target_ps",1500},{"sdc_period_ps",impl_->request.target_period_ps}};
    }
    return c;
}
CostEstimate PublishedPrimitiveCostProvider::nodeInStage(const Program& p,NodeId id,const StageCostQuery& cut) const {
    auto c=node(p,id);if(c.physical_operation.empty())return c;
    auto actual=stageCostQuery(p,cut.placement,cut.index,cut.last,cut.controls);
    if(actual.cut_sha256!=cut.cut_sha256||actual.graph_sha256!=cut.graph_sha256||actual.members!=cut.members||actual.captures.size()!=cut.captures.size())fail("stale/forged primitive stage context",id);
    for(std::size_t i=0;i<cut.captures.size();++i)if(cut.captures[i].value!=actual.captures[i].value||cut.captures[i].banks!=actual.captures[i].banks)fail("changed logical capture cut",id);
    if(std::find(actual.members.begin(),actual.members.end(),id)==actual.members.end())fail("primitive is outside the actual stage",id);
    auto description=impl_->describe(id);unsigned banks=0;
    for(auto capture:actual.captures)if(capture.value==id)banks+=capture.banks;
    if(!banks)fail("primitive lacks an independently qualified logical capture/load boundary",id);
    const auto& s=p.signal(id);double lhs=description.lhs,rhs=description.rhs,result=s.type.width,period=impl_->request.target_period_ps;
    Features f{{"lhs_width",lhs},{"rhs_width",rhs},{"result_width",result},{"log_width",std::log2(std::max({lhs,rhs,1.}))},
        {"width_product",lhs*std::max(rhs,1.)},{"fanout",double(banks)},{"lhs_zero_bits",0},{"rhs_zero_bits",0},
        {"abc_target_ps",1500},{"sdc_period_ps",period},{"width_result_product",std::max(lhs,rhs)*result}};
    double terms=0;for(int bit=0;bit<int(lhs);++bit)terms+=std::max(0,std::min(int(rhs),int(result)-bit));f["retained_product_terms"]=terms;
    const auto& model=object(impl_->operations(),description.operation.c_str());bool covered=false;
    for(const auto& v:array(model,"joint_domain")) {auto domain=v.getAsObject();if(!domain)fail("invalid joint primitive domain");
        covered|=number(*domain,"lhs_width")==lhs&&number(*domain,"rhs_width")==rhs&&number(*domain,"result_width")==result&&number(*domain,"abc_target_ps")==1500&&number(*domain,"sdc_period_ps")==period&&number(*domain,"production_capture_fanout")==banks;
    }
    if(!covered)fail("outside exact published primitive width/result/ABC-SDC/capture domain: "+description.operation,id);
    if(description.operation=="mux") {
        const auto& domain=object(model,"context_domain");bool family=false;
        for(const auto& name:array(domain,"independently_qualified_families"))family|=name.getAsString()=="parallel_dynamic_mux";
        if(!family||banks!=number(domain,"production_capture_banks"))fail("Mux context family/capture is not independently qualified",id);
        auto projection=p;std::set<NodeId> inside(actual.members.begin(),actual.members.end());
        for(auto member:actual.members)for(auto& operand:projection.signal(member).driver->operands)if(operand.kind==OperandKind::Symbol&&!inside.count(operand.node)) {
            operand.node=alias(p,operand.node);const auto& source=p.signal(operand.node);
            Operation boundary;boundary.kind=OperationKind::PortRead;boundary.type=source.type;
            Operand port;port.kind=OperandKind::Port;port.type=source.type;port.text="stage_boundary_"+std::to_string(operand.node);boundary.operands={port};projection.signal(operand.node).driver=boundary;
        }
        std::vector<MuxStaticCapture> captures;for(auto capture:actual.captures)captures.push_back({capture.value,capture.banks});
        Features extra;try {extra=muxStaticContext(projection,actual.members,captures,id);}
        catch(const std::invalid_argument& e) {fail(std::string("unqualified actual Mux stage context: ")+e.what(),id);}
        if(extra.at("context_constant_mux_count")||extra.at("context_boolean_count"))fail("mixed-control new Mux coefficients have no independent qualification",id);
        f.insert(extra.begin(),extra.end());
    }
    if(description.operation=="fp32")for(auto member:impl_->fp.at(id)->members)if(actual.placement.at(member)!=actual.index)fail("actual stage cuts a complete binary32 atom",id);
    c.delay=quantity(object(model,"delay"),f,true);c.combinational=quantity(object(model,"area"),f,false);
    c.physical_features=std::move(f);c.deferred_to_stage=false;c.reason.clear();return c;
}
std::string PublishedPrimitiveCostProvider::componentReport(const Program& p,const std::vector<int>& placement,int last,const std::vector<NodeId>& controls) const {
    llvm::json::Array rows;unsigned covered=0,uncovered=0,deferred=0;
    std::vector<StageCostQuery> cuts;if(!placement.empty())for(int at=0;at<=last;++at)cuts.push_back(stageCostQuery(p,placement,at,last,controls));
    std::set<NodeId> ignored(controls.begin(),controls.end());
    for(const auto& s:p.signals)if(!ignored.count(s.id)) {
        Object row{{"node",std::to_string(s.id)},{"width",s.type.width}};
        try {
            auto c=node(p,s.id);if(c.physical_operation.empty())continue;
            row["operation"]=c.physical_operation;
            if(placement.empty()) {Object fields;for(auto [name,value]:c.physical_features)fields[name]=value;
                row["status"]="requires_actual_stage_cut";row["known_request_features"]=std::move(fields);++deferred;}
            else {
                const auto& cut=cuts.at(placement.at(s.id));c=nodeInStage(p,s.id,cut);Object fs;for(auto [name,value]:c.physical_features)fs[name]=value;
                row["status"]="component_predicted_in_domain";row["stage"]=cut.index;row["cut_sha256"]=cut.cut_sha256;
                row["features"]=std::move(fs);row["delay_ps"]=c.delay;row["comb_area_um2"]=c.combinational;++covered;
            }
        } catch(const rtlzz::ArithmeticError& e) {row["status"]="cost_uncovered";row["reason"]=e.diagnostic.failure_reason;++uncovered;}
        rows.push_back(std::move(row));
    }
    return llvm::formatv("{0:2}",Value(Object{{"version","published-primitive-coverage-v1"},{"model_bundle_sha256",impl_->request.model_bundle_sha256},
        {"model_id",modelIdentity()},{"platform_profile_sha256",impl_->request.platform_profile_sha256},{"target_period_ps",impl_->request.target_period_ps},
        {"graph_sha256",rtlzz::arithmeticHash(emitText(p))},{"covered_components",covered},{"uncovered_components",uncovered},{"deferred_components",deferred},
        {"pipeline_qualified",false},{"candidate_PPA_calls",0},{"missing_pipeline_components",llvm::json::Array{"state_model","boundary_model","stage_composition_model","mul_interval_model_when_split"}},
        {"nodes",std::move(rows)}})).str();
}
CostEstimate PublishedPrimitiveCostProvider::estimateGroup(const Program&,const std::vector<NodeId>&) const {fail("published primitives do not qualify an entire stage/group; actual cut and composition evidence required");}
double PublishedPrimitiveCostProvider::stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const {fail("published_primitive_model_missing_state_model");}
void PublishedPrimitiveCostProvider::validateStateProtocol(bool,bool,bool) const {fail("published_primitive_model_missing_state_model: reset/valid/state qualification required");}
void PublishedPrimitiveCostProvider::validateInterface(const Program&,const std::string&) const {fail("published_primitive_model_missing_interface_and_state_qualification");}
double PublishedPrimitiveCostProvider::dataBudget(bool) const {fail("published_primitive_model_missing_boundary_model");}
double PublishedPrimitiveCostProvider::stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const {fail("published_primitive_model_missing_stage_composition_model");}
double PublishedPrimitiveCostProvider::stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const {fail("published_primitive_model_missing_stage_composition_model");}
}
