#include "backend/arithmetic_operator_query.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <tuple>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
[[noreturn]] void fail(const std::string& why){throw rtlzz::ArithmeticError({"operator_query","cost_uncovered",why,{},{}});}
std::string text(const O& o,const char* key){auto v=o.getString(key);if(!v)fail(std::string("missing string ")+key);return v->str();}
int integer(const O& o,const char* key){auto n=o.getInteger(key);if(!n||*n<0||*n>1000000)fail(std::string("bounded integer required: ")+key);return int(*n);}
double numeric(const O& o,const char* key){auto n=o.getNumber(key);if(!n||!std::isfinite(*n))fail(std::string("finite number required: ")+key);return *n;}
const O& object(const O& o,const char* key){auto x=o.getObject(key);if(!x)fail(std::string("missing object ")+key);return *x;}
const A& array(const O& o,const char* key){auto x=o.getArray(key);if(!x)fail(std::string("missing array ")+key);return *x;}
void fields(const O& o,std::initializer_list<const char*> allowed){std::set<std::string> keys;for(auto key:allowed)keys.insert(key);if(o.size()!=keys.size())fail("missing or unexpected query field");for(const auto& entry:o)if(!keys.count(entry.first.str()))fail("unexpected query field");}
int highest(const std::vector<bool>& mask){for(int i=int(mask.size())-1;i>=0;--i)if(mask[i])return i+1;return 0;}
A positions(const std::vector<bool>& mask){A out;for(int i=0;i<int(mask.size());++i)if(mask[i])out.push_back(i);return out;}
bool wordBit(const std::vector<std::uint64_t>& words,int bit){return std::size_t(bit/64)<words.size()&&((words[bit/64]>>(bit%64))&1);}
std::string maskHex(const std::vector<bool>& bits){std::string result;for(int i=(int(bits.size())+3)/4-1;i>=0;--i){int digit=0;for(int b=0;b<4;++b)if(i*4+b<int(bits.size())&&bits[i*4+b])digit|=1<<b;result+="0123456789abcdef"[digit];}auto at=result.find_first_not_of('0');return "0x"+(at==std::string::npos?std::string("0"):result.substr(at));}
std::vector<bool> hexMask(const O& o,const std::string& key,int width){
    auto raw=text(o,key.c_str());if(raw.rfind("0x",0)==0)raw=raw.substr(2);
    if(raw.empty())fail("empty known mask");std::vector<bool> bits(width);
    for(int i=0;i<int(raw.size());++i){char c=raw[raw.size()-i-1];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(d<0)fail("invalid known mask");
        for(int b=0;b<4;++b)if((d>>b)&1){if(i*4+b>=width)fail("known mask out of range");bits[i*4+b]=true;}}
    return bits;
}
std::string operation(const Signal& s){
    if(!s.driver)fail("undriven node");const auto& d=*s.driver;
    if(d.kind==OperationKind::PortRead)return "input";
    if(d.kind==OperationKind::AddCarry)return "addcarry";
    if(d.kind==OperationKind::Binary){if(d.op==OpCode::Mul)return "mul";if(d.op==OpCode::Add)return "add";}
    switch(d.kind){case OperationKind::Assign:case OperationKind::Cast:case OperationKind::ZExt:case OperationKind::Trunc:case OperationKind::Slice:case OperationKind::Concat:case OperationKind::BitSelect:return "wire";default:fail("operation not in unsigned query subset");}
}
struct Bit {NodeId node=kInvalidNodeId;int bit=0;int constant=-1;};
struct Extraction {
    const Program& p;
    std::vector<std::vector<bool>> demand;
    std::vector<std::vector<Bit>> maps;
    std::vector<int> mapping;
    Extraction(const Program& program):p(program),demand(p.signals.size()),maps(p.signals.size()),mapping(p.signals.size()){
        for(const auto& s:p.signals){if(s.type.isArray()||s.type.width<=0||s.type.width>257)fail("query requires scalar width 1..257");demand[s.id].resize(s.type.width);}
        std::deque<NodeId> pending;
        auto feed=[&](NodeId id,int bit){if(id>=demand.size())fail("invalid query reference");if(bit<0||bit>=int(demand[id].size()))return;if(!demand[id][bit]){demand[id][bit]=true;pending.push_back(id);}};
        for(const auto& port:p.ports)if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)for(int b=0;b<p.signal(id).type.width;++b)feed(id,b);
        while(!pending.empty()){
            auto id=pending.front();pending.pop_front();const auto& s=p.signal(id);if(!s.driver)fail("undriven demanded signal");if(s.value.valid&&s.value.constant)continue;
            auto op=operation(s);if(op=="input")continue;const auto& d=*s.driver;
            auto arg=[&](std::size_t i,int b){if(i>=d.operands.size())fail("missing query operand");const auto& a=d.operands[i];if(a.signed_view)fail("signed query operand");if(a.kind==OperandKind::Symbol)feed(a.node,b);else if(a.kind!=OperandKind::Literal)fail("unknown operand kind");};
            if(op!="wire"){for(std::size_t k=0;k<d.operands.size();++k)for(int b=0;b<std::min(highest(demand[id]),d.operands[k].type.width);++b)arg(k,b);continue;}
            for(int b=0;b<s.type.width;++b)if(demand[id][b])switch(d.kind){
            case OperationKind::Concat:{int offset=0;for(std::size_t k=d.operands.size();k-->0;){if(b>=offset&&b<offset+d.operands[k].type.width)arg(k,b-offset);offset+=d.operands[k].type.width;}if(offset!=s.type.width)fail("concat width mismatch");break;}
            case OperationKind::Slice:if(d.lo<0||d.hi-d.lo+1!=s.type.width)fail("slice width mismatch");arg(0,b+d.lo);break;
            case OperationKind::BitSelect:if(d.bit<0)fail("dynamic selection not covered");arg(0,d.bit);break;
            default:arg(0,b);break;
            }
        }
    }
    std::vector<Bit> operand(const Operand& a){
        if(a.signed_view)fail("signed origin map not covered");
        if(a.kind==OperandKind::Symbol){if(a.type.width!=p.signal(a.node).type.width)fail("reference type width differs");return origins(a.node);}
        if(a.kind!=OperandKind::Literal)fail("unknown operand origin");std::vector<Bit> out;for(int i=0;i<a.type.width;++i)out.push_back({kInvalidNodeId,0,wordBit(a.constant.limbs,i)?1:0});return out;
    }
    const std::vector<Bit>& origins(NodeId id){
        if(mapping[id]==2)return maps[id];if(mapping[id]==1)fail("cyclic origin mapping");mapping[id]=1;
        const auto& s=p.signal(id);const auto& d=*s.driver;auto& out=maps[id];
        if(s.value.valid&&s.value.constant){for(int b=0;b<s.type.width;++b)out.push_back({kInvalidNodeId,0,wordBit(s.value.known_one,b)?1:0});mapping[id]=2;return out;}
        auto op=operation(s);
        if(op!="wire"){for(int b=0;b<s.type.width;++b)out.push_back({id,b,-1});}
        else if(d.kind==OperationKind::Concat){for(std::size_t k=d.operands.size();k-->0;){auto v=operand(d.operands[k]);out.insert(out.end(),v.begin(),v.end());}}
        else {auto a=operand(d.operands.at(0));if(d.kind==OperationKind::Slice||d.kind==OperationKind::BitSelect){int low=d.kind==OperationKind::Slice?d.lo:d.bit;if(low<0||low+s.type.width>int(a.size()))fail("slice origin out of range");out.assign(a.begin()+low,a.begin()+low+s.type.width);}
            else {out=a;out.resize(s.type.width,{kInvalidNodeId,0,0});}}
        if(out.size()!=std::size_t(s.type.width))fail("origin width differs");
        for(int b=0;b<s.type.width;++b){bool z=s.value.valid&&wordBit(s.value.known_zero,b),o=s.value.valid&&wordBit(s.value.known_one,b);if(z&&o)fail("conflicting known bits");if(z||o)out[b]={kInvalidNodeId,0,o?1:0};}
        mapping[id]=2;return out;
    }
    O spec(NodeId id){
        const auto& s=p.signal(id);const auto& d=*s.driver;auto op=operation(s);if(op!="mul"&&op!="add"&&op!="addcarry")fail("not an arithmetic query");
        if(d.operands.size()!=(op=="addcarry"?3u:2u))fail("wrong arithmetic arity");
        O result{{"operation",op},{"result_width",highest(demand[id])},{"carry","zero"},{"capture_banks",1}};
        for(int k=0;k<2;++k){const auto& a=d.operands[k];if(a.kind!=OperandKind::Symbol||a.signed_view)fail("query currently requires unsigned dynamic data inputs");int w=billingWidth(p,a);if(w<=0||w>a.type.width)fail("invalid operand width provenance");
            auto m=operand(a);std::vector<bool> zero(w),one(w);for(int b=0;b<w;++b){zero[b]=m[b].constant==0;one[b]=m[b].constant==1;}
            std::string prefix=k?"rhs":"lhs";result[prefix+"_width"]=w;result[prefix+"_zero_hex"]=maskHex(zero);result[prefix+"_one_hex"]=maskHex(one);
        }
        if(op=="addcarry"){
            auto carry=operand(d.operands[2]);if(carry.size()!=1)fail("non-bit carry");
            auto mode=carry[0].constant==0?"zero":carry[0].constant==1?"one":"dynamic";result["carry"]=mode;
            if(carry[0].constant==0)result["operation"]="add";
        }
        return result;
    }
    A project(const std::vector<Bit>& map,NodeId producer,int result_width,int operand_width,bool input_operand){
        A out;for(int i=0;i<operand_width;++i){if(i>=int(map.size()))fail("short bit map");const auto& b=map[i];
            if((input_operand&&i>=result_width)||(!input_operand&&!demand[producer][std::min(b.bit,int(demand[producer].size())-1)]&&b.node==producer))out.push_back("undemanded");
            else if(b.constant>=0)out.push_back(b.constant?"1":"0");else if(b.node==producer){if(b.bit>=highest(demand[producer]))fail("consumer asks trimmed producer bit");out.push_back(b.bit);}else out.push_back("external");}
        return out;
    }
};
}

std::map<std::string,double> operatorQueryFeatures(const O& descriptor){
    fields(descriptor,{"version","semantics","declared_result_width","required_result_bits","local_context","flow_context","topology"});
    if(text(descriptor,"version")!=kOperatorQueryVersion||text(descriptor,"semantics")!="unsigned-modulo-v1")fail("unknown operator query schema/semantics");
    const auto& ctx=object(descriptor,"flow_context");double t=numeric(ctx,"target_period_ps");
    fields(ctx,{"target_platform","evaluation_stage","target_period_ps","abc_target_ps","profile_sha256","boundary_policy"});
    if(t<=0||numeric(ctx,"abc_target_ps")!=t||text(ctx,"target_platform")!="asap7"||text(ctx,"evaluation_stage")!="grt"||text(ctx,"profile_sha256").empty()||text(ctx,"boundary_policy").empty())fail("incompatible query flow context");
    const auto& local=object(descriptor,"local_context");const auto& p=object(local,"producer");auto op=text(p,"operation");if(op!="mul"&&op!="add"&&op!="addcarry")fail("unsupported query operation");
    fields(local,{"producer","consumers","captures"});
    fields(p,{"operation","lhs_width","rhs_width","result_width","lhs_zero_hex","lhs_one_hex","rhs_zero_hex","rhs_one_hex","carry","capture_banks"});
    std::map<std::string,double> f{{"lhs_width",integer(p,"lhs_width")},{"rhs_width",integer(p,"rhs_width")},{"result_width",integer(p,"result_width")},
        {"declared_result_width",integer(descriptor,"declared_result_width")},{"target_period_ps",t},{"abc_target_ps",t},{"log2_target_ps",std::log2(t)}};
    for(auto name:{"mul","add","addcarry"})f[std::string("operation_")+name]=op==name;
    auto carry=text(p,"carry");if(carry!="zero"&&carry!="one"&&carry!="dynamic")fail("invalid carry mode");f["carry_dynamic"]=carry=="dynamic";f["carry_one"]=carry=="one";
    int r=int(f["result_width"]),wa=int(f["lhs_width"]),wb=int(f["rhs_width"]);if(wa<1||wb<1||r<1||std::max({wa,wb,r,int(f["declared_result_width"])})>257||r>f["declared_result_width"])fail("invalid query width");
    int previous_required=-1;for(const auto& v:array(descriptor,"required_result_bits")){auto bit=v.getAsInteger();if(!bit||*bit<=previous_required||*bit>=r)fail("invalid required result bits");previous_required=int(*bit);}if(previous_required!=r-1)fail("incomplete result demand");
    f["full_product"]=op=="mul"&&r==wa+wb;f["low_result"]=r<(op=="mul"?wa+wb:std::max(wa,wb)+1);f["required_result_bits"]=array(descriptor,"required_result_bits").size();
    f["width_product"]=wa*wb;
    const auto& topology=object(descriptor,"topology");fields(topology,{"operand_shared_input_roots","same_stage_remaining_depth"});f["operand_shared_input_roots"]=integer(topology,"operand_shared_input_roots");f["same_stage_remaining_depth"]=integer(topology,"same_stage_remaining_depth");
    for(auto prefix:{std::string("lhs"),std::string("rhs")}){
        int w=integer(p,(prefix+"_width").c_str());auto zeros=hexMask(p,prefix+"_zero_hex",w),ones=hexMask(p,prefix+"_one_hex",w);
        for(int i=0;i<w;++i)if(zeros[i]&&ones[i])fail("conflicting operand masks");
        for(auto which:{0,1}){const auto& bits=which?ones:zeros;std::string base=prefix+(which?"_one":"_zero");int count=0,runs=0,longest=0,current=0,lead=0,trail=0;bool in=false;for(int i=0;i<w;++i){if(bits[i]){++count;++current;if(!in)++runs;longest=std::max(longest,current);}else current=0;in=bits[i];}while(trail<w&&bits[trail])++trail;while(lead<w&&bits[w-lead-1])++lead;
            f[base+"_count"]=count;f[base+"_runs"]=runs;f[base+"_longest_run"]=longest;f[base+"_lsb_run"]=trail;f[base+"_msb_run"]=lead;
            for(int bin=0;bin<4;++bin){int n=0;for(int i=0;i<w;++i)if(std::min(3,4*i/w)==bin&&bits[i])++n;f[base+"_bin"+std::to_string(bin)]=n;}}
    }
    for(const char* key:{"consumer_count","consumer_mul","consumer_add","consumer_addcarry","consumer_lhs_max","consumer_rhs_max","consumer_result_max","operand0_bindings","operand1_bindings","capture_banks","capture_declared_bits","mapped_dynamic_bits","mapped_unique_bits","mapped_repeated_bits","mapped_zero_bits","mapped_one_bits","mapped_external_bits","mapped_undemanded_bits","mapped_origin_span","mapping_runs","mapping_shift_min","mapping_shift_max"})f[key]=0;
    std::set<int> used;bool any_shift=false;
    auto mapping=[&](const A& map){int previous=-2,runs=0;for(int i=0;i<int(map.size());++i){if(auto v=map[i].getAsInteger()){if(*v<0||*v>=r)fail("mapped producer bit out of range");++f["mapped_dynamic_bits"];used.insert(int(*v));if(*v!=previous+1)++runs;previous=int(*v);int shift=i-int(*v);if(!any_shift){f["mapping_shift_min"]=f["mapping_shift_max"]=shift;any_shift=true;}else{f["mapping_shift_min"]=std::min(f["mapping_shift_min"],double(shift));f["mapping_shift_max"]=std::max(f["mapping_shift_max"],double(shift));}}
            else {auto token=map[i].getAsString();if(!token)fail("invalid bit-map token");std::string key=*token=="0"?"mapped_zero_bits":*token=="1"?"mapped_one_bits":*token=="external"?"mapped_external_bits":*token=="undemanded"?"mapped_undemanded_bits":"";if(key.empty())fail("unknown bit-map token");++f[key];previous=-2;}}
        f["mapping_runs"]+=runs;
    };
    for(const auto& v:array(local,"consumers")){auto c=v.getAsObject();if(!c)fail("invalid consumer");const auto& spec=object(*c,"spec");auto name=text(spec,"operation");if(name!="mul"&&name!="add"&&name!="addcarry")fail("unknown consumer operation");++f["consumer_count"];++f["consumer_"+name];for(auto k:{"lhs","rhs","result"}){auto key=std::string("consumer_")+k+"_max";f[key]=std::max(f[key],double(integer(spec,(std::string(k)+"_width").c_str())));}
        std::set<int> bound;for(const auto& b:array(*c,"bindings")){auto bind=b.getAsObject();if(!bind)fail("invalid consumer binding");fields(*bind,{"operand_index","result_bit_map"});auto index=integer(*bind,"operand_index");if(index>1||!bound.insert(index).second)fail("invalid or duplicate consumer operand");++f["operand"+std::to_string(index)+"_bindings"];const auto& bits=array(*bind,"result_bit_map");if(bits.size()!=std::size_t(integer(spec,index?"rhs_width":"lhs_width")))fail("consumer map width differs");mapping(bits);}}
    for(const auto& v:array(local,"captures")){auto c=v.getAsObject();if(!c)fail("invalid capture");auto banks=integer(*c,"banks"),width=integer(*c,"declared_width");if(banks<1||banks>4||array(*c,"result_bit_map").size()!=std::size_t(width))fail("invalid capture map");f["capture_banks"]+=banks;f["capture_declared_bits"]+=banks*width;for(int i=0;i<banks;++i)mapping(array(*c,"result_bit_map"));}
    f["mapped_unique_bits"]=used.size();f["mapped_repeated_bits"]=f["mapped_dynamic_bits"]-used.size();f["mapped_origin_span"]=used.empty()?0:*used.rbegin()-*used.begin()+1;
    for(const auto& entry:f)if(!std::isfinite(entry.second))fail("nonfinite query feature");return f;
}

std::vector<OperatorQuery> operatorQueries(const Program& original,const std::vector<int>& placement,int last,double target,
    const std::string& profile,const std::string& boundary,const std::vector<NodeId>& controls){
    if(placement.size()!=original.signals.size()||last<0)fail("invalid placement shape");
    beir::MutableProgram facts(original);facts.ensureValueFacts();auto p=facts.finish();Extraction e(p);std::vector<StageCostQuery> cuts;
    for(int at=0;at<=last;++at)cuts.push_back(stageCostQuery(p,placement,at,last,controls));
    std::map<NodeId,O> specs;for(const auto& s:p.signals)if(highest(e.demand[s.id])&&!(s.value.valid&&s.value.constant)){auto op=operation(s);if(op=="mul"||op=="add"||op=="addcarry")specs.emplace(s.id,e.spec(s.id));}
    std::vector<OperatorQuery> result;
    for(const auto& cut:cuts){std::set<NodeId> members(cut.members.begin(),cut.members.end());
        std::map<NodeId,std::set<NodeId>> children,leaves;
        std::function<const std::set<NodeId>&(NodeId)> roots=[&](NodeId id)->const std::set<NodeId>&{if(leaves.count(id))return leaves.at(id);std::set<NodeId> r;const auto& s=p.signal(id);if(!(s.value.valid&&s.value.constant)){if(operation(s)=="input")r.insert(id);else for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol){const auto& sub=roots(a.node);r.insert(sub.begin(),sub.end());}}return leaves.emplace(id,std::move(r)).first->second;};
        for(const auto& child:specs)if(members.count(child.first))for(const auto& a:p.signal(child.first).driver->operands)if(a.kind==OperandKind::Symbol){for(const auto& b:e.operand(a))if(b.constant<0&&specs.count(b.node)&&members.count(b.node)&&b.node!=child.first)children[b.node].insert(child.first);}
        std::map<NodeId,int> depths;std::function<int(NodeId)> depth=[&](NodeId id){if(depths.count(id))return depths.at(id);int n=1;for(auto child:children[id])n=std::max(n,1+depth(child));return depths[id]=n;};
        for(const auto& entry:specs){auto id=entry.first;if(!members.count(id))continue;A consumers,captures;
            for(const auto& child:specs){if(child.first==id||!members.count(child.first))continue;const auto& d=*p.signal(child.first).driver;A bindings;
                for(int k=0;k<2;++k){auto raw=e.operand(d.operands[k]);int w=integer(child.second,k?"rhs_width":"lhs_width"),r=integer(child.second,"result_width");auto map=e.project(raw,id,r,w,true);bool uses=false;for(const auto& b:map)uses|=bool(b.getAsInteger());if(uses)bindings.push_back(O{{"operand_index",k},{"result_bit_map",std::move(map)}});}
                if(!bindings.empty())consumers.push_back(O{{"spec",O(child.second)}, {"bindings",std::move(bindings)}, {"other_dynamic_inputs","independent_launch; source correlations require G validation"}});
            }
            for(const auto& capture:cut.captures){auto raw=e.origins(capture.value);A map;bool uses=false;for(int b=0;b<int(raw.size());++b){const auto& origin=raw[b];if(!e.demand[capture.value][b])map.push_back("undemanded");else if(origin.constant>=0)map.push_back(origin.constant?"1":"0");else if(origin.node==id){uses=true;map.push_back(origin.bit);}else map.push_back("external");}
                if(uses)captures.push_back(O{{"banks",capture.banks},{"declared_width",p.signal(capture.value).type.width},{"result_bit_map",std::move(map)}});
            }
            const auto& operands=p.signal(id).driver->operands;std::set<NodeId> overlap;const auto& left=roots(operands[0].node);const auto& right=roots(operands[1].node);std::set_intersection(left.begin(),left.end(),right.begin(),right.end(),std::inserter(overlap,overlap.end()));
            O desc{{"version",kOperatorQueryVersion},{"semantics","unsigned-modulo-v1"},{"declared_result_width",p.signal(id).type.width},
                {"topology",O{{"operand_shared_input_roots",int(overlap.size())},{"same_stage_remaining_depth",depth(id)}}},
                {"required_result_bits",positions(e.demand[id])},{"local_context",O{{"producer",O(entry.second)},{"consumers",std::move(consumers)},{"captures",std::move(captures)}}},
                {"flow_context",O{{"target_platform","asap7"},{"evaluation_stage","grt"},{"target_period_ps",target},{"abc_target_ps",target},{"profile_sha256",profile},{"boundary_policy",boundary}}}};
            auto features=operatorQueryFeatures(desc);result.push_back({id,cut.index,std::move(desc),std::move(features)});
        }
    }
    return result;
}

std::string operatorQueryJson(const OperatorQuery& query){O values;for(const auto& entry:query.features)values[entry.first]=entry.second;return llvm::formatv("{0:2}",V(O{{"trace",O{{"node",query.origin},{"stage",query.stage}}},{"descriptor",O(query.descriptor)},{"features",std::move(values)},{"qualified_price",false}})).str();}
}
