# Parameterized unsigned Mul templates and fixed C

The active implementation is `mul_split::{registry,expand}` and
`arithmetic::select`, called by PipelineV2 before grouped scheduling. It replaces
the fixed 32x32 split16 prototype and its old cost/selector fields. The ordinary
RTLzz operation-weight API remains a separate general feature.

The public request uses the unchanged versioned fields and three policies:

| Policy | Operation |
| --- | --- |
| `core-original-v1` | Atomic original Mul; the same fixed table and scheduler |
| `mul-candidates-v1` | Formal C, ordered per-original-occurrence product of original/half/quarter, filter eligibility, deduplicate, then K=16 |
| `mul-template-v1` | Diagnostic forced block_width parameter; never counted as formal C |

Automatic policies take no explicit template parameters. The diagnostic takes
exactly `{"block_width": b}`, 2<=b<=128, and rejects widths/shapes that cannot use
it. Original templates retain exact unsigned modular result semantics.
`mul-block-tree-v1` supports unsigned inputs 1..128 and result widths 1..lhs+rhs;
the required experiment domain is five W values 8/16/32/64/128, with full 2W or
low W output. Unequal inputs and other result sizes follow the typed contract;
only actually tested sizes are claimed. FP regions and their internal Mul are
not reclassified or split.

The expander slices each operand with a possibly short final block, multiplies
blocks at the sum of their widths, and uses Concat with literal zeros to express
exact shifts. Each balanced reduction level extends both inputs to
max(previous widths)+1 before AddCarry. Conservative upper intermediate widths
remain explicit until the final truncation to R. Partial products and each
complete reduction level form disjoint groups. Their allowed boundaries do not
prescribe register stages. The compiler validates product coverage, product
width, carry precision, typed references, boundaries and group DAGs before costs,
then validates the materialized graph and ownership.

The fixed provider still uses frozen fixed-op-width-v1. Parallel delay is a
longest path and comb is a unique sum; source operand widths, not product/result
widths, determine a Mul bucket. Initial placement finds a baseline under the same
group constraints; local moves strictly reduce shared state within 105/100 of
that baseline. Every candidate records real nodes, baseline/final stages and
attempted/accepted/rejected moves. Candidate score is
`(max stage delay, comb+register bits, register bits, register signals, candidate ID)`.
No target ps, model prediction or candidate PPA informs C selection.

The report retains arithmetic-structure-report-v2 JSON types and adds a selection
object. Nonselected `computation` graphs have no emitted RTL; their empty-body
hash does not claim code generation. The selected top-level report binds actual
RTL. Formal C selection and forced diagnostics have distinct capability flags.
`requireSchedule` can report an explicit indivisible-group abstract lower bound
exceeding a supplied cap; failed earliest placement remains
schedule_no_placement_found. No unproved L lower bound is claimed.

Validation covers simultaneous instances, complete/truncated products,
12/24/48/96 and tail sizes13/17, internal pipeline registers, reset/bubbles,
257-bit MAC carry, and actual group improvement. The predeclared square-comb
sensitivity reuses exactly the selected source, graphs and schedules at each
width/context; Fractions preserve bucket^2/64 without rounding. It is a
selection diagnostic, not another physical group. D inference remains step7.

The step5 closure adds `arithmetic::validateBlockTree` to the same validation
entry used by generation, scheduling and materialization. It checks ordered
original operand provenance through verified transparent extensions/aliases,
exact block coordinates/tails, unique Cartesian coverage, shift=sum(offsets),
and a reduction using each partial exactly once with zero carry and one final
low-R truncation. This is a structural certificate for mul-block-tree-v1, not
a generic formal prover. No expansion or scheduling algorithm was changed.
The test-only report reader restores actual archived scalar graphs; it requires
all recorded operation/operand/type/constant/slice/group/parameter fields.
PortRead source names are recovered from declared port mappings and the omitted
split-policy string is derived only from the registered template version;
value facts are recomputed from saved graph bytes. Missing semantic fields or
unsupported array boundary metadata fail reconstruction explicitly.
