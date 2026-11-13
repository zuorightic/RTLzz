#include "backend/arithmetic_add_normalization.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;
const O& object(const O& o,const char* k){auto p=o.getObject(k);if(!p)throw std::runtime_error("missing Add normalization object");return *p;}
int width(const O& o,const std::string& k){auto n=o.getInteger(k);if(!n||*n<1||*n>257)throw std::runtime_error("invalid Add normalization width");return int(*n);}
llvm::APInt mask(const O& o,const std::string& k,int w){
    auto raw=o.getString(k);if(!raw)throw std::runtime_error("missing Add mask");auto s=*raw;if(s.starts_with("0x")||s.starts_with("0X"))s=s.drop_front(2);
    if(s.empty()||s.size()>4096||s.find_first_not_of("0123456789abcdefABCDEF")!=llvm::StringRef::npos)throw std::runtime_error("invalid Add mask");
    llvm::APInt v(std::max<unsigned>(w,unsigned(s.size()*4)),s,16);if(v.getActiveBits()>unsigned(w))throw std::runtime_error("Add mask outside width");return v.zextOrTrunc(257);
}
std::string hex(const llvm::APInt& value){llvm::SmallString<80> text;value.toString(text,16,false);std::string out=text.str().str();std::transform(out.begin(),out.end(),out.begin(),[](unsigned char c){return char(std::tolower(c));});return out;}
std::array<int,4> key(const O& p,const std::string& n){int w=width(p,n+"_width"),r=width(p,"result_width"),count=0,low=0,high=0;auto z=mask(p,n+"_zero_hex",w),one=mask(p,n+"_one_hex",w);bool first=true;for(int i=0;i<std::min(w,r);++i)if(!z[i]&&!one[i]){++count;if(first){low=i;first=false;}high=i+1;}return {count,high,low,w};}
}
O normalizeAddQuery(const O& query){
    intervalOperatorFeatures(query);O out=query;auto local=out.getObject("local_context");if(!local)throw std::runtime_error("missing Add local context");auto p=local->getObject("producer");if(!p||p->getString("operation").value_or("")!="add"||p->getString("carry").value_or("")!="zero")throw std::runtime_error("canonical Add scope requires unsigned zero-carry Add");
    for(auto name:{std::string("lhs"),std::string("rhs")}){int w=width(*p,name+"_width");if(!mask(*p,name+"_one_hex",w).isZero())throw std::runtime_error("canonical Add first scope excludes known-one operands");for(auto field:{"zero","one"})(*p)[name+"_"+field+"_hex"]="0x"+hex(mask(*p,name+"_"+field+"_hex",w));}
    auto a=key(*p,"lhs"),b=key(*p,"rhs");bool swap=b>a;
    if(a==b){auto az=mask(*p,"lhs_zero_hex",width(*p,"lhs_width")),bz=mask(*p,"rhs_zero_hex",width(*p,"rhs_width"));swap=bz.ugt(az);}
    if(swap){for(auto suffix:{"width","zero_hex","one_hex"}){std::string a="lhs_"+std::string(suffix),b="rhs_"+std::string(suffix);auto value=(*p)[a];(*p)[a]=(*p)[b];(*p)[b]=std::move(value);}}
    auto flow=out.getObject("flow_context");if(!flow)throw std::runtime_error("missing canonical Add flow");(*flow)["boundary_policy"]="unsigned-add-normalized-operands-v1";
    return O{{"version","canonical-add-query-v1"},{"descriptor",std::move(out)},{"operand_permutation",llvm::json::Array{swap?1:0,swap?0:1}},{"boundary_policy","unsigned-add-normalized-operands-v1"},{"consumer_result_connections_preserved",true},{"physical_mapping_equivalence_qualified",false},{"price_qualification_granted",false}};
}
std::map<std::string,double> normalizedAddFeatures(const O& query){auto c=normalizeAddQuery(query);return intervalOperatorFeatures(*c.getObject("descriptor"));}
std::string normalizedAddCore(const O& query,const std::string& name){
    if(name.empty()||!(std::isalpha(static_cast<unsigned char>(name[0]))||name[0]=='_')||std::any_of(name.begin(),name.end(),[](unsigned char c){return !std::isalnum(c)&&c!='_';}))throw std::runtime_error("safe core identifier required");
    auto contract=normalizeAddQuery(query);const auto& spec=object(object(query,"local_context"),"producer");int wa=width(spec,"lhs_width"),wb=width(spec,"rhs_width"),r=width(spec,"result_width");bool swapped=*(*contract.getArray("operand_permutation"))[0].getAsInteger()==1;std::ostringstream s;
    auto masked=[&](const std::string& n,const char* v,int w){auto z=mask(spec,n+"_zero_hex",w),one=mask(spec,n+"_one_hex",w);llvm::APInt live=llvm::APInt::getLowBitsSet(257,w)&~(z|one);return "(("+std::string(v)+" & "+std::to_string(w)+"'h"+hex(live)+") | "+std::to_string(w)+"'h"+hex(one)+")";};
    s<<"// unsigned-add-normalized-operands-v1\n(* keep_hierarchy=1 *) module "<<name<<"(input wire ["<<wa-1<<":0] a,input wire ["<<wb-1<<":0] b,input wire cin,output wire ["<<r-1<<":0] value);\n";
    s<<"wire ["<<wa-1<<":0] masked_a="<<masked("lhs","a",wa)<<";\nwire ["<<wb-1<<":0] masked_b="<<masked("rhs","b",wb)<<";\n";
    s<<"wire ["<<r-1<<":0] canonical_lhs="<<r<<"'("<<(swapped?"masked_b":"masked_a")<<");\nwire ["<<r-1<<":0] canonical_rhs="<<r<<"'("<<(swapped?"masked_a":"masked_b")<<");\nassign value=canonical_lhs+canonical_rhs;\nendmodule\n";return s.str();
}
}
