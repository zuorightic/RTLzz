#include "backend/pipeline.hpp"
#include "backend/arithmetic_regions.hpp"
#include "backend/arithmetic_core.hpp"
#include "backend/rtl_names.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace pred::pipeline {
namespace {
using namespace beir;
using Key = rtlzz::PipelineOperation;

Key key(const Operation& op) {
    if (op.kind == OperationKind::Binary || op.kind == OperationKind::Unary) switch (op.op) {
    case OpCode::Add: return Key::Add;
    case OpCode::Sub: return Key::Subtract;
    case OpCode::Mul: return Key::Multiply;
    case OpCode::Div: return Key::Divide;
    case OpCode::Mod: return Key::Modulo;
    case OpCode::BitAnd: return Key::BitAnd;
    case OpCode::BitOr: return Key::BitOr;
    case OpCode::BitXor: return Key::BitXor;
    case OpCode::LogicAnd: return Key::LogicalAnd;
    case OpCode::LogicOr: return Key::LogicalOr;
    case OpCode::Eq: return Key::Equal;
    case OpCode::Ne: return Key::NotEqual;
    case OpCode::Lt: return Key::Less;
    case OpCode::Le: return Key::LessEqual;
    case OpCode::Gt: return Key::Greater;
    case OpCode::Ge: return Key::GreaterEqual;
    case OpCode::Shl: return Key::ShiftLeft;
    case OpCode::Shr: return Key::ShiftRight;
    case OpCode::LogicNot: return Key::LogicalNot;
    case OpCode::BitNot: return Key::BitNot;
    case OpCode::Neg: return Key::Negate;
    case OpCode::None: break;
    }
    switch (op.kind) {
    case OperationKind::Assign: return Key::Assign;
    case OperationKind::AddCarry: return Key::AddCarry;
    case OperationKind::Ite: return Key::Mux;
    case OperationKind::Case: return Key::Case;
    case OperationKind::Lookup: return Key::Lookup;
    case OperationKind::ArrayAccess: return Key::ArrayAccess;
    case OperationKind::Cast: return Key::Cast;
    case OperationKind::ZExt: return Key::ZeroExtend;
    case OperationKind::SExt: return Key::SignExtend;
    case OperationKind::Trunc: return Key::Truncate;
    case OperationKind::Slice: return Key::Slice;
    case OperationKind::BitSelect: return Key::BitSelect;
    case OperationKind::WriteSlice: return Key::WriteSlice;
    case OperationKind::WriteBit: return Key::WriteBit;
    case OperationKind::Concat: return Key::Concat;
    case OperationKind::Repeat: return Key::Repeat;
    case OperationKind::ReduceOr: return Key::ReduceOr;
    case OperationKind::ReduceAnd: return Key::ReduceAnd;
    case OperationKind::ReduceXor: return Key::ReduceXor;
    case OperationKind::DynamicBitSelect: return Key::DynamicBitSelect;
    case OperationKind::DynamicSlice: return Key::DynamicSlice;
    case OperationKind::DynamicWriteSlice: return Key::DynamicWriteSlice;
    case OperationKind::DynamicWriteBit: return Key::DynamicWriteBit;
    case OperationKind::Aggregate: return Key::Aggregate;
    case OperationKind::Call: return Key::Call;
    default: throw std::runtime_error("pipeline: unsupported operation cost key");
    }
}

std::uint64_t bits(const ValueType& type) {
    if (type.width <= 0) throw std::runtime_error("pipeline: invalid signal width");
    auto result = static_cast<std::uint64_t>(type.width);
    for (int dim : type.array_dims) {
        if (dim <= 0 || result > UINT64_MAX / static_cast<unsigned>(dim))
            throw std::runtime_error("pipeline: register size overflow");
        result *= static_cast<unsigned>(dim);
    }
    return result;
}

struct Graph {
    const beir::Program& p;
    int last;
    bool fixed_cost = false;
    bool model_cost = false;
    mutable bool uncovered_stage_attempt = false;
    std::unique_ptr<beir::arithmetic::CostProvider> provider;
    NodeId valid_input = kInvalidNodeId, valid_output = kInvalidNodeId;
    std::vector<NodeId> order;
    std::vector<std::vector<NodeId>> users;
    std::vector<bool> live, input, output, constant;
    std::vector<double> costs;
    std::vector<std::uint64_t> areas;
    std::vector<double> physical_areas;
    std::vector<bool> stage_deferred;
    std::vector<NodeId> unit_order, unit_of;
    std::vector<std::vector<NodeId>> units;
    std::vector<std::string> unit_labels;

    Graph(const beir::Program& program, const Options& options)
        : p(program), last(options.register_stages), users(p.signals.size()),
          live(p.signals.size()), input(live.size()), output(live.size()),
          constant(live.size()), costs(live.size()), areas(live.size()),physical_areas(live.size()),stage_deferred(live.size()) {
        fixed_cost=options.arithmetic_request.has_value();
        model_cost=options.arithmetic_request&&options.arithmetic_request->cost_mode=="backend-model";
        if(options.arithmetic_request)provider=arithmetic::makeCostProvider(p,*options.arithmetic_request);
        if(model_cost){provider->validateStateProtocol(options.reset_enabled,!options.input_valid_name.empty(),true);provider->validateInterface(p,options.output_valid_name);}
        if(model_cost&&(provider->dataBudget(true)<=0||provider->dataBudget(false)<=0))
            throw rtlzz::ArithmeticError({"scheduling","nonpositive_data_budget","target does not exceed model boundary/clock overhead in ps",{},{}});
        if (options.arithmetic_request) beir::arithmetic::validate(p,"cost_preflight");
        if (!p.arithmetic_regions.empty()) beir::regions::validate(p);
        for(const auto& r:p.arithmetic_regions) if(!r.internal_registers.empty())
            throw std::runtime_error("pipeline: cannot schedule materialized region registers");
        if (!options.input_valid_name.empty()) {
            for (const auto& port : p.ports) {
                if (port.name != options.input_valid_name && port.name != options.output_valid_name) continue;
                const bool is_input = port.name == options.input_valid_name;
                if (port.direction != (is_input ? PortDirection::Input : PortDirection::Output) ||
                    port.type.width != 1 || port.type.isArray() || port.element_nodes.size() != 1)
                    throw std::runtime_error("pipeline: invalid valid port: " + port.name);
                (is_input ? valid_input : valid_output) = port.element_nodes.front();
            }
            if (valid_input == kInvalidNodeId || valid_output == kInvalidNodeId)
                throw std::runtime_error("pipeline: unknown valid port");
        }
        std::vector<NodeId> pending;
        for (const auto& port : p.ports) {
            for (auto id : port.element_nodes) {
                p.signal(id);
                if (port.direction == PortDirection::Input) input[id] = true;
                if (port.direction == PortDirection::Output) output[id] = true;
                pending.push_back(id);
            }
        }
        for (const auto& r : p.arithmetic_regions) {
            pending.insert(pending.end(), r.outputs.begin(), r.outputs.end());
            for (const auto& v : r.inputs) if (v.kind == OperandKind::Symbol) pending.push_back(v.node);
        }
        while (!pending.empty()) {
            auto id = pending.back(); pending.pop_back();
            const auto& s = p.signal(id);
            if (live[id]) continue;
            live[id] = true;
            if (s.driver) for (const auto& v : s.driver->operands)
                if (v.kind == OperandKind::Symbol) pending.push_back(v.node);
        }
        std::vector<std::size_t> indegree(live.size());
        std::size_t count = 0;
        for (const auto& s : p.signals) {
            if (!live.at(s.id)) continue;
            ++count;
            if (!s.driver) {
                if (!input[s.id]) throw std::runtime_error("pipeline: undriven live signal " + s.name);
                continue;
            }
            if (s.driver->kind == OperationKind::PortRead) input[s.id] = true;
            for (const auto& v : s.driver->operands) {
                if (v.kind == OperandKind::Symbol) {
                    users[v.node].push_back(s.id);
                    ++indegree[s.id];
                } else if (v.kind == OperandKind::Port && !input[s.id]) {
                    throw std::runtime_error("pipeline: raw port dependency outside PortRead");
                }
            }
        }
        std::priority_queue<NodeId, std::vector<NodeId>, std::greater<NodeId>> ready;
        for (const auto& s : p.signals) if (live[s.id] && !indegree[s.id]) ready.push(s.id);
        while (!ready.empty()) {
            auto id = ready.top(); ready.pop(); order.push_back(id);
            for (auto user : users[id]) if (!--indegree[user]) ready.push(user);
        }
        if (order.size() != count) throw std::runtime_error("pipeline: cyclic dependency graph");
        unit_of.resize(live.size()); units.resize(live.size());unit_labels.resize(live.size());
        for (NodeId id=0; id<live.size(); ++id) {unit_of[id]=id;unit_labels[id]="node#"+std::to_string(id);}
        for (const auto& r:p.arithmetic_regions) {
            if (r.internal_split_enabled) {
                unsigned group_index=0;
                for (const auto& group : r.legal_cut_groups) {
                    auto representative=*std::min_element(group.begin(),group.end());
                    unit_labels[representative]=r.id+"/group"+std::to_string(group_index++);
                    for(auto id:group) unit_of[id]=representative;
                }
            } else {
                auto representative=*std::min_element(r.members.begin(),r.members.end());
                unit_labels[representative]=r.id+"/atomic";
                for(auto id:r.members) unit_of[id]=representative;
            }
        }
        for(auto id:order) units[unit_of[id]].push_back(id);
        std::vector<std::unordered_set<NodeId>> unit_users(live.size());
        std::vector<unsigned> unit_degree(live.size());
        for(auto id:order) for(auto user:users[id]) if(unit_of[id]!=unit_of[user] && unit_users[unit_of[id]].insert(unit_of[user]).second)
            ++unit_degree[unit_of[user]];
        std::priority_queue<NodeId,std::vector<NodeId>,std::greater<NodeId>> unit_ready;
        for(NodeId u=0;u<units.size();++u) if(!units[u].empty() && !unit_degree[u]) unit_ready.push(u);
        std::vector<NodeId> grouped;
        while(!unit_ready.empty()) {
            auto u=unit_ready.top();unit_ready.pop();unit_order.push_back(u);
            grouped.insert(grouped.end(),units[u].begin(),units[u].end());
            for(auto v:unit_users[u]) if(!--unit_degree[v]) unit_ready.push(v);
        }
        if(grouped.size()!=order.size()) throw std::runtime_error("pipeline: cyclic atomic graph");
        order=std::move(grouped);
        for (auto id : order) {
            const auto& s = p.signal(id);
            if (input[id]) continue; // External Q and PortRead are wiring.
            const auto& op = *s.driver;
            constant[id] = op.kind != OperationKind::Call;
            for (const auto& v : op.operands)
                if (v.kind != OperandKind::Literal &&
                    (v.kind != OperandKind::Symbol || !constant[v.node])) constant[id] = false;
            if(model_cost&&constant[id])continue;
            auto fixed = provider ? provider->node(p,id) : arithmetic::CostEstimate{};
            if(options.arithmetic_request && !fixed.covered) throw rtlzz::ArithmeticError({"cost","cost_uncovered",fixed.reason,{},std::to_string(id)});
            if (constant[id]) continue;
            auto k = key(op);
            if(options.arithmetic_request) {
                stage_deferred[id]=fixed.deferred_to_stage;costs[id]=fixed.delay;if(model_cost)physical_areas[id]=fixed.combinational;else areas[id]=fixed.combinational;
            } else {
                auto it = options.operation_delay_weights.find(k);
                costs[id] = it != options.operation_delay_weights.end() ? it->second
                    : k == Key::Assign ? 0 : k == Key::Multiply ? 3 : 1;
            }
        }
        if(model_cost&&std::none_of(costs.begin(),costs.end(),[](double value){return value>0;})&&std::none_of(stage_deferred.begin(),stage_deferred.end(),[](bool value){return value;}))
            throw rtlzz::ArithmeticError({"cost","cost_uncovered","pure wiring/state-only application has no qualified arithmetic-stage model",{},{}});
    }

    // Earliest feasible placement under a cost cap, with output sinks fixed at N.
    bool schedule(double cap, std::vector<int>& stage, std::string* diagnostic=nullptr) const {
        uncovered_stage_attempt=false;
        stage.assign(live.size(), 0);
        std::vector<double> arrival(live.size());
        std::vector<double> stage_peak(last+1,0);
        std::vector<std::size_t> stage_operations(last+1,0);
        std::vector<std::uint64_t> stage_bits(last+1,0);
        std::vector<std::vector<NodeId>> stage_members(last+1);
        for(auto u:unit_order) {
            int start=0; bool fixed=false;
            for(auto id:units[u]) {
                if(output[id]) fixed=true;
                if(p.signal(id).driver) for(const auto& v:p.signal(id).driver->operands)
                    if(v.kind==OperandKind::Symbol && unit_of[v.node]!=u && !constant[v.node]) start=std::max(start,stage[v.node]);
            }
            if(fixed) start=last;
            bool placed=false;
            double peak=0;
            for(int at=start;at<=last && at<=start+1;++at) {
                bool fits=true;
                double trial_peak=0;std::size_t operations=0;std::uint64_t value_bits=0;
                for(auto id:units[u]) {
                    stage[id]=at;arrival[id]=0;
                    if(input[id]) { if(at) fits=false; continue; }
                    if(constant[id]) continue;
                    double a=0;
                    for(const auto& v:p.signal(id).driver->operands)
                        if(v.kind==OperandKind::Symbol && !constant[v.node] && stage[v.node]==at)
                            a=std::max(a,arrival[v.node]);
                    arrival[id]=a+costs[id];peak=std::max(peak,arrival[id]);trial_peak=std::max(trial_peak,arrival[id]);
                    if(costs[id]>0||stage_deferred[id]){++operations;value_bits+=bits(p.signal(id).type);}
                    if(!model_cost&&arrival[id]>cap) fits=false;
                }
                const double limit=model_cost?std::min(cap,provider->dataBudget(at==0)):cap;
                double corrected=trial_peak;
                auto members=stage_members[at];members.insert(members.end(),units[u].begin(),units[u].end());
                if(model_cost){
                    bool covered=provider->stageCovered(stage_operations[at]+operations,stage_bits[at]+value_bits)&&provider->stageShapeCovered(members);
                    if(!covered)uncovered_stage_attempt=true;
                    corrected=covered?provider->stageDelay(std::max(stage_peak[at],trial_peak),stage_operations[at]+operations,stage_bits[at]+value_bits,members):std::numeric_limits<double>::infinity();
                }
                if(corrected>limit)fits=false;
                if(fits) {stage_peak[at]=std::max(stage_peak[at],trial_peak);stage_operations[at]+=operations;stage_bits[at]+=value_bits;stage_members[at]=std::move(members);placed=true;break;}
                if(fixed) break;
            }
            if(!placed) {
                if(diagnostic) *diagnostic="group="+unit_labels[u]+" legal_stages="+std::to_string(start)+".."+
                    std::to_string(last)+" N="+std::to_string(last)+" cap="+std::to_string(cap)+
                    " observed_arrival="+std::to_string(peak)+"; group cannot be cut internally";
                return false;
            }
        }
        return true;
    }

    Report evaluate(const std::vector<int>& stage) const {
        Report r; r.stage_delays.assign(static_cast<std::size_t>(last) + 1, 0);
        std::size_t max_state_width=0,min_variable_word=std::numeric_limits<std::size_t>::max();
        auto variable_bits=[&](NodeId id){
            const auto& s=p.signal(id);if(s.type.isArray())throw rtlzz::ArithmeticError({"cost","cost_uncovered","array state has no validated model",{},{}});
            std::uint64_t variable=s.type.width;
            if(s.value.valid)for(int bit=0;bit<s.type.width;++bit){auto word=static_cast<std::size_t>(bit/64);auto mask=std::uint64_t(1)<<(bit%64);
                if((word<s.value.known_zero.size()&&(s.value.known_zero[word]&mask))||(word<s.value.known_one.size()&&(s.value.known_one[word]&mask)))--variable;
            }return variable;
        };
        auto add_state=[&](NodeId id,std::uint64_t length,bool control){
            if(!length)return;auto variable=variable_bits(id);if(!variable)return;
            if(variable>(UINT64_MAX-r.model_state_bits)/length)throw rtlzz::ArithmeticError({"cost","cost_uncovered","variable state bit count overflow",{},{}});
            r.model_state_bits+=variable*length;r.model_state_signals+=static_cast<std::size_t>(length);
            if(!control){max_state_width=std::max(max_state_width,static_cast<std::size_t>(p.signal(id).type.width));min_variable_word=std::min(min_variable_word,static_cast<std::size_t>(variable));}
        };
        std::vector<std::size_t> stage_operations(last+1,0);std::vector<std::uint64_t> stage_bits(last+1,0);
        std::vector<double> stage_areas(last+1,0);
        std::vector<std::vector<NodeId>> stage_members(last+1);
        // Context prices belong to this complete proposed placement. Re-query
        // after every trial move; never reuse a NodeId-only contextual price.
        auto actual_costs=costs;
        auto actual_areas=physical_areas;
        if(model_cost&&provider->requiresStageContext()) {
            for(auto id:order) {
                if(stage[id]<0||stage[id]>last||(input[id]&&stage[id]!=0)||(output[id]&&stage[id]!=last)) {
                    r.final_delay=std::numeric_limits<double>::infinity();return r;
                }
                for(const auto& operand:p.signal(id).driver->operands)if(operand.kind==OperandKind::Symbol&&stage[operand.node]>stage[id]) {
                    r.final_delay=std::numeric_limits<double>::infinity();return r;
                }
            }
            std::vector<NodeId> controls;if(valid_input!=kInvalidNodeId)controls={valid_input,valid_output};
            for(int at=0;at<=last;++at) {
                const auto cut=arithmetic::stageCostQuery(p,stage,at,last,controls);
                for(auto id:cut.members) {
                    const auto value=provider->nodeInStage(p,id,cut);
                    if(value.deferred_to_stage||!value.covered)throw rtlzz::ArithmeticError({"cost","cost_uncovered","actual stage query did not return a qualified component",{},std::to_string(id)});
                    actual_costs[id]=value.delay;actual_areas[id]=value.combinational;
                }
            }
        }
        std::vector<double> arrival(live.size());
        for (auto u : unit_order) for (auto id : units[u]) if (stage[id] != stage[units[u].front()]) {
            r.final_delay = std::numeric_limits<double>::infinity(); return r;
        }
        for (auto id : order) {
            if (stage[id] < 0 || stage[id] > last || (input[id] && stage[id] != 0) ||
                (output[id] && stage[id] != last)) {
                r.final_delay = std::numeric_limits<double>::infinity(); return r;
            }
            const auto& s = p.signal(id);
            if (constant[id] || input[id]) continue;
            stage_members[stage[id]].push_back(id);
            double a = 0;
            for (const auto& v : s.driver->operands) {
                if (v.kind != OperandKind::Symbol || constant[v.node]) continue;
                if (stage[v.node] > stage[id]) {
                    r.final_delay = std::numeric_limits<double>::infinity(); return r;
                }
                if (stage[v.node] == stage[id]) a = std::max(a, arrival[v.node]);
            }
            arrival[id] = a + actual_costs[id];
            r.stage_delays[stage[id]] = std::max(r.stage_delays[stage[id]], arrival[id]);
            if(actual_costs[id]>0||stage_deferred[id]){++stage_operations[stage[id]];stage_bits[stage[id]]+=bits(s.type);}
        }
        for (auto id : order) {
            if (constant[id]) continue;
            if (!input[id] && areas[id] > UINT64_MAX - r.combinational_cost)
                throw std::runtime_error("pipeline: combinational cost overflow");
            if(!input[id]) r.combinational_cost += areas[id];
            if(!input[id]){r.predicted_comb_area_um2+=actual_areas[id];stage_areas[stage[id]]+=actual_areas[id];}
            int end = stage[id];
            for (auto user : users[id]) end = std::max(end, stage[user]);
            if (id == valid_input) end = last; // Dedicated chain, independent of data consumers.
            auto width = bits(p.signal(id).type);
            auto length = static_cast<std::uint64_t>(end - stage[id]);
            if (length && width > (UINT64_MAX - r.register_bits) / length)
                throw std::runtime_error("pipeline: register bit count overflow");
            r.register_bits += width * length;
            r.register_count += static_cast<std::size_t>(length);
            if(model_cost)add_state(id,length,id==valid_input);
        }
        if(fixed_cost) r.cost_model="fixed-op-width-v1";
        if(model_cost){
            r.predicted_comb_area_um2=0;
            r.cost_model=provider->modelIdentity();r.input_data_budget_ps=provider->dataBudget(true);r.register_data_budget_ps=provider->dataBudget(false);r.model_feasible=true;
            for(int at=0;at<=last;++at){
                if(!provider->stageCovered(stage_operations[at],stage_bits[at])||!provider->stageShapeCovered(stage_members[at])){r.final_delay=std::numeric_limits<double>::infinity();r.model_feasible=false;return r;}
                r.stage_delays[at]=provider->stageDelay(r.stage_delays[at],stage_operations[at],stage_bits[at],stage_members[at]);if(r.stage_delays[at]>provider->dataBudget(at==0))r.model_feasible=false;
                r.predicted_comb_area_um2+=provider->stageArea(stage_areas[at],stage_operations[at],stage_bits[at],stage_members[at]);
            }
            for(const auto& port:p.ports)if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)add_state(id,1,id==valid_output);
            r.predicted_state_area_um2=provider->stateArea(r.model_state_bits,r.model_state_signals,max_state_width,min_variable_word);
            r.predicted_total_area_um2=r.predicted_comb_area_um2+r.predicted_state_area_um2;
        }
        r.final_delay = *std::max_element(r.stage_delays.begin(), r.stage_delays.end());
        return r;
    }
};
} // namespace

bool feasibleSchedule(const beir::Program& p,const Options& o,double cap,std::string& diagnostic) {
    validateOptions(o);
    if(!std::isfinite(cap) || cap<0) throw std::runtime_error("pipeline: invalid feasibility cap");
    auto normalized=p;
    if(o.arithmetic_request) {beir::MutableProgram graph(normalized);graph.ensureValueFacts();normalized=graph.finish();}
    Graph graph(normalized,o);std::vector<int> stages;diagnostic.clear();
    return graph.schedule(cap,stages,&diagnostic);
}

Report evaluateSchedule(const beir::Program& p,const Options& o,const std::vector<int>& stages) {
    validateOptions(o);if(stages.size()!=p.signals.size())throw rtlzz::ArithmeticError({"scheduling","schedule_validation_error","stage map size mismatch",{},{}});
    beir::MutableProgram facts(p);facts.ensureValueFacts();auto graph=facts.finish();return Graph(graph,o).evaluate(stages);
}

void requireSchedule(const beir::Program& p,const Options& o,double cap) {
    validateOptions(o);if(!o.arithmetic_request||!std::isfinite(cap)||cap<0)throw rtlzz::ArithmeticError({"request_validation","invalid_request","finite abstract cap and arithmetic request required",{},{}});
    beir::MutableProgram facts(p);facts.ensureValueFacts();auto normalized=facts.finish();Graph graph(normalized,o);
    for(auto u:graph.unit_order) {
        double lower=0;std::vector<double> arrivals(normalized.signals.size());
        for(auto id:graph.units[u]) {
            double a=0;for(const auto& v:normalized.signal(id).driver->operands)if(v.kind==OperandKind::Symbol&&graph.unit_of[v.node]==u)a=std::max(a,arrivals[v.node]);
            arrivals[id]=a+graph.costs[id];lower=std::max(lower,arrivals[id]);
        }
        if(lower>cap)throw rtlzz::ArithmeticError({"scheduling","schedule_group_over_budget","indivisible group lower bound="+std::to_string(lower)+" cap="+std::to_string(cap)+" unit=abstract",graph.unit_labels[u],{}});
    }
    std::vector<int> stages;std::string reason;
    if(!graph.schedule(cap,stages,&reason))throw rtlzz::ArithmeticError({"scheduling","schedule_no_placement_found","earliest grouped strategy: "+reason,{},{}});
}

void validateOptions(const Options& o) {
    if(o.arithmetic_request) {
        rtlzz::validateArithmeticRequest(*o.arithmetic_request,o.register_stages);
        if(!o.operation_delay_weights.empty() || o.timing_relaxation_ratio!=0.05)
            throw rtlzz::ArithmeticError({"request_validation","invalid_request","new request conflicts with legacy cost overrides",{}, {}});
    }
    if (o.register_stages < 0 || o.register_stages == std::numeric_limits<int>::max())
        throw std::runtime_error("pipeline: register_stages must be non-negative and less than INT_MAX");
    if (!std::isfinite(o.timing_relaxation_ratio) || o.timing_relaxation_ratio < 0)
        throw std::runtime_error("pipeline: timing_relaxation_ratio must be finite and non-negative");
    for (const auto& [k, v] : o.operation_delay_weights) {
        if (k < Key::Assign || k > Key::Call || !std::isfinite(v) || v < 0)
            throw std::runtime_error("pipeline: invalid operation delay weight");
    }
    if (o.input_valid_name.empty() != o.output_valid_name.empty())
        throw std::runtime_error("pipeline: input_valid_name and output_valid_name must be configured together");
    if (!o.input_valid_name.empty() && o.input_valid_name == o.output_valid_name)
        throw std::runtime_error("pipeline: valid input and output names must differ");
    if (o.reset_enabled) {
        if (o.reset_name.empty() || !(std::isalpha(static_cast<unsigned char>(o.reset_name[0])) || o.reset_name[0] == '_') ||
            o.reset_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos || rtl_names::isSystemVerilogKeyword(o.reset_name))
            throw std::runtime_error("pipeline: reset_name must be a non-reserved RTL identifier");
        if (o.register_stages && o.reset_name == o.clock_name) throw std::runtime_error("pipeline: clock and reset names must differ");
    } else if (!o.input_reset_values.empty()) throw std::runtime_error("pipeline: input_reset_values requires reset_enabled");
    if (o.register_stages) {
        if (o.clock_name.empty() || !(std::isalpha(static_cast<unsigned char>(o.clock_name[0])) || o.clock_name[0] == '_') ||
            o.clock_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            throw std::runtime_error("pipeline: clock_name must be an RTL identifier");
        if (rtl_names::isSystemVerilogKeyword(o.clock_name))
            throw std::runtime_error("pipeline: clock_name is a reserved RTL identifier");
    }
}

Program build(const beir::Program& program, const Options& options) {
    validateOptions(options);
    beir::Program normalized = program;
    if (options.arithmetic_request) {
        beir::MutableProgram facts(normalized);
        facts.ensureValueFacts();
        normalized = facts.finish();
    }
    Program result;
    result.options = options;
    result.combinational = normalized;
    result.rtl_program = normalized;
    if (!options.register_stages && !options.reset_enabled && options.input_valid_name.empty()) {
        if(!normalized.arithmetic_regions.empty()) beir::regions::validate(normalized);
        if(options.arithmetic_request) {
            Graph graph(normalized,options);
            result.stages.assign(normalized.signals.size(),0);
            result.report=graph.evaluate(result.stages);
            result.report.baseline_delay=result.report.final_delay;
            result.report.allowed_delay=result.report.final_delay*105.0/100.0;
            if(!graph.model_cost)result.report.cost_model="fixed-op-width-v1";
        }
        result.initial_stages=result.stages;
        return result;
    }
    Graph g(normalized, options);
    if (g.valid_input != kInvalidNodeId) result.valid_nodes.push_back(g.valid_input);
    auto connect_valid_output = [&]() {
        if (result.valid_nodes.empty()) return;
        auto& output = result.rtl_program.signal(g.valid_output);
        output.value = {}; // Discard the frontend constant placeholder facts.
        Operation driver; driver.kind = OperationKind::Assign; driver.type = output.type;
        Operand value; value.kind = OperandKind::Symbol; value.node = result.valid_nodes.back(); value.type = output.type;
        driver.operands.push_back(value); output.driver = std::move(driver);
        addDebugMessage(output.debug, "backend-owned pipeline valid output");
    };
    if (!options.register_stages) {
        if (options.reset_enabled || !result.valid_nodes.empty()) {
            const auto& graph = g;
            result.stages.assign(program.signals.size(), 0);
            result.report = graph.evaluate(result.stages);
            result.report.baseline_delay = result.report.final_delay;
            result.report.allowed_delay = result.report.final_delay * (1 + options.timing_relaxation_ratio);
        }
        connect_valid_output();
        initializeResetValues(result);
        if(g.model_cost){bool zero=true;for(const auto& [name,words]:result.report.output_reset_values)for(auto v:words)zero&=v==0;g.provider->validateStateProtocol(options.reset_enabled,!options.input_valid_name.empty(),zero);}
        result.initial_stages=result.stages;
        return result;
    }
    double upper = 0, lower = 0;
    std::size_t all_operations=0;std::uint64_t all_bits=0;
    for (auto id : g.order) {
        upper += g.costs[id]; lower = std::max(lower, g.costs[id]);
        if(g.costs[id]>0||g.stage_deferred[id]){++all_operations;all_bits+=bits(normalized.signal(id).type);}
    }
    if(g.model_cost){
        lower=0; // A fitted stage scale can reduce the raw node-path proxy.
        if(g.provider->dataBudget(true)<=0||g.provider->dataBudget(false)<=0)
            throw rtlzz::ArithmeticError({"scheduling","nonpositive_data_budget","target does not exceed declared boundary/clock overhead in ps",{},{}});
        const double available=g.last?std::max(g.provider->dataBudget(true),g.provider->dataBudget(false)):g.provider->dataBudget(true);
        for(auto u:g.unit_order){double longest=0;std::size_t ops=0;std::uint64_t value_bits=0;std::vector<double> arrivals(normalized.signals.size());
            for(auto id:g.units[u]){double a=0;for(const auto& v:normalized.signal(id).driver->operands)if(v.kind==OperandKind::Symbol&&g.unit_of[v.node]==u)a=std::max(a,arrivals[v.node]);arrivals[id]=a+g.costs[id];longest=std::max(longest,arrivals[id]);if(g.costs[id]>0||g.stage_deferred[id]){++ops;value_bits+=bits(normalized.signal(id).type);}}
            if(!g.provider->stageShapeCovered(g.units[u]))throw rtlzz::ArithmeticError({"cost","cost_uncovered","indivisible group is outside validated stage graph domain",g.unit_labels[u],{}});
            if(g.provider->monotoneGroupBounds()){
                longest=g.provider->stageDelay(longest,ops,value_bits,g.units[u]);
                if(longest>available)throw rtlzz::ArithmeticError({"scheduling","schedule_group_over_budget","model indivisible group lower bound="+std::to_string(longest)+" ps exceeds every available stage budget="+std::to_string(available)+" ps",g.unit_labels[u],{}});
            }
        }
        // The unscheduled full graph need not fit one validated stage. Start
        // with the absolute data cap, then query each actual stage during
        // placement; never price a fictitious all-graph stage out of domain.
        upper=std::max(upper,std::max(g.provider->dataBudget(true),g.provider->dataBudget(false)));
    }
    if (!std::isfinite(upper)) throw std::runtime_error("pipeline: delay sum overflow");
    std::vector<int> baseline;
    if (!g.schedule(upper, baseline)) {
        if(g.model_cost&&g.uncovered_stage_attempt)throw rtlzz::ArithmeticError({"cost","cost_uncovered","declared earliest-placement search encountered uncovered stage graph/size and found no fully covered schedule; not predicted infeasibility",{},{}});
        if(g.model_cost)throw rtlzz::ArithmeticError({"scheduling","schedule_no_placement_found","grouped earliest placement did not find a model-budget-feasible schedule; not a global impossibility proof",{},{}});
        throw std::runtime_error("pipeline: no feasible schedule for declared groups at N="+std::to_string(g.last));
    }
    // Monotonic earliest-stage feasibility; keep the feasible upper endpoint.
    for (int i = 0; i < 64 && lower < upper; ++i) {
        double middle = lower + (upper - lower) / 2;
        if (middle == lower || middle == upper) break;
        std::vector<int> candidate;
        if (g.schedule(middle, candidate)) { upper = middle; baseline = std::move(candidate); }
        else lower = middle;
    }
    result.stages = baseline;
    result.initial_stages=baseline;
    auto before = g.evaluate(baseline);
    double allowed = before.final_delay * (1 + options.timing_relaxation_ratio);
    std::vector<double> allowed_stages;
    if(g.model_cost){
        double register_baseline=0;for(std::size_t i=1;i<before.stage_delays.size();++i)register_baseline=std::max(register_baseline,before.stage_delays[i]);
        for(std::size_t i=0;i<before.stage_delays.size();++i){double t0=i?register_baseline:before.stage_delays[0];allowed_stages.push_back(std::min(t0>0?t0*1.05:std::numeric_limits<double>::infinity(),g.provider->dataBudget(i==0)));}
        allowed=*std::max_element(allowed_stages.begin(),allowed_stages.end());
    }
    if (!std::isfinite(allowed)) throw std::runtime_error("pipeline: timing allowance overflow");
    auto current = before;
    std::size_t accepted_group_moves = 0;
    for (;;) {
        auto best = current;
        NodeId best_node = kInvalidNodeId;
        int best_stage = 0;
        for (auto u : g.unit_order) {
            auto id = g.units[u].front();
            bool fixed = false;
            for(auto member:g.units[u]) if(g.input[member] || g.output[member]) fixed=true;
            if(fixed || std::all_of(g.units[u].begin(), g.units[u].end(), [&](auto m){return g.constant[m];})) continue;
            int original = result.stages[id];
            for (int stage = 0; stage <= g.last; ++stage) {
                if (stage == original) continue;
                for(auto member:g.units[u]) result.stages[member] = stage;
                auto candidate = g.evaluate(result.stages);
                bool within=candidate.final_delay<=allowed;
                if(g.model_cost){within=within&&candidate.model_feasible;for(std::size_t i=0;i<allowed_stages.size();++i)within=within&&candidate.stage_delays[i]<=allowed_stages[i];}
                bool improvement=g.model_cost?candidate.predicted_total_area_um2<current.predicted_total_area_um2:candidate.register_bits<current.register_bits;
                if(options.arithmetic_request) result.moves.push_back({g.unit_labels[u],original,stage,false,
                    !std::isfinite(candidate.final_delay)?"dependency_or_group_violation":!within?"schedule_group_over_budget":!improvement?"no_state_improvement":"eligible",
                    candidate.final_delay,candidate.register_bits});
                if (within && improvement &&
                    (g.model_cost?(candidate.predicted_total_area_um2<best.predicted_total_area_um2 || (candidate.predicted_total_area_um2==best.predicted_total_area_um2&&candidate.final_delay<best.final_delay)):
                    (candidate.register_bits < best.register_bits ||
                     (candidate.register_bits == best.register_bits && candidate.final_delay < best.final_delay)))) {
                    best = std::move(candidate); best_node = id; best_stage = stage;
                }
            }
            for(auto member:g.units[u]) result.stages[member] = original;
        }
        if (best_node == kInvalidNodeId) break;
        const int previous_stage=result.stages[best_node];
        for(auto member:g.units[g.unit_of[best_node]]) result.stages[member] = best_stage;
        if(options.arithmetic_request) result.moves.push_back({g.unit_labels[g.unit_of[best_node]],previous_stage,best_stage,true,"accepted_state_improvement",best.final_delay,best.register_bits});
        current = std::move(best);
        ++accepted_group_moves;
    }
    result.report = current;
    result.report.baseline_delay = before.final_delay;
    result.report.allowed_delay = allowed;
    result.report.allowed_stage_delays=allowed_stages;
    result.report.baseline_register_bits = before.register_bits;
    result.report.accepted_group_moves = accepted_group_moves;
    if(options.arithmetic_request&&!g.model_cost) result.report.cost_model="fixed-op-width-v1";

    auto& rtl = result.rtl_program;
    std::vector<std::vector<NodeId>> delayed(normalized.signals.size());
    std::unordered_set<std::string> names;
    for (const auto& port : rtl.ports) names.insert(rtl_names::sanitizeIdentifier(port.name));
    for (const auto& s : rtl.signals) names.insert(rtl_names::sanitizeIdentifier(s.name));
    if (!names.insert(options.clock_name).second)
        throw std::runtime_error("pipeline: clock name collides with data port or signal: " + options.clock_name);
    // Existing emitter sanitization must not silently rename a requested clock.
    // The emitter performs its full reserved-keyword and sanitized-name check.
    for (auto id : g.order) {
        delayed[id].push_back(id);
        if (g.constant[id] || id == g.valid_input) continue;
        int end = result.stages[id];
        for (auto user : g.users[id]) end = std::max(end, result.stages[user]);
        for (int stage = result.stages[id] + 1; stage <= end; ++stage) {
            Signal reg;
            reg.id = rtl.signals.size();
            reg.name = "rtlzz_pipe_" + std::to_string(id) + "_s" + std::to_string(stage);
            while (!names.insert(reg.name).second) reg.name += "_";
            reg.type = program.signal(id).type;
            reg.debug = program.signal(id).debug;
            if (program.signal(id).driver) addDebugInfoLocs(reg.debug, program.signal(id).driver->debug);
            addDebugMessage(reg.debug, "pipeline register for node " + std::to_string(id) + " at stage " + std::to_string(stage));
            addDebugDerivedOperand(reg.debug, Operand{OperandKind::Symbol, id});
            Operation d; d.kind = OperationKind::Assign; d.type = reg.type; d.debug = reg.debug;
            Operand v; v.kind = OperandKind::Symbol; v.node = delayed[id].back(); v.type = reg.type;
            d.operands.push_back(v); reg.driver = std::move(d);
            result.registers.push_back({reg.id, v.node, stage});
            delayed[id].push_back(reg.id);
            rtl.signals.push_back(std::move(reg));
        }
    }
    // Materialize one shared valid chain after all ordinary data registers.
    // Valid always advances, including bubbles; data captures only valid sources.
    if (g.valid_input != kInvalidNodeId) {
        for (int stage = 1; stage <= options.register_stages; ++stage) {
            Signal reg; reg.id = rtl.signals.size();
            reg.name = "rtlzz_valid_s" + std::to_string(stage);
            while (!names.insert(reg.name).second) reg.name += "_";
            reg.type = program.signal(g.valid_input).type;
            reg.debug = program.signal(g.valid_input).debug;
            addDebugMessage(reg.debug, "pipeline valid at stage " + std::to_string(stage));
            Operation driver; driver.kind = OperationKind::Assign; driver.type = reg.type;
            Operand value; value.kind = OperandKind::Symbol; value.node = result.valid_nodes.back(); value.type = reg.type;
            driver.operands.push_back(value); reg.driver = std::move(driver);
            Register entry{reg.id, value.node, stage}; entry.is_valid = true;
            result.registers.push_back(std::move(entry)); result.valid_nodes.push_back(reg.id);
            rtl.signals.push_back(std::move(reg));
        }
        delayed[g.valid_input] = result.valid_nodes;
        for (auto& reg : result.registers) if (!reg.is_valid)
            reg.enable = result.valid_nodes.at(static_cast<std::size_t>(reg.stage - 1));
    }
    for (auto id : g.order) {
        auto& s = rtl.signal(id);
        if (!s.driver) continue;
        for (auto& v : s.driver->operands) {
            if (v.kind != OperandKind::Symbol || g.constant[v.node]) continue;
            auto offset = result.stages[id] - result.stages[v.node];
            if (offset < 0) throw std::runtime_error("pipeline: backwards stage dependency");
            v.node = delayed[v.node].at(static_cast<std::size_t>(offset));
        }
        addDebugMessage(s.debug, "pipeline computation stage " + std::to_string(result.stages[id]));
    }
    for (auto& r : rtl.arithmetic_regions) {
        std::set<NodeId> internal(r.members.begin(),r.members.end());
        for(auto id:r.outputs) internal.erase(id);
        for(const auto& reg:result.registers) if(internal.count(reg.source)) {
            if(!r.internal_split_enabled) throw std::runtime_error("pipeline: internal register in atomic region");
            internal.insert(reg.node);r.internal_registers.push_back(reg.node);
        }
        auto original_inputs=r.inputs;
        r.inputs.clear();
        for(const auto& v:original_inputs) {
            if(v.kind!=OperandKind::Symbol || g.constant[v.node]) {r.inputs.push_back(v);continue;}
            std::set<NodeId> seen;
            for(auto member:r.members) if(program.signal(member).driver)
                for(const auto& used:program.signal(member).driver->operands)
                    if(used.kind==OperandKind::Symbol && used.node==v.node) {
                        auto adjusted=v;
                        adjusted.node=delayed[v.node].at(static_cast<std::size_t>(result.stages[member]-result.stages[v.node]));
                        if(seen.insert(adjusted.node).second) r.inputs.push_back(adjusted);
                    }
            if(seen.empty()) r.inputs.push_back(v);
        }
    }
    if(!rtl.arithmetic_regions.empty()) beir::regions::validate(rtl);
    // Drop dead computations, preserving dense IDs and debug identities.
    for (const auto& original : program.signals)
        if (!g.live[original.id]) rtl.signal(original.id).driver.reset();
    connect_valid_output();
    initializeResetValues(result);
    if(g.model_cost){bool zero=true;for(const auto& reg:result.registers)for(auto v:reg.reset_values)zero&=v==0;for(const auto& [name,words]:result.report.output_reset_values)for(auto v:words)zero&=v==0;g.provider->validateStateProtocol(options.reset_enabled,!options.input_valid_name.empty(),zero);}
    if (options.reset_enabled) {
        if (!names.insert(options.reset_name).second) throw std::runtime_error("pipeline: reset name collides with data port, signal or clock");
        Port reset; reset.name = options.reset_name; reset.direction = PortDirection::Input; reset.type.width = 1;
        rtl.ports.push_back(reset); rtl.inputs.push_back(reset.name);
    }
    Port clock; clock.name = options.clock_name; clock.direction = PortDirection::Input; clock.type.width = 1;
    rtl.ports.push_back(clock);
    rtl.inputs.push_back(clock.name);
    if (result.registers.size() != result.report.register_count)
        throw std::runtime_error("pipeline: register accounting mismatch");
    validateProgram(result);
    return result;
}

void validateProgram(const Program& p) {
    validateOptions(p.options);
    if (p.combinational.arithmetic_regions.size() != p.rtl_program.arithmetic_regions.size())
        throw std::runtime_error("pipeline: region metadata count changed");
    if (!p.rtl_program.arithmetic_regions.empty()) {
        beir::regions::validate(p.combinational);
        beir::regions::validate(p.rtl_program);
        for(std::size_t i=0;i<p.combinational.arithmetic_regions.size();++i) {
            const auto& a=p.combinational.arithmetic_regions[i];const auto& b=p.rtl_program.arithmetic_regions[i];
            if(a.id!=b.id || a.operation!=b.operation || a.semantics!=b.semantics ||
               a.template_version!=b.template_version || a.template_parameters!=b.template_parameters || a.sharing_policy!=b.sharing_policy ||
               a.internal_split_enabled!=b.internal_split_enabled || a.split_version!=b.split_version ||
               a.members!=b.members || a.outputs!=b.outputs || a.legal_cut_groups!=b.legal_cut_groups)
                throw std::runtime_error("pipeline: region identities or membership changed");
            std::set<NodeId> internal(a.members.begin(),a.members.end());
            for(auto id:a.outputs) internal.erase(id);
            std::vector<NodeId> expected_registers;
            for(const auto& reg:p.registers) if(internal.count(reg.source)) {
                internal.insert(reg.node);expected_registers.push_back(reg.node);
            }
            if(!a.internal_registers.empty() || b.internal_registers!=expected_registers)
                throw std::runtime_error("pipeline: region internal register ownership changed");
        }
    }
    auto validate_region_inputs=[&](const std::vector<NodeId>* origins) {
        auto same_input=[&](const beir::Operand& a,const beir::Operand& b) {
            if(a.kind!=b.kind||!beir::sameType(a.type,b.type)||a.signed_view!=b.signed_view)return false;
            if(a.kind==OperandKind::Literal)return a.constant.width==b.constant.width&&a.constant.signed_view==b.constant.signed_view&&a.constant.limbs==b.constant.limbs;
            if(a.kind!=OperandKind::Symbol)return false;
            if(!origins)return a.node==b.node;
            return b.node<origins->size()&&(*origins)[b.node]==a.node;
        };
        for(std::size_t i=0;i<p.combinational.arithmetic_regions.size();++i) {
            const auto& a=p.combinational.arithmetic_regions[i];const auto& b=p.rtl_program.arithmetic_regions[i];
            if(!a.internal_split_enabled) {
                if(a.inputs.size()!=b.inputs.size())throw std::runtime_error("pipeline: atomic region input count changed");
                for(std::size_t k=0;k<a.inputs.size();++k)if(!same_input(a.inputs[k],b.inputs[k]))throw std::runtime_error("pipeline: atomic region input identity changed");
            } else {
                for(const auto& actual:b.inputs)if(std::none_of(a.inputs.begin(),a.inputs.end(),[&](const auto& original){return same_input(original,actual);}))throw std::runtime_error("pipeline: split region has an undeclared logical input");
                for(const auto& original:a.inputs)if(std::none_of(b.inputs.begin(),b.inputs.end(),[&](const auto& actual){return same_input(original,actual);}))throw std::runtime_error("pipeline: split region lost a logical input");
            }
        }
    };
    if (!p.options.register_stages) {validate_region_inputs(nullptr);return;}
    Graph g(p.combinational, p.options);
    if (p.stages.size() != p.combinational.signals.size() ||
        p.rtl_program.signals.size() != p.combinational.signals.size() + p.registers.size())
        throw std::runtime_error("pipeline: invalid materialized signal count");
    auto expected = g.evaluate(p.stages);
    if (!std::isfinite(expected.final_delay) || !std::isfinite(p.report.allowed_delay) ||
        expected.final_delay != p.report.final_delay || expected.stage_delays != p.report.stage_delays ||
        expected.combinational_cost != p.report.combinational_cost ||
        expected.final_delay > p.report.allowed_delay ||
        expected.register_bits != p.report.register_bits || p.registers.size() != expected.register_count)
        throw std::runtime_error("pipeline: invalid schedule or register report");
    if(g.model_cost){
        if(!expected.model_feasible||expected.model_state_bits!=p.report.model_state_bits||expected.model_state_signals!=p.report.model_state_signals||expected.predicted_total_area_um2!=p.report.predicted_total_area_um2||expected.predicted_comb_area_um2!=p.report.predicted_comb_area_um2||expected.predicted_state_area_um2!=p.report.predicted_state_area_um2)
            throw rtlzz::ArithmeticError({"scheduling","schedule_validation_error","model area or absolute timing budget mismatch",{},{}});
        if(!p.report.allowed_stage_delays.empty()){
            if(p.report.allowed_stage_delays.size()!=expected.stage_delays.size())throw rtlzz::ArithmeticError({"scheduling","schedule_validation_error","model path-cap shape mismatch",{},{}});
            for(std::size_t i=0;i<expected.stage_delays.size();++i)if(expected.stage_delays[i]>p.report.allowed_stage_delays[i])throw rtlzz::ArithmeticError({"scheduling","schedule_validation_error","model path exceeds frozen cap",{},{}});
        }
    }
    auto stages = p.stages;
    stages.resize(p.rtl_program.signals.size(), -1);
    std::vector<NodeId> origins(stages.size(), kInvalidNodeId);
    for (auto id : g.order) origins[id] = id;
    std::uint64_t storage = 0;
    for (const auto& reg : p.registers) {
        const auto& q = p.rtl_program.signal(reg.node);
        const auto& d = p.rtl_program.signal(reg.source);
        if (reg.node < p.combinational.signals.size() || stages[reg.node] != -1 ||
            origins[reg.source] == kInvalidNodeId || reg.stage != stages[reg.source] + 1 ||
            reg.stage > g.last || !sameType(q.type, d.type) || !q.driver ||
            q.driver->kind != OperationKind::Assign || q.driver->operands.size() != 1 ||
            q.driver->operands[0].kind != OperandKind::Symbol || q.driver->operands[0].node != reg.source)
            throw std::runtime_error("pipeline: invalid register connection");
        stages[reg.node] = reg.stage; origins[reg.node] = origins[reg.source];
        auto width = bits(q.type);
        if (storage > UINT64_MAX - width) throw std::runtime_error("pipeline: register storage overflow");
        storage += width;
    }
    validate_region_inputs(&origins);
    if (g.valid_input != kInvalidNodeId) {
        if (p.valid_nodes.size() != static_cast<std::size_t>(g.last)+1 || p.valid_nodes.front() != g.valid_input)
            throw std::runtime_error("pipeline: invalid valid chain");
        for (const auto& reg : p.registers) {
            if (reg.is_valid != (reg.node == p.valid_nodes.at(reg.stage)))
                throw std::runtime_error("pipeline: invalid valid register role");
            if (reg.is_valid) {
                if (reg.node != p.valid_nodes.at(reg.stage) || reg.source != p.valid_nodes.at(reg.stage-1) || reg.enable != kInvalidNodeId)
                    throw std::runtime_error("pipeline: invalid valid register");
            } else if (reg.enable != p.valid_nodes.at(reg.stage-1))
                throw std::runtime_error("pipeline: invalid data register enable");
        }
        const auto& output = p.rtl_program.signal(g.valid_output);
        if (!output.driver || output.driver->kind != OperationKind::Assign || output.driver->operands.size() != 1 ||
            output.driver->operands.front().kind != OperandKind::Symbol || output.driver->operands.front().node != p.valid_nodes.back())
            throw std::runtime_error("pipeline: invalid valid output");
    } else if (!p.valid_nodes.empty() || std::any_of(p.registers.begin(), p.registers.end(), [](const auto& r) { return r.is_valid || r.enable != kInvalidNodeId; }))
        throw std::runtime_error("pipeline: unexpected valid state");
    if (storage != expected.register_bits) throw std::runtime_error("pipeline: register storage mismatch");
    for (auto id : g.order) {
        const auto& before = p.combinational.signal(id);
        const auto& after = p.rtl_program.signal(id);
        if (!sameType(before.type, after.type)) throw std::runtime_error("pipeline: computation type changed");
        if (!before.driver || id == g.valid_output) continue;
        if (!after.driver || before.driver->operands.size() != after.driver->operands.size())
            throw std::runtime_error("pipeline: computation driver changed");
        for (std::size_t i = 0; i < before.driver->operands.size(); ++i) {
            const auto& v = before.driver->operands[i];
            const auto& adjusted = after.driver->operands[i];
            if (v.kind != OperandKind::Symbol || g.constant[v.node]) continue;
            p.rtl_program.signal(adjusted.node);
            if (adjusted.kind != OperandKind::Symbol || origins[adjusted.node] != v.node ||
                stages[adjusted.node] != p.stages[id] || adjusted.signed_view != v.signed_view ||
                !sameType(adjusted.type, v.type))
                throw std::runtime_error("pipeline: unaligned computation dependency");
        }
    }
}

std::string emitReport(const Program& p) {
    std::ostringstream os;
    os.precision(17);
    os << "pipeline internal register stages " << p.options.register_stages << "\n"
       << "baseline delay " << p.report.baseline_delay << "\n"
       << "allowed delay " << p.report.allowed_delay << "\n"
       << "final delay " << p.report.final_delay << "\n"
       << "baseline register bits " << p.report.baseline_register_bits << "\n"
       << "register bits " << p.report.register_bits << "\n"
       << "cost model " << p.report.cost_model << "\n"
       << "abstract combinational cost " << p.report.combinational_cost << "\n"
       << "accepted group moves " << p.report.accepted_group_moves << "\n"
       << "register count " << p.report.register_count << "\n";
    if (!p.valid_nodes.empty()) os << "valid input " << p.options.input_valid_name << " output " << p.options.output_valid_name
        << " internal registers " << p.valid_nodes.size()-1 << "\n";
    for (std::size_t i = 0; i < p.report.stage_delays.size(); ++i)
        os << "stage " << i << " delay " << p.report.stage_delays[i] << "\n";
    for (const auto &[name, values] : p.report.output_reset_values) {
        os << "output reset " << name << " [";
        for (std::size_t i=0; i<values.size(); ++i) os << (i ? "," : "") << values[i];
        os << "]\n";
    }
    return os.str();
}

std::string emitText(const Program& p) {
    std::ostringstream os;
    os << emitReport(p);
    for (std::size_t id = 0; id < p.stages.size(); ++id)
        os << "node #" << id << " stage " << p.stages[id] << "\n";
    for (const auto& r : p.registers) {
        os << "register #" << r.node << " <- #" << r.source << " stage " << r.stage;
        if (r.is_valid) os << " valid";
        if (r.enable != kInvalidNodeId) os << " enable #" << r.enable;
        os << "\n";
    }
    os << beir::emitText(p.rtl_program);
    return os.str();
}
} // namespace pred::pipeline
