# 条件算子函数与有限视图选择接口

本接口承接主仓库 `mission/lxy-plan-fix.md` 第一章 §0.12 第8步。第4—5步的物理采集、函数独立验收，以及第6—7步的 S/shared/完整阶段资格仍按原依赖执行。当前只完成消费软件；本轮回放的参数和 S/边界提供者均为 TEST_ONLY，不是物理验收结果。

## 第5步文件直接进入公共加载器

`makeCostProvider(program, request)` 识别 `operator-condition-function-v1`，构造 `ConditionalOperatorCostProvider`。输入就是第5步 `tested-model.json` 的文件内容，不需要改成旧 `arithmetic-backend-model-v1` 或精确 shape 表。

请求沿用 `ArithmeticRequest`，要求 `version=arithmetic-request-v2`、`cost_mode=backend-model`、`target_platform=asap7`、`operator_boundary_policy=unsigned-op-hierarchy-v1`、明确的目标 T、L/II、平台文件及哈希、模型文件及哈希。请求校验器按条件文件的 `profile_sha256` 读取平台身份。

生产组件加载要求整体 `quality_status=isolated_functions_passed`，登记的每项操作均 `status=passed_independent`，`full_D_ready=false`；TEST_ONLY 文件或等待独立验收的候选会被公共生产入口拒绝。缺模型、哈希错误、平台或边界错误不回退固定表。`ConditionalUse::TestOnly` 是显式的软件测试调用参数，不是公共请求文件中的可选开关。

`node()` 只声明需要实际阶段上下文的算术节点；`nodeInStage()` 重建并核对 cut 身份、成员和逻辑 capture，再调用真实 `operatorQueries()`。`placementPrices()` 保留原始 d、冻结 reserve 和 A，单位分别为 ps、ps 和 µm²。`componentReport()` 报告节点覆盖和缺失的 S/完整边界资格。仅有节点函数时，普通流水线编译会给出 `cost_uncovered`，不能把独立节点资格提升为完整 D 资格。

## 第6—7步需要提供的 S/边界实现

`ConditionalBoundaryProvider::price()` 接收真实组合 BEIR、已经物化对照的 `StateCostQuery`、全部 `ConditionalBoundaryRequest`，以及当前节点函数 SHA256。返回的 `ConditionalBoundaryPrice` 必须绑定节点函数哈希、状态布局哈希、目标 T 和边界组件哈希。

每级请求含实际 `StageCostQuery`、数据启动节点、捕获节点和依赖边集合。每级价格必须逐项提供：

- 启动端到达时间 `launch_arrival_ps`，包含其应计的外部输入、clk→Q 等边界条件。
- 捕获端要求时间 `capture_required_ps`，包含 setup、时钟和预留等边界条件；允许为负，以便保留目标达不到时的真实预测违约。
- 每条级内依赖边的 `wire_delay_ps`；有据的零基线也必须显式返回零。节点函数已拥有的内部延迟不能在这里重复加收。
- `other_timing_violation_ps`，明确覆盖算术 DAG 之外的 service、control、直通或其他路径。提供者必须自行验证这些路径和协议的覆盖，不能因为算术路径满足 T 就宣告全电路可行。

`other_timing_violation_ps` 默认是未提供状态（NaN），遗漏填写会被拒绝；有据的零违约必须显式赋值为0。

非节点面积恰好分为六个互斥所有者：`input_service`、`native_state`、`external_state`、`shared_clock`、`shared_physical`、`root_repair`。算术面积由节点函数计一次；提供者不能再次返回 `operators`。状态中的 valid/控制按所属银行归入相应所有者。缺项、负面积、NaN、不匹配的依赖或端点集合均拒绝。

生产选择另外要求提供者对当前域给出 `joint_qualified=true`。后续 S/G 适配器只有在依赖、协议、目标和完整阶段独立验证通过后才能返回这一状态。它不是模型消费者替提供者推断的资格。本轮只有返回 `joint_qualified=false` 的软件夹具，尚无生产 S/G 实现。

## 公共枚举、评分、选择与物化

`enumerateConditionalViews(source, structural_options, candidate_policy, assignment_limit)` 复用公共候选生成和不可分组规则，穷举声明范围内的合法 placement。固定价格只用于结构约束和物化，不进入物理评分。候选结构沿用当前 K=16 政策；赋值达到有限上限时返回不完整清单。清单哈希绑定结构、placement、候选政策和枚举状态，消费时拒绝意外增删或改变的视图。

`selectConditionalViews(inventory, structural_options, model_request, boundary_provider, use)` 对完整清单逐个调用同一节点函数和 S/边界提供者。它按串联相加、并行取最大传播到达时间，节点面积按唯一 NodeId 计一次；每个节点的冻结 reserve 加入其延迟一次，S/G 不应重复加收这一 reserve。

选择规则为：

1. 任一视图缺完整覆盖，或者枚举不完整，报告 `cost_uncovered`，不输出部分集合上的入选 RTL。
2. 有预测可行视图，选择总面积最小者；面积相同时以稳定视图 ID 打破并列。
3. 所有合格视图违约，选择最大时序违约最小者，再按面积和稳定 ID 排序，报告 `target_unmet`。

入选 placement 通过公共 `materializeSchedule()` 产生寄存器/valid/复位实现，并再次核对真实状态布局。返回对象中的 pipeline 保留其固定**结构物化**上下文；正式价格、目标、依赖、覆盖、选择身份以同次返回的独立选择报告为准，不能把结构报告中的抽象价格当成模型价格。

当前这一有限视图接口已经实现；公共请求的自动完整 D 调度尚需接入合格 S/G 提供者。没有节点或边界资格时，不会仅凭接口可调用就产生正式 D 成功记录。模型查询和视图选择不启动综合、OpenROAD 或最终 PPA，也不宣称声明候选范围之外的全局最优。

## 软件验证

主仓库 `regression/arithmetic_condition_consumer.py` 准备独立解析夹具；RTLzz `arithmetic-condition-consumer` 通过本接口重建 MAC32 的三个结构/44视图，在50、150、1500ps三档测试上下文消费节点模型和实际 S 投影。Python 从既有完整 BEIR/视图重算 Query、价格、DAG、面积和排序，随后用独立算术向量验证三份入选 RTL 的 valid/复位和数据周期。软件夹具不读取正式训练或锁定测试物理标签，所有选择报告均明确 TEST_ONLY。
