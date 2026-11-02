#include "backend/arithmetic_core.hpp"
#include "arithmetic_report_reader.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
using namespace pred::beir;
static std::string read(const std::string& path){std::ifstream f(path);if(!f)throw std::runtime_error("missing "+path);return {std::istreambuf_iterator<char>(f),{}};}
static Operand ref(const Program& p,NodeId id){Operand v;v.node=id;v.type=p.signal(id).type;return v;}
static Operand lit(int w,std::uint64_t n){Operand v;v.kind=OperandKind::Literal;v.type.width=w;v.constant.width=w;v.constant.limbs={n};return v;}
static NodeId node(Program& p,int w,OperationKind kind,std::vector<Operand> args,OpCode op=OpCode::None){Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=w;Operation d;d.kind=kind;d.type=s.type;d.op=op;d.operands=std::move(args);s.driver=d;p.signals.push_back(s);return s.id;}
static NodeId input(Program& p,int w,const std::string& name){Operand v;v.kind=OperandKind::Port;v.text=name;v.type.width=w;auto id=node(p,w,OperationKind::PortRead,{v});p.signal(id).port_name=name;Port port;port.name=name;port.direction=PortDirection::Input;port.type.width=w;port.element_nodes={id};p.ports.push_back(port);p.inputs.push_back(name);return id;}
static Program relational(bool ne,int source_width,int result_width,bool extend=false){
 Program p;auto a=input(p,result_width,"a"),d=input(p,source_width,"d");auto value=ref(p,d);
 if(extend)value=ref(p,node(p,16,OperationKind::ZExt,{value}));
 auto z=node(p,1,OperationKind::Binary,{value,lit(value.type.width,0)},ne?OpCode::Ne:OpCode::Eq);
 auto safe=node(p,result_width,OperationKind::Ite,ne?std::vector<Operand>{ref(p,z),value,lit(value.type.width,1)}:std::vector<Operand>{ref(p,z),lit(value.type.width,1),value});
 node(p,result_width,OperationKind::Binary,{ref(p,a),ref(p,safe)},OpCode::Div);return p;
}
static Program branches(std::uint64_t a,std::uint64_t b,int result_width=8,bool nested=false){
 Program p;auto x=input(p,result_width,"a"),c=input(p,1,"c");auto v=ref(p,node(p,nested?16:result_width,OperationKind::Ite,{ref(p,c),lit(16,a),lit(16,b)}));
 if(nested)v=ref(p,node(p,result_width,OperationKind::Ite,{ref(p,c),v,lit(16,1)}));
 node(p,result_width,OperationKind::Binary,{ref(p,x),v},OpCode::Mod);return p;
}
int main(int argc,char** argv){
 if(argc<3)return 2;bool observe=argc>3;unsigned unexpected=0;llvm::json::Array tests,replays;
 auto check=[&](const std::string& name,Program p,bool expected){bool accepted=true;std::string code;
  try{pred::beir::arithmetic::validate(p,"divisor_contract_test");}catch(const rtlzz::ArithmeticError& e){accepted=false;code=e.diagnostic.failure_code;if(code!="unsafe_divisor")throw;}
  if(accepted!=expected)++unexpected;tests.push_back(llvm::json::Object{{"case",name},{"expected_safe",expected},{"accepted",accepted},{"failure_code",code}});
 };
 check("Eq_truncates_d256_to_zero",relational(false,16,8),false);
 check("Ne_truncates_d256_to_zero",relational(true,16,8),false);
 check("two_nonzero_branches_lost_high_bit",branches(256,1),false);
 check("nested_Ite_loses_nonzero_high_bits",branches(256,512,8,true),false);
 check("legal_narrow_Ite_low_bits_retained",branches(257,1),true);
 check("legal_nested_narrow_Ite_low_bits",branches(257,513,8,true),true);
 check("original_Eq_safe_wrapper",relational(false,16,16),true);
 check("original_Ne_safe_wrapper",relational(true,16,16),true);
 check("ZExt_then_narrow_Eq_keeps_original_8bits",relational(false,8,8,true),true);
 check("ZExt_then_narrow_Ne_keeps_original_8bits",relational(true,8,8,true),true);
 // A known-one fact is useful only when its actual bit survives the cast.
 for(bool high:{false,true}){auto p=branches(high?256:257,high?256:257,8,true);auto& s=p.signal(p.signals.size()-3);s.value.valid=true;s.value.known_one={high?256u:1u};check(high?"known_one_outside_retained_bits":"known_one_inside_retained_bits",p,!high);}
 for(bool keep_low:{false,true}){
  auto p=branches(1,1,64);auto& choice=*p.signal(p.signals.size()-2).driver;
  for(auto& value:choice.operands)if(value.kind==OperandKind::Literal){value.type.width=128;value.constant.width=128;value.constant.limbs={keep_low?1u:0u,2u};}
  check(keep_low?"128_to_64_retains_low_nonzero":"128_to_64_discards_only_high_nonzero",p,keep_low);
 }
 auto parsed=llvm::json::parse(read(argv[1]));if(!parsed||!parsed->getAsArray())return 2;
 unsigned reports=0,divmod=0;
 for(const auto& row:*parsed->getAsArray()){
  auto path=row.getAsString();if(!path)return 2;auto report=llvm::json::parse(read(path->str()));if(!report||!report->getAsObject())return 2;
  auto p=arithmetic_replay::graph(*report->getAsObject());pred::beir::arithmetic::validate(p,"historical_divisor_replay");
  unsigned count=0;for(const auto& s:p.signals)if(s.driver&&s.driver->kind==OperationKind::Binary&&(s.driver->op==OpCode::Div||s.driver->op==OpCode::Mod))++count;
  divmod+=count;++reports;replays.push_back(llvm::json::Object{{"path",path->str()},{"status","passed"},{"Div_Mod",count}});
 }
 llvm::json::Object result{{"tests",std::move(tests)},{"observed_wrong_acceptances",unexpected},{"historical_reports",reports},{"historical_Div_Mod",divmod},{"historical_replays",std::move(replays)},{"observe_old_production",observe}};
 std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result))).str();std::cout<<"divisor contracts wrong="<<unexpected<<" historical_reports="<<reports<<" Div_Mod="<<divmod<<"\n";
 return !observe&&unexpected?1:0;
}
