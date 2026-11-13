#include "backend/arithmetic_wide_function.hpp"
#include "backend/arithmetic_wide_alu.hpp"
#include <llvm/ADT/APInt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;
const O& object(const O& o,const char* key){auto p=o.getObject(key);if(!p)throw std::runtime_error("missing function object");return *p;}
const A& array(const O& o,const char* key){auto p=o.getArray(key);if(!p)throw std::runtime_error("missing function array");return *p;}
std::string string(const O& o,const char* key){auto p=o.getString(key);if(!p)throw std::runtime_error("missing function string");return p->str();}
double number(const O& o,const char* key){auto p=o.getNumber(key);if(!p||!std::isfinite(*p))throw std::runtime_error("missing/nonfinite function number");return *p;}
double value(const llvm::json::Value& v){auto p=v.getAsNumber();if(!p||!std::isfinite(*p))throw std::runtime_error("nonfinite function parameter");return *p;}
double linear(const O& m,const std::map<std::string,double>& f){const auto& names=array(m,"features");const auto& coeff=array(m,"coefficients");if(names.size()!=coeff.size())throw std::runtime_error("function parameter mismatch");std::set<std::string> used;double y=m.getNumber("intercept").value_or(0.);for(std::size_t i=0;i<names.size();++i){auto n=names[i].getAsString();if(!n||!used.insert(n->str()).second||!f.count(n->str()))throw std::runtime_error("unknown/duplicate function feature");y+=f.at(n->str())*value(coeff[i]);}if(!std::isfinite(y))throw std::runtime_error("nonfinite function price");return std::max(0.,y);}
bool alias(const O& p){
 auto op=string(p,"operation"),carry=string(p,"carry");
 auto mask=[&](const char* n){auto raw=string(p,n);llvm::StringRef s(raw);if(s.starts_with("0x")||s.starts_with("0X"))s=s.drop_front(2);return llvm::APInt(8193,s,16);};
 auto az=mask("lhs_zero_hex"),ao=mask("lhs_one_hex"),bz=mask("rhs_zero_hex"),bo=mask("rhs_one_hex");int a=int(number(p,"lhs_width")),b=int(number(p,"rhs_width"));bool ac=(az|ao)==llvm::APInt::getLowBitsSet(8193,a),bc=(bz|bo)==llvm::APInt::getLowBitsSet(8193,b);
 if(op=="mul")return (ac&&(ao.isZero()||ao.isPowerOf2()))||(bc&&(bo.isZero()||bo.isPowerOf2()));
 if((op=="add"||op=="adc")&&carry=="zero")return (ac&&ao.isZero())||(bc&&bo.isZero());
 if((op=="sub"||op=="sbb")&&carry=="zero")return bc&&bo.isZero();
 return op=="mux"&&carry!="dynamic";
}
bool canonicalSwap(const O& p){
 auto op=string(p,"operation");if(op!="add"&&op!="adc")return false;int w=int(number(p,"arithmetic_width"));
 auto masks=[&](const std::string& name){auto raw=string(p,name.c_str());llvm::StringRef x(raw);if(x.starts_with("0x")||x.starts_with("0X"))x=x.drop_front(2);return llvm::APInt(8193,x,16);};
 auto az=masks("lhs_zero_hex"),ao=masks("lhs_one_hex"),bz=masks("rhs_zero_hex"),bo=masks("rhs_one_hex");
 auto key=[&](bool rhs){int width=int(number(p,rhs?"rhs_width":"lhs_width")),count=0,lo=0,hi=0;auto z=rhs?bz:az,o=rhs?bo:ao;for(int i=0;i<std::min(width,w);++i)if(!(z|o)[i]){if(!count)lo=i;++count;hi=i+1;}return std::array<int,4>{count,hi,lo,width};};
 auto a=key(false),b=key(true);if(a!=b)return b>a;if(az!=bz)return bz.ugt(az);return bo.ugt(ao);
}
void semanticScope(const O& q){
 const auto& local=object(q,"local_context");const auto& p=object(local,"producer");auto op=string(p,"operation");int w=int(number(p,"arithmetic_width")),r=int(number(p,"result_width"));
 auto hex=[&](const O& spec,const std::string& key){auto raw=string(spec,key.c_str());llvm::StringRef x(raw);if(x.starts_with("0x")||x.starts_with("0X"))x=x.drop_front(2);return llvm::APInt(8193,x,16);};
 if(number(object(q,"topology"),"same_stage_remaining_depth")!=1)throw std::runtime_error("unqualified remaining-stage context");
 for(const std::string name:{"lhs","rhs"}){auto one=hex(p,name+"_one_hex"),zero=hex(p,name+"_zero_hex");if(!one.isZero())throw std::runtime_error("known-one operands need a qualified successor");int count=0,lo=-1,hi=-1,width=int(number(p,(name+"_width").c_str()));for(int i=0;i<width;++i)if(!zero[i]){++count;if(lo<0)lo=i;hi=i;}if(count&&hi-lo+1!=count)throw std::runtime_error("multiple live operand intervals need qualification");}
 auto az=hex(p,"lhs_zero_hex"),bz=hex(p,"rhs_zero_hex");int a=int(number(p,"lhs_width")),b=int(number(p,"rhs_width"));
 if(op=="mul"&&(w!=a+b||!az.isZero()||!bz.isZero()))throw std::runtime_error("canonical full-product precision and unmasked operands required");
 if((op=="add"||op=="adc"||op=="sub"||op=="sbb"||op=="mux")&&number(p,"result_lo")!=0)throw std::runtime_error("unqualified nonzero arithmetic slice");
 if((op=="sub"||op=="sbb")&&(w!=std::max(a,b)||!az.isZero()||!bz.isZero()))throw std::runtime_error("unqualified subtract padding/known-bit context");
 if(op=="mux"&&(string(p,"carry")!="dynamic"||w!=number(p,"value_width")))throw std::runtime_error("canonical dynamic whole-result Mux required");
 const auto& consumers=array(local,"consumers");const auto& captures=array(local,"captures");
 if(!consumers.empty()){
  if((op!="add"&&op!="adc"&&op!="mul")||!captures.empty()||consumers.size()!=1)throw std::runtime_error("unqualified consumer/capture context");auto c=consumers[0].getAsObject();if(!c)throw std::runtime_error("consumer required");const auto& s=object(*c,"spec");const auto& binds=array(*c,"bindings");int cw=int(number(s,"arithmetic_width"));
  if(binds.size()!=1||string(s,"operation")!="add"||string(s,"carry")!="zero"||string(s,"status")!="none"||number(s,"result_lo")!=0||number(s,"value_width")!=cw||number(s,"lhs_width")!=cw||number(s,"rhs_width")!=cw)throw std::runtime_error("unqualified consumer algorithm/demand");auto bind=binds[0].getAsObject();if(!bind)throw std::runtime_error("binding required");const auto& map=array(*bind,"result_bit_map");int shift=-1;for(std::size_t i=0;i<map.size();++i)if(map[i].getAsInteger()){shift=int(i);break;}
  if(shift<0||shift+r>cw||int(map.size())!=cw)throw std::runtime_error("unqualified result connection");
  for(int i=0;i<cw;++i)if(i>=shift&&i<shift+r){auto b=map[i].getAsInteger();if(!b||*b!=i-shift)throw std::runtime_error("unqualified result connection");}else{auto z=map[i].getAsString();if(!z||*z!="0")throw std::runtime_error("unqualified result connection");}
  std::string bound=number(*bind,"operand_index")==0?"lhs":"rhs",other=bound=="lhs"?"rhs":"lhs";auto outside=llvm::APInt::getLowBitsSet(8193,cw)^llvm::APInt::getLowBitsSet(8193,r).shl(shift);
  if(hex(s,bound+"_zero_hex")!=outside||!hex(s,bound+"_one_hex").isZero()||!hex(s,other+"_one_hex").isZero()||!hex(s,other+"_zero_hex").isZero())throw std::runtime_error("consumer mask/connection mismatch");
 }else{
  if(captures.size()!=1)throw std::runtime_error("unqualified capture bit demand");auto c=captures[0].getAsObject();if(!c||number(*c,"banks")>2||number(*c,"declared_width")!=r)throw std::runtime_error("unqualified capture bit demand");const auto& bits=array(*c,"result_bit_map");if(int(bits.size())!=r)throw std::runtime_error("unqualified capture bit demand");for(int i=0;i<r;++i){auto b=bits[i].getAsInteger();if(!b||*b!=i)throw std::runtime_error("unqualified capture bit demand");}
 }
}
void supported(const O& s,const std::map<std::string,double>& f,double t){
 if(string(s,"version")!="numerical-structural-support-v2-distinct-geometry")throw std::runtime_error("structural support identity");
 const auto& target=array(s,"target_interval_ps");if(target.size()!=2||t<value(target[0])||t>value(target[1]))throw std::runtime_error("outside continuous target interval");
 const auto& names=array(s,"features");const auto& bounds=array(s,"bounds");const auto& scales=array(s,"scales");const auto& anchors=array(s,"anchors");
 if(names.size()!=bounds.size()||names.size()!=scales.size()||anchors.empty())throw std::runtime_error("invalid structural support");
 std::vector<double> vector;
 for(std::size_t i=0;i<names.size();++i){auto n=names[i].getAsString();auto b=bounds[i].getAsArray();if(!n||!f.count(n->str())||!b||b->size()!=2)throw std::runtime_error("structural support feature mismatch");double x=f.at(n->str());if(x<value((*b)[0])-1e-9||x>value((*b)[1])+1e-9)throw std::runtime_error("outside structural range "+n->str());vector.push_back(x);}
 std::vector<double> distances;std::set<std::vector<double>> unique;
 for(const auto& raw:anchors){auto a=raw.getAsArray();if(!a||a->size()!=vector.size())throw std::runtime_error("structural anchor dimensions");std::vector<double> row;double d=0;for(std::size_t i=0;i<vector.size();++i){double v=value((*a)[i]),scale=value(scales[i]);if(scale<=0)throw std::runtime_error("invalid support scale");row.push_back(v);double delta=(vector[i]-v)/scale;d+=delta*delta;}if(!unique.insert(row).second)throw std::runtime_error("duplicate structural anchor");distances.push_back(std::sqrt(d));}
 auto k=s.getInteger("k");if(!k||*k<1||*k>int64_t(distances.size()))throw std::runtime_error("invalid support k");std::sort(distances.begin(),distances.end());if(distances[*k-1]>number(s,"radius")+1e-9)throw std::runtime_error("outside structural neighborhood");
}
}
std::map<std::string,double> wideFunctionQueryFeatures(const O& q){
 auto f=wideOperatorQueryFeatures(q);const auto& p=object(object(q,"local_context"),"producer");if(alias(p))throw std::runtime_error("exact alias lowering required before arithmetic pricing");auto graph=demandedWideAluGeometry(p);for(const auto& [n,v]:graph.features)f[n]=v;
 bool swapped=canonicalSwap(p);const std::vector<int>* bits[]={swapped?&graph.rhs_bits:&graph.lhs_bits,swapped?&graph.lhs_bits:&graph.rhs_bits};for(int i=0;i<2;++i){std::string name=i?"rhs":"lhs";const auto& v=*bits[i];f[name+"_live_bits"]=v.size();f["log2_"+name+"_live_bits"]=std::log2(v.size()+1.);f[name+"_low_position"]=v.empty()?0:v.front();f[name+"_high_position"]=v.empty()?0:v.back()+1;}
 int overlap=0;for(int bit:graph.lhs_bits)if(std::find(graph.rhs_bits.begin(),graph.rhs_bits.end(),bit)!=graph.rhs_bits.end())++overlap;f["active_overlap"]=overlap;
 if(!graph.mul){f["dynamic_result_bits"]=graph.dynamic_outputs.size();f["known_result_bits"]=f["result_width"]-f["dynamic_result_bits"];}
 std::set<int> dynamic(graph.dynamic_outputs.begin(),graph.dynamic_outputs.end());double uses=0,position=0;for(const auto& raw:array(object(q,"local_context"),"consumers")){const auto& c=*raw.getAsObject();double w=number(object(c,"spec"),"arithmetic_width");for(const auto& rawb:array(c,"bindings")){const auto& b=*rawb.getAsObject();const auto& map=array(b,"result_bit_map");for(std::size_t i=0;i<map.size();++i){auto bit=map[i].getAsInteger();if(bit&&dynamic.count(int(*bit))){++uses;position+=i/std::max(1.,w-1.);}}}}
 f["consumer_operand_pin_uses"]=uses;f["consumer_mapping_position_mean"]=position/std::max(1.,uses);double a=f.at("lhs_live_bits"),b=f.at("rhs_live_bits"),w=f.at("arithmetic_width"),pressure=f.at("clock_pressure");
 f["live_input_sum"]=a+b;f["log2_live_input_sum"]=std::log2(a+b+1.);f["operand_live_min"]=std::min(a,b);f["operand_live_max"]=std::max(a,b);f["log2_retained_work"]=std::log2(f.at("retained_product_terms")+1.);f["log2_value_width"]=std::log2(f.at("value_width")+1.);f["result_high_fraction"]=f.at("result_lo")/w;f["pressure_live_sum"]=(a+b)*pressure;f["pressure_consumer_uses"]=f.at("consumer_operand_pin_uses")*pressure;f["compare_eq_work"]=f.at("predicate_eq")*std::max(a,b);f["compare_le_work"]=f.at("predicate_le")*std::max(a,b);
 f["log2_alu_work"]=std::log2(f["alu_work"]+1.);f["pressure_alu_work"]=f["alu_work"]*pressure;f["consumer_conditioning_proxy"]=std::log2(f["consumer_arithmetic_width"]+1.)*(1-f["consumer_mapping_position_mean"]);f["consumer_load_work"]=uses*f["consumer_conditioning_proxy"];f["log2_operand_live_min"]=std::log2(std::min(a,b)+1.);f["log2_demand_hi"]=std::log2(number(p,"result_lo")+number(p,"value_width")+1.);
 for(const auto& [n,v]:f)if(!std::isfinite(v))throw std::runtime_error("nonfinite numerical function feature");return f;
}
O evaluateWideFunctionModel(const O& model,const O& q,bool diagnostic){try{
 if(string(model,"version")!="common-wide-operator-functions-v2"||(string(model,"quality_status")!="passed_independent"&&!diagnostic))throw std::runtime_error("operator functions lack independent qualification");
 const auto& flow=object(q,"flow_context");if(string(flow,"profile_sha256")!=string(model,"profile_sha256")||string(flow,"boundary_policy")!="unsigned-wide-family-hierarchy-v1")throw std::runtime_error("function/measurement policy mismatch");
 const auto& p=object(object(q,"local_context"),"producer");auto f=wideFunctionQueryFeatures(q);
 if(number(object(q,"topology"),"operand_shared_input_roots")!=0)throw std::runtime_error("correlated operator operands require qualified composition");
 if(alias(p))throw std::runtime_error("exact alias lowering required before arithmetic pricing");
 semanticScope(q);
 std::string op=string(p,"operation"),name=(op=="add"||op=="adc")?"add":(op=="sub"||op=="sbb")?"sub":(op=="ult"||op=="ule"||op=="eq")?"compare":op;
 const auto* domain=object(model,"support").getObject(name);const auto* m=object(model,"functions").getObject(name);if(!domain||!m)throw std::runtime_error("missing operator family");
 if((name!="mul"&&f["alu_work"]==0)||f["dynamic_result_bits"]==0)throw std::runtime_error("constant/wire lowering required before arithmetic pricing");
 try{supported(*domain,f,number(flow,"target_period_ps"));}catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}
 O features;for(const auto& [n,v]:f)features[n]=v;
 double d=linear(object(*m,"delay_ps"),f),area=linear(object(*m,"area_um2"),f);if(d<=0||area<=0)throw std::runtime_error("nonpositive arithmetic price");
 return O{{"status",diagnostic?"diagnostic_estimate":"covered"},{"operator_delay_ps",d},{"comb_area_um2",area},
     {"train_LOSO_delay_reserve_ps",number(*m,"delay_reserve_ps")},{"reserve_charged_in_price",false},{"features",std::move(features)},{"qualified_for_public_D",!diagnostic}};
}catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}}
}
