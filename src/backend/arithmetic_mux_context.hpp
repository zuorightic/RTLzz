#pragma once
#include "backend/beir.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace pred::beir::arithmetic {
inline constexpr const char* kMuxStaticFeatureSchema="arithmetic-mux-static-features-v1";
struct MuxStaticCapture { NodeId value; unsigned banks; };
// A candidate's actual combinational members and explicit logical state cut.
// Capture banks count retained logical slots BEFORE mapping, never OpenDB DFFs.
// This helper does not price another operation or invoke a physical tool.
inline std::map<std::string,double> muxStaticContext(
    const Program& p,const std::vector<NodeId>& members,
    const std::vector<MuxStaticCapture>& captures,NodeId query) {
    auto reject=[](const char* why)->void{throw std::invalid_argument(why);};
    auto signal=[&](NodeId id)->const Signal&{
        if(id>=p.signals.size()||p.signals[id].id!=id||!p.signals[id].driver)reject("invalid Mux static graph node");
        const auto& s=p.signals[id];if(s.type.isArray()||s.type.width<=0||s.type.width>2048||s.driver->type.width!=s.type.width)reject("unqualified static scalar width");return s;
    };
    auto alias=[&](NodeId id){std::set<NodeId> seen;
        for(;;){if(!seen.insert(id).second)reject("cyclic static alias");const auto& s=signal(id);const auto& d=*s.driver;
            if(d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast)break;
            if(d.operands.size()!=1||d.operands[0].kind!=OperandKind::Symbol||d.operands[0].type.width!=s.type.width)reject("unsupported static alias");id=d.operands[0].node;
        }return id;
    };
    auto literal=[&](const Operand& v){
        if(v.kind!=OperandKind::Literal)return std::string("dynamic");
        if(v.type.width<=0||v.type.width>2048||v.constant.width!=v.type.width)reject("invalid static literal");
        bool z=true,one=true,ones=true;for(int bit=0;bit<v.type.width;++bit){bool b=static_cast<std::size_t>(bit/64)<v.constant.limbs.size()&&((v.constant.limbs[bit/64]>>(bit%64))&1);
            z&=!b;one&=b==(bit==0);ones&=b;
        }return z?std::string("zero"):one?std::string("one"):ones?std::string("ones"):std::string("other");
    };
    std::set<NodeId> paid,raw;for(auto id:members){if(!raw.insert(id).second)reject("duplicate stage member");const auto& d=*signal(id).driver;
        if(d.kind==OperationKind::Assign||d.kind==OperationKind::Cast||d.kind==OperationKind::PortRead)continue;
        paid.insert(id);
    }
    if(paid.empty()||paid.size()>6)reject("unqualified static context cardinality");
    query=alias(query);if(!paid.count(query))reject("Mux query outside actual stage");
    std::map<NodeId,unsigned> banks;for(auto capture:captures){auto id=alias(capture.value);
        if(!paid.count(id)||!capture.banks||capture.banks>3||!banks.emplace(id,capture.banks).second)reject("invalid static capture cut");}
    if(banks.size()!=paid.size())reject("incomplete static capture cut");
    std::set<NodeId> inputs;std::set<int> widths;std::set<unsigned> bank_set;std::multiset<std::string> kinds;
    std::uint64_t launch=0,capture=0,results=0,dynamic=0,constant=0,boolean=0;int query_width=0;
    for(auto id:paid){const auto& s=signal(id);const auto& d=*s.driver;std::string kind;std::vector<Operand> data;std::optional<Operand> select;
        if(d.kind==OperationKind::Ite){
            if(d.operands.size()!=3||d.operands[0].type.width!=1||literal(d.operands[0])!="dynamic")reject("unqualified static selector");
            select=d.operands[0];if(literal(d.operands[1])!="dynamic"||d.operands[1].type.width!=d.operands[2].type.width)reject("unqualified reversed/static Mux data");
            data.push_back(d.operands[1]);auto rhs=literal(d.operands[2]);
            if(rhs=="dynamic"){kind="mux";data.push_back(d.operands[2]);++dynamic;}
            else {if(rhs=="other")reject("unqualified static Mux constant");kind="mux_"+rhs;++constant;}
        }else if(d.kind==OperationKind::Unary&&d.op==OpCode::LogicNot&&d.operands.size()==1){kind="eq_zero";data=d.operands;++boolean;}
        else if(d.kind==OperationKind::Binary&&d.op==OpCode::Eq&&d.operands.size()==2){kind="eq_zero";
            if(literal(d.operands[0])=="zero")data={d.operands[1]};else if(literal(d.operands[1])=="zero")data={d.operands[0]};else reject("unqualified static comparison");++boolean;
        }else if(d.kind==OperationKind::Binary&&d.op==OpCode::BitOr&&d.operands.size()==2){kind="or";data=d.operands;++boolean;}
        else reject("unsupported operation in Mux static context");
        if(data.empty())reject("static context lacks data");const int w=data[0].type.width;
        if(w<=0||w>64||(kind=="eq_zero"?s.type.width!=1:s.type.width!=w))reject("unqualified static result shape");
        for(const auto& v:data)if(v.type.width!=w)reject("asymmetric static control data");
        auto consume=[&](const Operand& v){
            if(v.kind!=OperandKind::Symbol)reject("unqualified dynamic static operand");auto input=alias(v.node);
            if(paid.count(input)||!inputs.insert(input).second)reject("shared or connected context not qualified");
            const auto& source=signal(input);if(source.driver->kind!=OperationKind::PortRead||source.type.width!=v.type.width||source.driver->operands.size()!=1||source.driver->operands[0].kind!=OperandKind::Port)
                reject("static context input is not a declared independent boundary");
            if(source.value.valid&&(source.value.constant||std::any_of(source.value.known_zero.begin(),source.value.known_zero.end(),[](auto x){return x!=0;})||
                std::any_of(source.value.known_one.begin(),source.value.known_one.end(),[](auto x){return x!=0;})))reject("known input pattern not qualified");
            launch+=source.type.width;
        };
        for(const auto& v:data)consume(v);if(select)consume(*select);
        if(id==query){if(kind!="mux")reject("query is not a dynamic Mux");query_width=s.type.width;}
        if(kind=="mux_ones"&&w==1)kind="mux_one";
        kinds.insert(kind);widths.insert(w);bank_set.insert(banks.at(id));results+=s.type.width;capture+=std::uint64_t(banks.at(id))*s.type.width;
    }
    std::multiset<std::string> mixed={"mux","mux_zero","mux_one","mux_ones","eq_zero","or"};
    if(widths==std::set<int>{1}){mixed.erase(mixed.find("mux_ones"));mixed.insert("mux_one");}
    if(widths.size()!=1||bank_set.size()!=1||!((dynamic==paid.size()&&dynamic>=2&&dynamic<=4)||kinds==mixed))reject("unqualified parallel context composition");
    if(!query_width||launch>640||capture>2048)reject("Mux static context outside capacity");
    return {{"context_operation_count",double(paid.size())},{"context_dynamic_mux_count",double(dynamic)},
        {"context_constant_mux_count",double(constant)},{"context_boolean_count",double(boolean)},
        {"context_launch_bits",double(launch)},{"context_capture_bits",double(capture)},
        {"context_result_bits",double(results)},{"selector_fanout_bits",double(query_width)}};
}
} // namespace pred::beir::arithmetic
