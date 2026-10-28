// Export the frozen compiler's real graph, placement and register table. This
// test does not inject a schedule, alter group boundaries or override costs.
#include "pipelinev2/PipelineV2.h"
#include "backend/simple_cost.hpp"
#include "backend/rtlgen.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace pred;
using namespace beir;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void quoted(std::ostream& out, const std::string& s) {
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32) throw std::runtime_error("unexpected JSON control character");
        else out << c;
    }
    out << '"';
}
template<class T> static void array(std::ostream& out, const std::vector<T>& v) {
    out << '[';
    for (std::size_t i=0; i<v.size(); ++i) { if(i) out << ','; out << v[i]; }
    out << ']';
}
static std::string operation(const Signal& s) {
    if(!s.driver) return "Input";
    const auto& d=*s.driver;
    switch(d.kind) {
    case OperationKind::Assign:return "Assign";
    case OperationKind::PortRead:return "PortRead";
    case OperationKind::AddCarry:return "AddCarry";
    case OperationKind::Slice:return "Slice";
    case OperationKind::ZExt:return "ZExt";
    case OperationKind::Concat:return "Concat";
    case OperationKind::Binary:if(d.op==OpCode::Mul) return "Mul";break;
    default:break;
    }
    throw std::runtime_error("unexpected operation in frozen multiplier");
}
int main(int argc, char** argv) {
    try {
        require(argc==4,"usage: mul-placement-test vullib source output");
        const std::filesystem::path out=argv[3];
        std::filesystem::create_directories(out);
        std::ifstream input(argv[2]);
        require(bool(input),"missing source");
        const std::string source((std::istreambuf_iterator<char>(input)),{});
        pipelinev2::PipelineConfig cfg;
        cfg.source_name="mul_kernel.logic.cpp"; cfg.source_text=source;
        cfg.top_function="mul_kernel"; cfg.arithmetic_candidate="mul32_split16";
        cfg.clang_args={"-std=c++20",std::string("-I")+argv[1]};
        cfg.output_kind=pipelinev2::OutputKind::Rtl; cfg.pipeline.emplace();
        auto& opt=*cfg.pipeline;
        opt.register_stages=1; opt.use_simple_cost=true;
        opt.input_valid_name="iv"; opt.output_valid_name="ov"; opt.reset_enabled=true;
        opt.input_reset_values={{"a",{0}},{"b",{0}},{"iv",{0}}};
        auto result=pipelinev2::compile(cfg);
        if (!result.ok()) throw std::runtime_error(result.error+"\n"+result.error_debug_text);
        require(bool(result.pipeline_program),"missing materialized pipeline");
        auto q=*result.pipeline_program;
        pipeline::validateProgram(q);
        const auto& p=q.combinational;
        require(p.arithmetic_regions.size()==1,"expected one region");
        const auto& r=p.arithmetic_regions.front();
        require(r.legal_cut_groups.size()==3,"expected original three groups");
        std::set<NodeId> inputs,outputs;
        NodeId iv=kInvalidNodeId;
        for(const auto& port:p.ports) {
            auto& set=port.direction==PortDirection::Input?inputs:outputs;
            set.insert(port.element_nodes.begin(),port.element_nodes.end());
            if(port.name=="iv") iv=port.element_nodes.at(0);
        }
        std::ofstream json(out/"graph.json");
        json << std::setprecision(17) << "{\"region_id\":"; quoted(json,r.id);
        json << ",\"template_version\":"; quoted(json,r.template_version);
        json << ",\"split_version\":"; quoted(json,r.split_version);
        json << ",\"sharing_policy\":"; quoted(json,r.sharing_policy);
        json << ",\"valid_input\":" << iv << ",\"N\":1,\"groups\":[";
        for(std::size_t i=0;i<r.legal_cut_groups.size();++i) {
            if(i) json << ','; array(json,r.legal_cut_groups[i]);
        }
        json << "],\"production_stages\":"; array(json,q.stages);
        json << ",\"nodes\":[";
        for(const auto& s:p.signals) {
            if(s.id) json << ',';
            bool is_input=inputs.count(s.id) || (s.driver && s.driver->kind==OperationKind::PortRead);
            auto cost=is_input?simple_cost::Cost{}:simple_cost::node(p,s.id);
            json << "{\"id\":" << s.id << ",\"name\":"; quoted(json,s.name);
            json << ",\"operation\":"; quoted(json,operation(s));
            json << ",\"width\":" << s.type.width << ",\"input\":" << (is_input?"true":"false")
                 << ",\"output\":" << (outputs.count(s.id)?"true":"false")
                 << ",\"cpp_delay\":" << cost.delay << ",\"cpp_area\":" << cost.area
                 << ",\"kind\":" << (s.driver?static_cast<int>(s.driver->kind):-1)
                 << ",\"opcode\":" << (s.driver?static_cast<int>(s.driver->op):-1)
                 << ",\"facts_valid\":" << (s.value.valid?"true":"false")
                 << ",\"known_zero\":"; array(json,s.value.known_zero);
            json << ",\"known_one\":"; array(json,s.value.known_one);
            json << ",\"operands\":[";
            if(s.driver) for(std::size_t i=0;i<s.driver->operands.size();++i) {
                if(i) json << ',';
                const auto& v=s.driver->operands[i];
                json << "{\"kind\":" << static_cast<int>(v.kind) << ",\"width\":" << v.type.width
                     << ",\"signed_view\":" << (v.signed_view?"true":"false");
                if(v.kind==OperandKind::Symbol) json << ",\"node\":" << v.node;
                if(v.kind==OperandKind::Literal) {json << ",\"limbs\":"; array(json,v.constant.limbs);}
                json << '}';
            }
            json << "]}";
        }
        json << "],\"registers\":[";
        for(std::size_t i=0;i<q.registers.size();++i) {
            if(i) json << ','; const auto& reg=q.registers[i];
            json << "{\"node\":" << reg.node << ",\"source\":" << reg.source << ",\"stage\":" << reg.stage
                 << ",\"width\":" << q.rtl_program.signal(reg.node).type.width
                 << ",\"is_valid\":" << (reg.is_valid?"true":"false") << ",\"reset\":";
            array(json,reg.reset_values); json << '}';
        }
        json << "],\"report\":{\"baseline_delay\":" << q.report.baseline_delay
             << ",\"allowed_delay\":" << q.report.allowed_delay << ",\"final_delay\":" << q.report.final_delay
             << ",\"register_bits\":" << q.report.register_bits << ",\"register_count\":" << q.report.register_count
             << ",\"accepted_group_moves\":" << q.report.accepted_group_moves << ",\"stage_delays\":";
        array(json,q.report.stage_delays); json << "}}\n";
        std::ofstream(out/"schedule.txt") << pipeline::emitText(q);
        // Exactly the same module names and external shell as the frozen pilot,
        // so the emitted byte stream can be compared before functional testing.
        q.rtl_program.function_name="mul32_split16_core";
        std::ofstream sv(out/"mul32_split16.sv"); sv << rtlgen::emitSystemVerilog(q);
        sv << "\nmodule mul32_split16(input logic clk,rstn,input logic [31:0] a,b,input logic iv,output logic [63:0] y,output logic ov);\n"
           << "logic [31:0] aq,bq;logic ivq,ovd;logic [63:0] yd;\n"
           << "mul32_split16_core core(.clk(clk),.rstn(rstn),.a(aq),.b(bq),.iv(ivq),.y(yd),.ov(ovd));\n"
           << "always_ff @(posedge clk) if(!rstn) begin aq<=0;bq<=0;ivq<=0;y<=0;ov<=0;end else begin aq<=a;bq<=b;ivq<=iv;ov<=ovd;if(ovd) y<=yd;end\nendmodule\n";
        std::cout << "PASS frozen mul graph export, production schedule validation and native RTL\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
