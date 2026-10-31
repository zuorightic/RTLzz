#include "backend/mul_contract.hpp"
#include "backend/arithmetic_core.hpp"
#include "arithmetic_request.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
namespace pred::beir::arithmetic {
void validateBlockTree(const Program& p,const ArithmeticRegion& r,const std::string& phase){
 auto fail=[&](const std::string& reason,NodeId n=kInvalidNodeId){throw rtlzz::ArithmeticError({phase,"invalid_expansion_graph",reason,r.id,n==kInvalidNodeId?"":std::to_string(n)});};
 if(r.inputs.size()!=2)fail("block tree requires two ordered logical operands");
 const int aw=r.template_parameters.at("lhs_width"),bw=r.template_parameters.at("rhs_width"),R=r.template_parameters.at("result_width"),b=r.template_parameters.at("block_width");
 if(billingWidth(p,r.inputs[0])!=aw||billingWidth(p,r.inputs[1])!=bw)fail("block parameters disagree with original operand widths");
 auto canonical=[&](Operand v){
  std::set<NodeId> seen;
  while(v.kind==OperandKind::Symbol){
   if(!seen.insert(v.node).second)fail("cyclic source provenance",v.node);
   const auto& s=p.signal(v.node);if(!s.driver)break;const auto& d=*s.driver;
   if(d.kind!=OperationKind::Assign&&d.kind!=OperationKind::ZExt&&d.kind!=OperationKind::Cast)break;
   if(d.operands.size()!=1||v.signed_view||d.operands[0].signed_view||d.operands[0].constant.signed_view||d.operands[0].type.width>v.type.width||!d.operands[0].type.array_dims.empty())break;
   v=d.operands[0];
  }return v;
 };
 auto sameSource=[&](const Operand& a,const Operand& b){auto x=canonical(a),y=canonical(b);
  if(x.kind!=y.kind||!sameType(x.type,y.type)||x.signed_view||y.signed_view)return false;
  if(x.kind==OperandKind::Symbol)return x.node==y.node;
  if(x.kind==OperandKind::Literal){auto xx=x.constant.limbs,yy=y.constant.limbs;while(!xx.empty()&&!xx.back())xx.pop_back();while(!yy.empty()&&!yy.back())yy.pop_back();return x.constant.width==y.constant.width&&xx==yy;}
  return false;
 };
 std::set<NodeId> products;std::map<NodeId,std::pair<int,int>> coordinates;std::set<std::pair<int,int>> covered;
 for(auto id:r.members){const auto& d=*p.signal(id).driver;if(d.kind!=OperationKind::Binary||d.op!=OpCode::Mul)continue;
  int offsets[2];int widths[]{aw,bw};
  for(int axis=0;axis<2;++axis){
   auto v=canonical(d.operands[axis]);if(v.kind!=OperandKind::Symbol)fail("partial operand is not an input block",id);
   const auto& slice=*p.signal(v.node).driver;
   if(slice.kind!=OperationKind::Slice||slice.operands.size()!=1||!sameSource(slice.operands[0],r.inputs[axis]))fail("partial block uses wrong original operand",v.node);
   const int lo=slice.lo;
   if(lo<0||lo>=widths[axis]||lo%b||slice.hi!=lo+std::min(b,widths[axis]-lo)-1||v.type.width!=std::min(b,widths[axis]-lo))fail("block coordinate or tail width mismatch",v.node);
   offsets[axis]=lo;
  }
  std::pair<int,int> xy{offsets[0],offsets[1]};if(!covered.insert(xy).second)fail("duplicate partial-product coordinate",id);
  coordinates[id]=xy;products.insert(id);
 }
 for(int a=0;a<aw;a+=b)for(int c=0;c<bw;c+=b)if(!covered.count({a,c}))fail("missing partial-product coordinate");
 auto isZero=[&](const Operand& v){return v.kind==OperandKind::Literal&&!v.constant.signed_view&&v.constant.isZero();};
 std::map<NodeId,unsigned> uses;std::set<NodeId> visiting;
 std::function<void(Operand,int)> term=[&](Operand value,int shift){
  value=canonical(value);if(value.kind!=OperandKind::Symbol)fail("reduction contains a non-product term");auto id=value.node;
  if(!visiting.insert(id).second)fail("cyclic reduction",id);
  const auto& d=*p.signal(id).driver;
  if(products.count(id)){
   const auto xy=coordinates.at(id);if(shift!=xy.first+xy.second)fail("partial-product shift disagrees with block coordinates",id);++uses[id];
  }else if(d.kind==OperationKind::Concat){
   if(d.operands.size()!=2||!isZero(d.operands[1])||d.operands[1].type.width<=0)fail("partial shift must concatenate explicit low zeros",id);
   auto v=canonical(d.operands[0]);if(v.kind!=OperandKind::Symbol||!products.count(v.node))fail("shift does not contain exactly one partial product",id);
   term(d.operands[0],shift+d.operands[1].type.width);
  }else if(d.kind==OperationKind::AddCarry){
   if(shift||d.operands.size()!=3||!isZero(d.operands[2]))fail("reduction has shifted sum or nonzero carry",id);
   term(d.operands[0],0);term(d.operands[1],0);
  }else fail("unexpected reduction connection",id);
  visiting.erase(id);
 };
 Operand output;output.node=r.outputs.at(0);output.type=p.signal(output.node).type;
 // Only the registered final low-R truncation may discard bits.
 const auto& final=*p.signal(output.node).driver;
 if(final.kind==OperationKind::Slice){if(final.lo!=0||final.hi!=R-1||final.operands.size()!=1)fail("incorrect final result truncation",output.node);output=final.operands[0];}
 term(output,0);
 for(auto id:products)if(uses[id]!=1)fail("reduction omits or repeats a partial product",id);
 if(uses.size()!=products.size())fail("reduction coverage mismatch");
}
}
