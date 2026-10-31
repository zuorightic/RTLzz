#pragma once
#include "backend/arithmetic_core.hpp"
#include <llvm/Support/JSON.h>
#include <limits>
#include <climits>
#include <stdexcept>
namespace arithmetic_replay {
using namespace pred::beir;
inline const llvm::json::Value& field(const llvm::json::Object& o,const char* key){auto* v=o.get(key);if(!v)throw std::runtime_error(std::string("replay missing field: ")+key);return *v;}
inline int integer(const llvm::json::Object& o,const char* key){auto n=field(o,key).getAsInteger();if(!n||*n<INT_MIN||*n>INT_MAX)throw std::runtime_error(std::string("replay integer: ")+key);return *n;}
inline std::string string(const llvm::json::Object& o,const char* key){auto n=field(o,key).getAsString();if(!n)throw std::runtime_error(std::string("replay string: ")+key);return n->str();}
inline bool boolean(const llvm::json::Object& o,const char* key){auto n=field(o,key).getAsBoolean();if(!n)throw std::runtime_error(std::string("replay boolean: ")+key);return *n;}
inline const llvm::json::Array& array(const llvm::json::Object& o,const char* key){auto* a=field(o,key).getAsArray();if(!a)throw std::runtime_error(std::string("replay array: ")+key);return *a;}
inline const llvm::json::Object& object(const llvm::json::Value& v){auto* o=v.getAsObject();if(!o)throw std::runtime_error("replay object required");return *o;}
inline std::uint64_t word(const llvm::json::Value& v){auto s=v.getAsString();if(!s||s->empty()||s->str().find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("replay decimal identity/limb required");std::size_t end=0;auto n=std::stoull(s->str(),&end);if(end!=s->size())throw std::runtime_error("replay invalid decimal");return n;}
inline std::vector<std::uint64_t> words(const llvm::json::Array& values){std::vector<std::uint64_t> a;for(const auto& v:values)a.push_back(word(v));return a;}
inline ValueType type(const llvm::json::Object& o){ValueType t;t.width=integer(o,"width");for(const auto& v:array(o,"dims")){auto d=v.getAsInteger();if(!d||*d<1||*d>INT_MAX)throw std::runtime_error("replay dimension");t.array_dims.push_back(*d);}return t;}
inline Program graph(const llvm::json::Object& report){
 const auto& g=object(field(report,"graph"));Program p;p.function_name="historical_replay";
 for(const auto& value:array(g,"nodes")){const auto& n=object(value);Signal s;s.id=word(field(n,"id"));s.name="replay_"+std::to_string(s.id);s.type=type(n);Operation d;d.type=s.type;d.kind=static_cast<OperationKind>(integer(n,"kind"));d.op=static_cast<OpCode>(integer(n,"opcode"));d.hi=integer(n,"hi");d.lo=integer(n,"lo");d.bit=integer(n,"bit");d.times=integer(n,"times");d.to_width=integer(n,"to_width");d.signed_truncation=boolean(n,"signed_truncation");
  for(const auto& value:array(n,"operands")){const auto& o=object(value);Operand v;v.kind=static_cast<OperandKind>(integer(o,"kind"));v.type=type(o);v.signed_view=boolean(o,"signed");v.constant.signed_view=boolean(o,"constant_signed");v.constant.width=integer(o,"constant_width");v.constant.limbs=words(array(o,"limbs"));if(v.kind==OperandKind::Symbol)v.node=word(field(o,"node"));else if(!string(o,"node").empty())throw std::runtime_error("unexpected non-symbol identity");d.operands.push_back(v);}
  s.driver=std::move(d);if(s.id!=p.signals.size())throw std::runtime_error("replay non-dense IDs");p.signals.push_back(std::move(s));
 }
 for(const auto& value:array(g,"ports")){const auto& o=object(value);Port port;port.name=string(o,"name");port.direction=static_cast<PortDirection>(integer(o,"direction"));port.type=type(o);port.element_nodes=words(array(o,"nodes"));
  if(port.type.isArray())throw std::runtime_error("historical reader supports this scalar archive only; array metadata needs a new adapter");
  for(auto id:port.element_nodes){auto& s=p.signal(id);s.port_name=port.name;s.port_element_index=0;s.name=port.name;if(port.direction==PortDirection::Input){p.inputs.push_back(port.name);if(!s.driver||s.driver->kind!=OperationKind::PortRead)throw std::runtime_error("missing historical input driver");s.driver->operands.at(0).text=port.name;}else p.outputs.push_back(port.name);}
  p.ports.push_back(port);
 }
 for(const auto& value:array(g,"regions")){const auto& o=object(value);ArithmeticRegion r;r.id=string(o,"id");r.operation=string(o,"operation");r.semantics=string(o,"semantic_version");r.template_version=string(o,"template_id");r.sharing_policy=string(o,"sharing_policy");r.internal_split_enabled=boolean(o,"internally_cuttable");r.members=words(array(o,"members"));r.outputs=words(array(o,"outputs"));
  for(const auto& [key,value]:object(field(o,"parameters"))){auto n=value.getAsInteger();if(!n||*n<0||*n>INT_MAX)throw std::runtime_error("replay template parameter");r.template_parameters[key.str()]=*n;}
  for(const auto& value:array(o,"legal_groups")){auto* group=value.getAsArray();if(!group)throw std::runtime_error("replay group");r.legal_cut_groups.push_back(words(*group));}
  // Only derived, non-numerical metadata: report-v2 did not serialize this
  // registered policy string. No nodes/widths/coordinates are synthesized.
  if(r.template_version=="mul-block-tree-v1")r.split_version="partial-products-reduction-levels-v1";
  for(const auto& value:array(o,"inputs")){const auto& n=object(value);Operand v;v.type.width=integer(n,"width");v.signed_view=boolean(n,"signed_view");v.constant.width=integer(n,"constant_width");v.constant.signed_view=boolean(n,"constant_signed");v.constant.limbs=words(array(n,"constant_limbs"));auto id=string(n,"node");if(id=="literal")v.kind=OperandKind::Literal;else{v.kind=OperandKind::Symbol;v.node=word(field(n,"node"));v.type.array_dims=p.signal(v.node).type.array_dims;if(!v.type.array_dims.empty())throw std::runtime_error("historical region boundary dimensions not fully serialized");}r.inputs.push_back(v);}
  p.arithmetic_regions.push_back(r);
 }
 // Facts are recomputed from the saved typed graph, not guessed from vectors.
 MutableProgram facts(std::move(p));facts.ensureValueFacts();return facts.finish();
}
}
