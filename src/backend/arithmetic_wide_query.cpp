#include "backend/arithmetic_wide_query.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;constexpr unsigned B=8193;
const O& obj(const O& o,const char* n){auto p=o.getObject(n);if(!p)throw std::runtime_error("missing wide object");return *p;}
const A& arr(const O& o,const char* n){auto p=o.getArray(n);if(!p)throw std::runtime_error("missing wide array");return *p;}
std::string str(const O& o,const char* n){auto s=o.getString(n);if(!s)throw std::runtime_error("missing wide string");return s->str();}
int num(const O& o,const char* n,int low=1,int high=4096){auto v=o.getInteger(n);if(!v||*v<low||*v>high)throw std::runtime_error("invalid wide integer");return int(*v);}
double number(const O& o,const char* n){auto v=o.getNumber(n);if(!v||!std::isfinite(*v))throw std::runtime_error("invalid wide number");return *v;}
void fields(const O& o,std::initializer_list<const char*> keys){std::set<std::string> names;for(auto k:keys)names.insert(k);if(o.size()!=names.size())throw std::runtime_error("wide schema field count");for(const auto& x:o)if(!names.count(x.first.str()))throw std::runtime_error("unknown wide field");}
llvm::APInt hexval(const std::string& raw){llvm::StringRef s(raw);if(s.starts_with("0x")||s.starts_with("0X"))s=s.drop_front(2);if(s.empty()||s.size()>2048||s.find_first_not_of("0123456789abcdefABCDEF")!=llvm::StringRef::npos)throw std::runtime_error("invalid wide hex");llvm::APInt v(std::max<unsigned>(B,s.size()*4),s,16);if(v.getActiveBits()>B)throw std::runtime_error("wide hex overflow");return v.zextOrTrunc(B);}
std::string hex(const llvm::APInt& v){llvm::SmallString<2100> b;v.toString(b,16,false);std::string s=b.str().str();std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});return s;}
struct Spec {
 std::string op,carry,status;int a,b,w,lo,v,r,banks;llvm::APInt az,ao,bz,bo;
 Spec(const O& p):op(str(p,"operation")),carry(str(p,"carry")),status(str(p,"status")),a(num(p,"lhs_width")),b(num(p,"rhs_width")),w(num(p,"arithmetic_width")),lo(num(p,"result_lo",0)),v(num(p,"value_width")),r(num(p,"result_width")),banks(num(p,"capture_banks",1,4)),az(hexval(str(p,"lhs_zero_hex"))),ao(hexval(str(p,"lhs_one_hex"))),bz(hexval(str(p,"rhs_zero_hex"))),bo(hexval(str(p,"rhs_one_hex"))){
 fields(p,{"operation","lhs_width","rhs_width","arithmetic_width","result_lo","value_width","status","result_width","lhs_zero_hex","lhs_one_hex","rhs_zero_hex","rhs_one_hex","carry","capture_banks"});
 if(!std::set<std::string>{"add","adc","sub","sbb","mul","ult","ule","eq","mux"}.count(op)||!std::set<std::string>{"zero","one","dynamic"}.count(carry)||!std::set<std::string>{"none","carry","borrow"}.count(status)||lo+v>w||r!=v+(status!="none")||!(az&ao).isZero()||!(bz&bo).isZero()||(az|ao).getActiveBits()>unsigned(a)||(bz|bo).getActiveBits()>unsigned(b))throw std::runtime_error("invalid wide semantics");
 if(op!="adc"&&op!="sbb"&&op!="mux"&&carry!="zero")throw std::runtime_error("nonzero wide control requires ADC/SBB/Mux");
 if((status=="carry"&&op!="add"&&op!="adc")||(status=="borrow"&&op!="sub"&&op!="sbb")||(status!="none"&&std::max(a,b)>w))throw std::runtime_error("wide status precision mismatch");
 if((op=="ult"||op=="ule"||op=="eq")&&(v!=1||lo||w<std::max(a,b)))throw std::runtime_error("wide comparison shape");
 }
 int precision()const{return op=="ult"||op=="ule"||op=="eq"?std::max(a,b):w;}
 int possibilities(bool rhs,int bit)const{if(bit<0||bit>=(rhs?b:a)||(rhs?bz:az)[bit])return 1;if((rhs?bo:ao)[bit])return 2;return 3;}
 std::vector<int> live(bool rhs)const{std::vector<int> out;for(int i=0;i<std::min(rhs?b:a,precision());++i)if(possibilities(rhs,i)==3)out.push_back(i);return out;}
 bool swap()const{if(op!="add"&&op!="adc")return false;auto key=[&](bool rhs){auto x=live(rhs);return std::array<int,4>{int(x.size()),x.empty()?0:x.back()+1,x.empty()?0:x.front(),rhs?b:a};};auto x=key(false),y=key(true);if(x!=y)return y>x;if(az!=bz)return bz.ugt(az);return bo.ugt(ao);}
 bool constant(bool rhs)const{return ((rhs?bz:az)|(rhs?bo:ao))==llvm::APInt::getLowBitsSet(B,rhs?b:a);}
};
std::vector<int> aliases(const Spec& s){
 int source=-1,shift=0;
 if(s.op=="mul")for(int i=0;i<2;++i)if(s.constant(i)){auto value=i?s.bo:s.ao;if(value.isZero())return std::vector<int>(s.r,1);if(value.isPowerOf2()){source=1-i;shift=value.countr_zero();break;}}
 if((s.op=="add"||s.op=="adc")&&s.carry=="zero"){if(s.constant(false)&&s.ao.isZero())source=1;else if(s.constant(true)&&s.bo.isZero())source=0;}
 if((s.op=="sub"||s.op=="sbb")&&s.carry=="zero"&&s.constant(true)&&s.bo.isZero())source=0;
 if(s.op=="mux"&&s.carry!="dynamic")source=s.carry=="one"?1:0;
 if(source<0)return {};std::vector<int> out;for(int i=s.lo;i<s.lo+s.v;++i)out.push_back(s.possibilities(source,i-shift));if(s.status!="none")out.push_back(1);return out;
}
llvm::APInt evaluate(const Spec& s,llvm::APInt a,llvm::APInt b,bool input){
 a=(a&llvm::APInt::getLowBitsSet(B,s.a)&~s.az)|s.ao;b=(b&llvm::APInt::getLowBitsSet(B,s.b)&~s.bz)|s.bo;bool c=s.carry=="dynamic"?input:s.carry=="one";auto mask=llvm::APInt::getLowBitsSet(B,s.w);llvm::APInt value(B,0);bool flag=false;
 if(s.op=="add"||s.op=="adc"){value=(a&mask)+(b&mask)+llvm::APInt(B,c);flag=value[s.w];value&=mask;}
 else if(s.op=="sub"||s.op=="sbb"){flag=(a&mask).ult((b&mask)+llvm::APInt(B,c));value=((a&mask)-(b&mask)-llvm::APInt(B,c))&mask;}
 else if(s.op=="mul")value=(a*b)&mask;else if(s.op=="ult")value=llvm::APInt(B,a.ult(b));else if(s.op=="ule")value=llvm::APInt(B,a.ule(b));else if(s.op=="eq")value=llvm::APInt(B,a==b);else value=(c?b:a)&mask;
 value=value.lshr(s.lo)&llvm::APInt::getLowBitsSet(B,s.v);if(s.status!="none"&&flag)value.setBit(s.v);return value;
}
int known(const Spec& s){
 auto alias=aliases(s);if(!alias.empty())return std::count_if(alias.begin(),alias.end(),[](int p){return p!=3;});
 std::vector<int> native;int flag=1;
 if(s.op=="add"||s.op=="adc"||s.op=="sub"||s.op=="sbb"){
  int carry=s.carry=="dynamic"?3:s.carry=="one"?2:1;bool add=s.op=="add"||s.op=="adc";
  for(int bit=0;bit<s.w;++bit){int output=0,next=0;for(int a=0;a<2;++a)if(s.possibilities(false,bit)&(1<<a))for(int b=0;b<2;++b)if(s.possibilities(true,bit)&(1<<b))for(int c=0;c<2;++c)if(carry&(1<<c)){int v=add?a+b+c:a-b-c;output|=1<<(v&1);next|=1<<(add?v>=2:v<0);}native.push_back(output);carry=next;}flag=carry;
 }else if(s.op=="mul"){
  auto am=llvm::APInt::getLowBitsSet(B,s.a)^s.az,bm=llvm::APInt::getLowBitsSet(B,s.b)^s.bz;
  if(s.constant(false)&&s.constant(true)){auto product=s.ao*s.bo;for(int i=0;i<s.w;++i)native.push_back(product[i]?2:1);}
  else{int az=0,bz=0;while(az<s.a&&s.az[az])++az;while(bz<s.b&&s.bz[bz])++bz;int high=(am*bm).getActiveBits();for(int i=0;i<s.w;++i)native.push_back(i<az+bz||i>=high?1:3);}
 }else if(s.op=="mux")for(int i=0;i<s.w;++i)native.push_back(s.carry=="zero"?s.possibilities(false,i):s.carry=="one"?s.possibilities(true,i):s.possibilities(false,i)|s.possibilities(true,i));
 else{auto am=llvm::APInt::getLowBitsSet(B,s.a)^s.az,bm=llvm::APInt::getLowBitsSet(B,s.b)^s.bz;int bit=3;if(s.op=="ult")bit=am.ult(s.bo)?2:s.ao.uge(bm)?1:3;else if(s.op=="ule")bit=am.ule(s.bo)?2:s.ao.ugt(bm)?1:3;else if(!(s.ao&s.bz).isZero()||!(s.bo&s.az).isZero())bit=1;else if(s.constant(false)&&s.constant(true))bit=s.ao==s.bo?2:1;native.assign(s.w,1);native[0]=bit;}
 int count=0;for(int i=s.lo;i<s.lo+s.v;++i)count+=native[i]!=3;if(s.status!="none")count+=flag!=3;return count;
}
}
std::string evaluateWideOperator(const O& spec,const std::string& a,const std::string& b,bool c){return hex(evaluate(Spec(spec),hexval(a),hexval(b),c));}
std::map<std::string,double> wideOperatorQueryFeatures(const O& q){
 fields(q,{"version","semantics","declared_result_width","required_result_bits","local_context","flow_context","topology"});if(str(q,"version")!="arithmetic-wide-query-v1"||str(q,"semantics")!="unsigned-wide-arithmetic-semantics-v1")throw std::runtime_error("unknown wide Query");const auto& local=obj(q,"local_context");fields(local,{"producer","consumers","captures"});const auto& p=obj(local,"producer");Spec s(p);const auto& flow=obj(q,"flow_context");fields(flow,{"target_platform","evaluation_stage","target_period_ps","abc_target_ps","profile_sha256","boundary_policy"});double t=number(flow,"target_period_ps");if(t<=0||number(flow,"abc_target_ps")!=t||str(flow,"boundary_policy")!="unsigned-wide-family-hierarchy-v1"||str(flow,"target_platform")!="asap7"||str(flow,"evaluation_stage")!="grt"||str(flow,"profile_sha256").empty())throw std::runtime_error("wide flow identity");if(num(q,"declared_result_width")<s.r)throw std::runtime_error("wide declared demand");int previous=-1;for(const auto& v:arr(q,"required_result_bits")){auto bit=v.getAsInteger();if(!bit||*bit!=previous+1||*bit>=s.r)throw std::runtime_error("wide result demand");previous=int(*bit);}if(previous!=s.r-1)throw std::runtime_error("incomplete wide result demand");const auto& topology=obj(q,"topology");fields(topology,{"operand_shared_input_roots","same_stage_remaining_depth"});num(topology,"operand_shared_input_roots",0,1000000);num(topology,"same_stage_remaining_depth",0,1000000);
 bool sw=s.swap();auto a=s.live(sw),b=s.live(!sw);int wa=sw?s.b:s.a,wb=sw?s.a:s.b;bool add=s.op=="add"||s.op=="adc",sub=s.op=="sub"||s.op=="sbb",compare=s.op=="ult"||s.op=="ule"||s.op=="eq";int overlap=0,terms=0;for(int i:a){if(std::find(b.begin(),b.end(),i)!=b.end())++overlap;for(int j:b)terms+=i+j<s.lo+s.v;}int known_bits=known(s);
 std::map<std::string,double> f{{"one",1},{"lhs_width",wa},{"rhs_width",wb},{"arithmetic_width",s.w},{"result_lo",s.lo},{"value_width",s.v},{"result_width",s.r},{"target_ps",t},{"log2_target",std::log2(t/500.)},{"clock_pressure",1500./t},{"family_add",double(add)},{"family_sub",double(sub)},{"family_mul",double(s.op=="mul")},{"family_compare",double(compare)},{"family_mux",double(s.op=="mux")},{"predicate_lt",double(s.op=="ult")},{"predicate_le",double(s.op=="ule")},{"predicate_eq",double(s.op=="eq")},{"control_dynamic",double(s.carry=="dynamic")},{"control_one",double(s.carry=="one")},{"status_carry",double(s.status=="carry")},{"status_borrow",double(s.status=="borrow")},{"active_overlap",overlap},{"retained_product_terms",terms},{"known_result_bits",known_bits},{"dynamic_result_bits",s.r-known_bits},{"log2_arithmetic_width",std::log2(s.w+1.)},{"pressure_product_terms",terms*1500./t},{"pressure_width",s.w*1500./t}};
 for(int i=0;i<2;++i){const auto& bits=i?b:a;std::string n=i?"rhs":"lhs";bool original=i?!sw:sw;f[n+"_live_bits"]=bits.size();f["log2_"+n+"_live_bits"]=std::log2(bits.size()+1.);f[n+"_low_position"]=bits.empty()?0:bits.front();f[n+"_high_position"]=bits.empty()?0:bits.back()+1;f[n+"_known_one_bits"]=(original?s.bo:s.ao).popcount();}
 f["capture_banks"]=0;f["capture_bits"]=0;f["consumer_count"]=arr(local,"consumers").size();f["consumer_arithmetic_width"]=0;f["consumer_operand_pin_uses"]=0;f["consumer_mapping_position_mean"]=0;double uses=0,position=0;
 auto map=[&](const A& bits,int expected){if(int(bits.size())!=expected)throw std::runtime_error("wide map length");for(const auto& v:bits)if(auto b=v.getAsInteger()){if(*b<0||*b>=s.r)throw std::runtime_error("wide mapped bit");}else{auto str=v.getAsString();if(!str||(*str!="0"&&*str!="1"&&*str!="external"&&*str!="undemanded"))throw std::runtime_error("wide map token");}};
 for(const auto& raw:arr(local,"captures")){auto c=raw.getAsObject();if(!c)throw std::runtime_error("wide capture");fields(*c,{"banks","declared_width","result_bit_map"});int banks=num(*c,"banks",1,4),w=num(*c,"declared_width");map(arr(*c,"result_bit_map"),w);f["capture_banks"]+=banks;f["capture_bits"]+=banks*w;}
 if(arr(local,"consumers").empty()&&arr(local,"captures").empty())throw std::runtime_error("unobserved wide producer");
 if(arr(local,"consumers").size()>1)throw std::runtime_error("wide consumer bound");
 for(const auto& raw:arr(local,"consumers")){auto c=raw.getAsObject();if(!c)throw std::runtime_error("wide consumer");fields(*c,{"spec","bindings","other_dynamic_inputs"});if(str(*c,"other_dynamic_inputs")!="independent_launch; source correlations require G validation")throw std::runtime_error("wide input correlation");Spec consumer(obj(*c,"spec"));f["consumer_arithmetic_width"]=std::max(f["consumer_arithmetic_width"],double(consumer.w));std::set<int> seen;for(const auto& rawb:arr(*c,"bindings")){auto bind=rawb.getAsObject();if(!bind)throw std::runtime_error("wide binding");fields(*bind,{"operand_index","result_bit_map"});int i=num(*bind,"operand_index",0,1);if(!seen.insert(i).second)throw std::runtime_error("duplicate wide binding");const auto& bits=arr(*bind,"result_bit_map");map(bits,i?consumer.b:consumer.a);for(std::size_t bit=0;bit<bits.size();++bit)if(bits[bit].getAsInteger()){++uses;position+=double(bit)/std::max(1,consumer.w-1);}}if(seen.empty()||uses==0)throw std::runtime_error("unbound wide consumer");}
 f["consumer_operand_pin_uses"]=uses;f["consumer_mapping_position_mean"]=position/std::max(1.,uses);return f;
}
std::string wideOperatorCore(const O& q,const std::string& name){
 wideOperatorQueryFeatures(q);if(name.empty()||!(std::isalpha(static_cast<unsigned char>(name[0]))||name[0]=='_')||std::any_of(name.begin(),name.end(),[](unsigned char c){return !std::isalnum(c)&&c!='_';}))throw std::runtime_error("safe wide module identifier required");Spec s(obj(obj(q,"local_context"),"producer"));std::ostringstream out;
 auto masked=[&](bool rhs){int w=rhs?s.b:s.a;auto z=rhs?s.bz:s.az,o=rhs?s.bo:s.ao;auto live=llvm::APInt::getLowBitsSet(B,w)&~(z|o);return "(("+std::string(rhs?"b":"a")+"&"+std::to_string(w)+"'h"+hex(live)+")|"+std::to_string(w)+"'h"+hex(o)+")";};std::string control=s.carry=="dynamic"?"cin":s.carry=="one"?"1'b1":"1'b0";bool swapped=s.swap();out<<"// unsigned-wide-family-hierarchy-v1\n(* keep_hierarchy=1 *) module "<<name<<"Core(input wire ["<<s.a-1<<":0] a,input wire ["<<s.b-1<<":0] b,input wire cin,output wire ["<<s.r-1<<":0] value);\nwire ["<<s.a-1<<":0] masked_a="<<masked(false)<<";\nwire ["<<s.b-1<<":0] masked_b="<<masked(true)<<";\nwire ["<<s.w-1<<":0] aa="<<s.w<<"'("<<(swapped?"masked_b":"masked_a")<<");\nwire ["<<s.w-1<<":0] bb="<<s.w<<"'("<<(swapped?"masked_a":"masked_b")<<");\n";
 std::string flag="1'b0";
 if(s.op=="add"||s.op=="adc"||s.op=="sub"||s.op=="sbb"){char sign=s.op=="add"||s.op=="adc"?'+':'-';out<<"wire ["<<s.w<<":0] extended={1'b0,aa}"<<sign<<"{1'b0,bb}"<<sign<<s.w+1<<"'("<<control<<");\nwire ["<<s.w-1<<":0] native_value=extended["<<s.w-1<<":0];\n";flag="extended["+std::to_string(s.w)+"]";}
 else if(s.op=="mul")out<<"wire ["<<s.w-1<<":0] native_value=aa*bb;\n";else if(s.op=="mux")out<<"wire ["<<s.w-1<<":0] native_value="<<control<<"?bb:aa;\n";else out<<"wire ["<<s.w-1<<":0] native_value="<<s.w<<"'(aa"<<(s.op=="ult"?"<":s.op=="ule"?"<=":"==")<<"bb);\n";
 out<<"assign value=";if(s.status!="none")out<<"{"<<flag<<",";out<<"native_value["<<s.lo<<"+:"<<s.v<<"]";if(s.status!="none")out<<"}";out<<";\nendmodule\n(* keep_hierarchy=1 *) module "<<name<<"();endmodule\n";return out.str();
}
}
