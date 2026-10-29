#include "backend/beopt.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace pred::beir;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (false)

// Hex decoding only: expected arithmetic is computed by the Python producer.
static std::vector<std::uint64_t> limbs(const std::string& text, int width) {
    std::vector<std::uint64_t> result((width+63)/64, 0);
    for (std::size_t i=0; i<text.size(); ++i) {
        char c=text[text.size()-1-i];
        unsigned digit=c>='a' ? c-'a'+10 : c-'0';
        CHECK(digit<16);
        for (unsigned bit=0; bit<4; ++bit) if ((digit>>bit)&1) {
            auto offset=i*4+bit;
            CHECK(offset<static_cast<unsigned>(width));
            result[offset/64] |= std::uint64_t{1}<<(offset%64);
        }
    }
    return result;
}

static Operand literal(const std::string& text, int width) {
    Operand out; out.kind=OperandKind::Literal; out.type.width=width;
    out.constant.width=width; out.constant.limbs=limbs(text,width); return out;
}

static Operand node(Program& p, const std::string& name, int width,
                    OperationKind kind, std::vector<Operand> args, OpCode code=OpCode::None) {
    Signal s; s.id=p.signals.size(); s.name=name; s.type.width=width;
    Operation op; op.type=s.type; op.kind=kind; op.op=code; op.operands=std::move(args);
    if (kind==OperationKind::Trunc || kind==OperationKind::ZExt) op.to_width=width;
    s.driver=std::move(op); p.signals.push_back(s);
    Operand out; out.kind=OperandKind::Symbol; out.node=s.id; out.text=name; out.type=s.type; return out;
}

static unsigned checkFacts(Program p, const std::vector<std::string>& expected) {
    MutableProgram graph(std::move(p)); graph.ensureValueFacts();
    unsigned checks=0;
    for (std::size_t i=0; i<expected.size(); ++i) {
        const auto& name=graph.program().outputs.at(i);
        const Signal* value=nullptr;
        for (const auto& s:graph.program().signals) if (s.name==name) value=&s;
        CHECK(value);
        const auto& facts=value->value;
        CHECK(facts.valid && facts.constant && facts.width==value->type.width);
        auto wanted=limbs(expected[i],value->type.width);
        CHECK(facts.value.limbs==wanted);
        CHECK(facts.known_one==wanted);
        CHECK(facts.known_zero.size()==wanted.size());
        for (int bit=0; bit<value->type.width; ++bit) {
            CHECK(((facts.known_zero[bit/64]>>(bit%64))&1) != ((wanted[bit/64]>>(bit%64))&1));
        }
        ++checks;
    }
    return checks;
}

int main(int argc, char** argv) {
    CHECK(argc==2);
    std::ifstream stream(argv[1]); CHECK(stream.good());
    unsigned rows=0, checks=0; int width=0;
    while (stream>>width) {
        std::string a,b,c,d,product,dot,carry,diff,low;
        CHECK(bool(stream>>a>>b>>c>>d>>product>>dot>>carry>>diff>>low));
        Program p; p.function_name="wide_constants";
        auto x=literal(a,width), y=literal(b,width);
        auto p0=node(p,"product",2*width,OperationKind::Binary,{x,y},OpCode::Mul);
        auto p1=node(p,"other",2*width,OperationKind::Binary,
                     {literal(c,width),literal(d,width)},OpCode::Mul);
        node(p,"dot",2*width+1,OperationKind::Binary,{p0,p1},OpCode::Add);
        node(p,"carry",width+1,OperationKind::AddCarry,{x,y,literal("1",1)});
        node(p,"diff",width,OperationKind::Binary,{x,y},OpCode::Sub);
        node(p,"low",width,OperationKind::Trunc,{p0});
        p.outputs={"product","dot","carry","diff","low"};
        const std::vector<std::string> expected={product,dot,carry,diff,low};
        checks+=checkFacts(p,expected);
        auto optimized=pred::beir::opt::optimizeProgram(p);
        checks+=checkFacts(optimized,expected);
        checks+=checkFacts(pred::beir::opt::optimizeProgram(optimized),expected);

        // Unknown lower bits remain unknown; all zero-extension padding is known.
        Program partial;
        Operand port; port.kind=OperandKind::Port; port.text="x"; port.type.width=width;
        auto input=node(partial,"x",width,OperationKind::PortRead,{port});
        node(partial,"extended",2*width+1,OperationKind::ZExt,{input});
        MutableProgram graph(std::move(partial)); graph.ensureValueFacts();
        const auto& facts=graph.program().signals.back().value;
        CHECK(!facts.constant);
        for (int bit=0; bit<2*width+1; ++bit) {
            CHECK(((facts.known_zero[bit/64]>>(bit%64))&1)==unsigned(bit>=width));
            CHECK(((facts.known_one[bit/64]>>(bit%64))&1)==0);
        }
        ++checks; ++rows;
    }
    CHECK(stream.eof() && rows==185);
    std::cout<<"WIDE FACTS PASS rows="<<rows<<" checks="<<checks<<" max_width=257\n";
}
