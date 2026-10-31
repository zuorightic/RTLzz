#include "arithmetic_report_reader.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <algorithm>
#include <iostream>
using namespace pred::beir;using namespace arithmetic_replay;
static std::string read(const std::string& p){std::ifstream f(p);if(!f)throw std::runtime_error("missing input "+p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv){
 if(argc!=3)return 2;auto parsed=llvm::json::parse(read(argv[1]));if(!parsed)return 2;const auto& manifest=*parsed->getAsArray();llvm::json::Array results,negatives;unsigned reports=0,regions=0,rejected=0,unreconstructable=0;Program example,tail;
 for(const auto& value:manifest){const auto& entry=object(value);auto name=string(entry,"name");llvm::json::Object row{{"name",name}};
  try{auto raw=read(string(entry,"path"));if(rtlzz::arithmeticHash(raw)!=string(entry,"input_sha256"))throw std::runtime_error("replay input identity mismatch");auto source=llvm::json::parse(raw);if(!source)throw std::runtime_error("bad replay JSON");auto p=graph(object(*source));pred::beir::arithmetic::validate(p,"historical_replay");++reports;regions+=p.arithmetic_regions.size();row["status"]="passed";row["regions"]=static_cast<int>(p.arithmetic_regions.size());if(name=="w8_full_1")example=p;if(name=="w13_full_1")tail=p;
  }catch(const rtlzz::ArithmeticError& e){++rejected;row["status"]="rejected";row["reason"]=std::string(e.what());}
   catch(const std::exception& e){++unreconstructable;row["status"]="unreconstructable";row["reason"]=std::string(e.what());}
  results.push_back(std::move(row));
 }
 auto reject=[&](const std::string& name,Program p){llvm::json::Object row{{"name",name}};try{pred::beir::arithmetic::validate(p,"historical_mutation");row["status"]="unexpected_pass";}catch(const rtlzz::ArithmeticError& e){row["status"]="rejected";row["failure_code"]=e.diagnostic.failure_code;row["reason"]=std::string(e.what());}negatives.push_back(std::move(row));};
 if(!example.signals.empty()){
  auto r=example.arithmetic_regions[0];auto high=[&](Program& p){return std::find_if(p.signals.begin(),p.signals.end(),[&](const auto& s){return std::count(r.members.begin(),r.members.end(),s.id)&&s.driver->kind==OperationKind::Slice&&s.driver->lo==4&&s.driver->operands[0].node==r.inputs[0].node;});};
  auto p=example;auto s=high(p);if(s==p.signals.end())return 3;s->driver->lo=0;s->driver->hi=3;reject("duplicate_low_block",p);
  p=example;for(auto id:r.members){auto& d=*p.signal(id).driver;if(d.kind!=OperationKind::AddCarry)continue;
   const auto& left=*p.signal(d.operands[0].node).driver;const auto& right=*p.signal(d.operands[1].node).driver;
   if(p.signal(left.operands[0].node).driver->kind==OperationKind::Concat&&p.signal(right.operands[0].node).driver->kind==OperationKind::Concat){
    // Repeat the wider shifted product: keeps the sum/carry precision valid,
    // but omits the other high-block term. The coverage certificate must fail.
    d.operands[0]=d.operands[1];break;
   }
  }reject("omitted_reduction_high_term",p);
  p=example;s=high(p);s->driver->operands[0]=r.inputs[1];reject("wrong_original_source",p);
  p=example;for(auto id:r.members){auto& d=*p.signal(id).driver;if(d.kind==OperationKind::Concat&&d.operands[1].type.width==8){
   auto product=d.operands[0];Signal ext;ext.id=p.signals.size();ext.name="mutation_extension";ext.type.width=product.type.width+4;Operation op;op.kind=OperationKind::ZExt;op.type=ext.type;op.to_width=ext.type.width;op.operands={product};ext.driver=op;
   Operand ref;ref.node=ext.id;ref.type=ext.type;d.operands[0]=ref;d.operands[1].type.width=4;d.operands[1].constant.width=4;p.signals.push_back(ext);p.arithmetic_regions[0].members.push_back(ext.id);p.arithmetic_regions[0].legal_cut_groups[0].push_back(ext.id);break;
  }}reject("wrong_partial_shift",p);
 }
 if(!tail.signals.empty()){auto p=tail;const auto& r=p.arithmetic_regions[0];for(auto id:r.members){auto& d=*p.signal(id).driver;if(d.kind==OperationKind::Slice&&d.lo==12){d.lo=11;d.hi=11;break;}}reject("nonuniform_tail_coordinate",p);}
 bool good=reports==129&&regions==219&&negatives.size()==5;for(const auto& v:negatives)if(string(object(v),"status")!="rejected")good=false;
 llvm::json::Object summary{{"version","mul-historical-replay-v1"},{"status",good?"passed":"failed"},{"reports",static_cast<int>(reports)},{"regions",static_cast<int>(regions)},{"expected_reports",129},{"expected_regions",219},{"rejected_reports",static_cast<int>(rejected)},{"unreconstructable_reports",static_cast<int>(unreconstructable)},{"reader_scope","actual archived scalar graphs; nodes, slice/shift operands, constants, groups and parameters required; PortRead text from declared ports; split policy derived from registered template v1; facts recomputed"},{"results",std::move(results)},{"mutations",std::move(negatives)}};
 std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(summary))).str();std::cout<<(good?"PASS":"FAIL")<<" historical reports="<<reports<<" regions="<<regions<<" mutations=5\n";return good?0:1;
}
