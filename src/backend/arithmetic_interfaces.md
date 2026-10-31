# Arithmetic compiler interfaces v1

The core request, native BEIR preflight, fixed-cost provider and reports are
shared by original and parameterized blocked Mul templates. Step5 adds real
partial products, reduction-level groups and C candidate selection. Offline
backend inference remains step7. Dedicated AES/Sigmoid/fixed-point profiles and arbitrary RTL import remain unsupported; an ordinary helper built from supported primitive nodes can still lower normally, without claiming application-level acceptance.

`rtlzz::CompileOptions::arithmetic_request` and
`pipelinev2::PipelineConfig::arithmetic_request` consume `ArithmeticRequest`.
`loadArithmeticRequest(path)` parses the same JSON used by both CLIs. Relative
profile/table/model paths resolve against the request file. Unknown fields,
unsupported semantic/candidate policies, malformed quantities, L/II mismatch and
content identities fail explicitly. Automatic policies infer widths from the typed graph. `mul-candidates-v1`
enumerates original/half/quarter configurations per occurrence, filters
eligibility, deduplicates structures and takes the first K=16 before scheduling.
`mul-template-v1` requires the single integer parameter `block_width` in 2..128
and is a forced functional diagnostic. `core-original-v1` retains atomic Mul. No request implies the general RTLzz behavior. The superseded `use_simple_cost`/`arithmetic_candidate` public fields and old
split16 implementation are removed. Historical evidence reads exact snapshots.

```json
{
  "version": "arithmetic-request-v1",
  "target_platform": "asap7",
  "platform_profile": "platforms/asap7.json",
  "platform_profile_sha256": "<SHA256 of exact file>",
  "cost_mode": "fixed",
  "fixed_cost_table": "fixed_op_width_v1.json",
  "fixed_cost_table_sha256": "d83c391e3b5435b40d9805c869b01cd9ff8cdf2dae27c173223ca4614d2c59bd",
  "candidate_policy": "core-original-v1",
  "semantic_profile": "arithmetic-semantics-v2",
  "L": 3,
  "II": 1,
  "target_period_ps": 1500,
  "template_parameters": {}
}
```

L includes the external output capture, N=L-1 is RTLzz's internal boundary count.
The standalone CLI derives N from the request unless `--latency N` was explicit;
an explicit mismatch is rejected. `vulrtlgen --arithmetic-request FILE` applies
to each declared PIPELINE kernel and checks its LATENCY. Its reset-probe and
surrounding generated control logic use their ordinary lowering path. In a
module with no PIPELINE it applies to the combinational logic unit (L=1).
A surrounding service/valid wrapper can add ticks; its observed latency is
reported separately. Request L is never silently changed to fit a graph.

```
predicate-expand --input kernel.cpp --top top --vullib VULLIB \
  --arithmetic-request request.json --arithmetic-report report.json -o kernel.sv
vulrtlgen -m Main.utb -o output --arithmetic-request request.json
```

The VUL bridge writes `.arithmetic.json` next to each generated kernel source,
including in release mode. Native and CIRCT share the request, normalized graph,
groups and schedule. CIRCT is opt-in; enabling it with an unavailable/failing tool
is an error. `VUL_REGRESSION_CIRCT=0` only skips CIRCT regression work and still
runs software/native validation.

The static registry includes `mul-original-v1`, parameterized
`mul-block-tree-v1` and the existing `softfloat3d-bitvector-mul-v1` implementation. The former wraps each eligible
unowned unsigned Mul in an atomic region and records operand billing widths and
result width. Different instances have pre-BEOPT semantic occurrence identities;
ordinary inlined helpers generate nodes, not an invented semantic registration.
The FP implementation expands the registered F32MulRNE semantic marker into its existing real
bit-vector graph. Its value/flags exit, numerical contract and atomic boundary
remain intact. Unregistered region versions and missing/overlapping group metadata
are rejected. Only declared legal groups may be split by later templates.

`arithmetic::validate` is the shared, versioned native operation/shape contract.
It runs after expansion, after selection normalization, before costs and after
materialization. It checks enum/opcode/arity, positive types/shapes, literal limbs
and high padding, references, driver/result agreement, slices/concatenation,
port coverage, registered template parameter/input mapping, region and contracted
DAGs. Missing high zero literal limbs follow existing BEIR zero-padding semantics;
extra limbs or nonzero padding fail. AddCarry is unsigned modulo its declared
result width and has a one-bit carry, without an invented universal W+1 rule.
Mux/Case branch widths follow the existing native BEIR explicit-resize semantics;
array shapes and selectors still must match. Only the actual materialized clock
and reset names may be auxiliary ports without node mappings. Existing native
emission/reset and CIRCT checks remain final defenses.

Ordered Case lowers into actual priority Ite nodes. Lookup/ArrayAccess requires
an explicit one-dimensional Aggregate and provably bounded indices; it produces
real BitSelect/Ite nodes before scheduling. The generator limit is 4096 entries
and at most 12 unproven index bits. Reachable undefined indices, opaque tables or
unsupported shapes are rejected instead of billed as a cheap black box. These
nodes retain ownership when inside an atomic region. Full dynamic numerical
contract/array support outside this declared subset remains separately scoped.

`CostProvider` consumes real nodes/groups plus `CostContext` (platform/profile,
cost mode, model identity, feature schema, evaluation stage and optional stage
index). Returned delay, combinational and per-state-bit quantities have separate
units, source, coverage/reason; floating quantities allow future physical models.
The nonvirtual group guard dispatches to an overridable estimateGroup only after checking atomic boundaries. The implemented provider is fixed-op-width-v1, bound to the exact step2 table hash.
Multiplication/division/modulo use the maximum of verified unsigned extension or
assignment provenance, otherwise conservative typed operand widths, rounded up
to the fixed bucket. Result width does not replace input billing width. Other
operators use the frozen unit/dynamic/wire categories. Parallel delay uses max
path; area sums live unique nodes. Shared nodes are billed once, and there is no
unverified Div/Mod sharing discount. Group queries cannot cut an indivisible
region/group. Initial placement, moves and final validation consume this same
provider and the existing group scheduler, with fixed 105/100 abstract cap.

The target period is request identity in fixed mode, not a physical feasibility
prediction. Backend-model requests require a model identity and reject missing,
mismatched or unavailable inference with `cost_uncovered`; no model is supplied
or trained by step4. Actual model support-domain validation, stage-dependent
queries after moves and physical scoring remain step7 acceptance requirements.

`arithmetic-structure-report-v2` records the request and cost-query identities,
registered numerical/template versions and parameters, actual nodes/ports,
regions/boundaries, cost provenance, stages, shared-register counts, and graph,
schedule and emitted RTL hashes. `graph_identity_text` is the exact JSON
serialization hashed; it must parse to `graph`. Schedule identity is SHA256 of
the graph digest concatenated with `schedule_identity_text`, which parses to
an object containing `stages` and `protocol` (clock/reset/valid and input reset values). The RTL hash covers the emitted kernel/body, not the outer VUL wrapper.
Capability states separate generated, internally cuttable, numerical validation,
C selection, D coverage/selection and physical evaluation. Compilation does not
claim independent numerical verification or a formal C structural selection.
Arithmetic-contract failures have stage/code/reason and available region/node/request identities. Other frontend/CIRCT failures retain the existing stage/error/debug path; unified experiment-row handling remains a later driver responsibility.
No diagnostic is interpreted as a proof that L is insufficient.

The repository acceptance entry is
`python3 -m experiments.arithmetic_dse.interfaces_evidence`. It replays the saved
cycle trace against an independent Python reference, recomputes fixed costs,
atomic schedules and shared register chains, and verifies inherited freezes
through explicit source snapshots. The evidence index and source bindings protect
identity; those hashes do not substitute for numerical or cycle checks. The VUL
probe has retained self-checking source/logs and exact embedded-body identity,
without claiming a saved transaction trace for independent replay. See
[the evidence scope](../../../experiments/arithmetic/evidence/step4_interfaces_v1/README.md).

Report v2 encodes `pipeline.register_bits` and `register_count` as nonnegative
JSON integers over the uint64 domain. The key-field schemas in
`configs/arithmetic_compiler_interfaces_v1.json` cover counts, costs, L/II,
stages, protocol and diagnostics. The descriptor filename/version remains v1;
its explicit report_version is v2. Current checking does not coerce strings,
floats or booleans into counters. Node identities and constant limb words remain
intentional decimal strings. Only the immutable archived v1 reader interprets
original v1 reports; no old report is rewritten. This changes serialization,
without changing cost, stage assignment or emitted computation.

`mul-block-tree-v1` slices each actual operand into blocks (including tails),
forms full block products, concatenates exact shift zeros, and adds pairs in
balanced reduction levels. Each add explicitly extends both operands to the
maximum previous width plus one. The final output has the sole declared result
truncation/extension. No lookup table of fixed 16/32/64 indices is used. A legal
group covers the parallel partial products or one complete reduction level;
the scheduler chooses levels, without a user register map. Template preflight
checks product coverage and intermediate product/carry precision in addition
to the base typed-node contract. FP regions remain indivisible.

Selection reports contain every evaluated configuration, its cost, score,
initial/final map, attempted/accepted/rejected moves and its real computation
graph. Nonselected computation graphs have no emitted RTL (their hash is the
empty-body hash); only the selected report/RTL is emitted and hash bound.
`pipeline::evaluateSchedule` and `requireSchedule` reuse the actual same group
and cost rules. A group lower bound over an explicit abstract cap reports
`schedule_group_over_budget`; earliest-strategy failure reports
`schedule_no_placement_found`, never an unproved L impossibility.
