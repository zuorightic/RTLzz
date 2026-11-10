# 条件算子函数与有限视图选择接口

本接口承接主仓库 `mission/lxy-plan-fix.md` 第一章 §0.12 第8步。第4—5步的物理采集、函数独立验收，以及第6—7步的完整阶段资格仍按原依赖执行。初版消费软件使用 TEST_ONLY 夹具；后继 `ReusedStateBoundaryProvider` 已接入真实状态和旧 S 参数，完成六点有限复用验证。当前节点函数回放仍用软件夹具，完整 D 联合价格尚未取得资格。

## 第5步文件直接进入公共加载器

`makeCostProvider(program, request)` 识别 `operator-condition-function-v1`，构造 `ConditionalOperatorCostProvider`。输入就是第5步 `tested-model.json` 的文件内容，不需要改成旧 `arithmetic-backend-model-v1` 或精确 shape 表。

请求沿用 `ArithmeticRequest`，要求 `version=arithmetic-request-v2`、`cost_mode=backend-model`、`target_platform=asap7`、`operator_boundary_policy=unsigned-op-hierarchy-v1`、明确的目标 T、L/II、平台文件及哈希、模型文件及哈希。请求校验器按条件文件的 `profile_sha256` 读取平台身份。

生产组件加载要求整体 `quality_status=isolated_functions_passed`，登记的每项操作均 `status=passed_independent`，`full_D_ready=false`；TEST_ONLY 文件或等待独立验收的候选会被公共生产入口拒绝。缺模型、哈希错误、平台或边界错误不回退固定表。`ConditionalUse::TestOnly` 是显式的软件测试调用参数，不是公共请求文件中的可选开关。

`node()` 只声明需要实际阶段上下文的算术节点；`nodeInStage()` 重建并核对 cut 身份、成员和逻辑 capture，再调用真实 `operatorQueries()`。`placementPrices()` 保留原始 d、冻结 reserve 和 A，单位分别为 ps、ps 和 µm²。`componentReport()` 报告节点覆盖和缺失的 S/完整边界资格。仅有节点函数时，普通流水线编译会给出 `cost_uncovered`，不能把独立节点资格提升为完整 D 资格。

## 第6—7步的 S/边界契约

`ConditionalBoundaryProvider::price()` 接收真实组合 BEIR、已经物化对照的 `StateCostQuery`、全部 `ConditionalBoundaryRequest`，以及当前节点函数 SHA256。返回的 `ConditionalBoundaryPrice` 必须绑定节点函数哈希、状态布局哈希、目标 T 和边界组件哈希。

每级请求含实际 `StageCostQuery`、数据启动节点、捕获节点和依赖边集合。每级价格必须逐项提供：

- 启动端到达时间 `launch_arrival_ps`，包含其应计的外部输入、clk→Q 等边界条件。
- 捕获端要求时间 `capture_required_ps`，包含 setup、时钟和预留等边界条件；允许为负，以便保留目标达不到时的真实预测违约。
- 每条级内依赖边的 `wire_delay_ps`；有据的零基线也必须显式返回零。节点函数已拥有的内部延迟不能在这里重复加收。
- `other_timing_violation_ps`，明确覆盖算术 DAG 之外的 service、control、直通或其他路径。提供者必须自行验证这些路径和协议的覆盖，不能因为算术路径满足 T 就宣告全电路可行。

`other_timing_violation_ps` 默认是未提供状态（NaN），遗漏填写会被拒绝；有据的零违约必须显式赋值为0。

非节点面积恰好分为六个互斥所有者：`input_service`、`native_state`、`external_state`、`shared_clock`、`shared_physical`、`root_repair`。算术面积由节点函数计一次；提供者不能再次返回 `operators`。状态中的 valid/控制按所属银行归入相应所有者。缺项、负面积、NaN、不匹配的依赖或端点集合均拒绝。

生产选择另外要求提供者对当前域给出 `joint_qualified=true`。S/G 适配器只有在依赖、协议、目标和完整阶段独立验证通过后才能返回这一状态。它不是模型消费者替提供者推断的资格。真实 S 后继同样保持 `joint_qualified=false`。

## 真实状态提供者与旧 S 复用

`backend/arithmetic_state_boundary.hpp` 提供 `ReusedStateBoundaryProvider(binding_path, binding_sha256, node_request, use)`。其 `price()` 重新投影真实 BEIR、placement 和协议，逐项核对物化标志、内部级数、valid 身份、布局、变量掩码、复位值、使能级和全部状态寿命，再重建完整级请求。调用方伪造面积特征、漏捕获或连线、改节点模型或 T 均会被拒绝。

三个状态所有者直接使用 `step7_general_state_v1` 冻结 S 的 grt 非负线性系数；不读取实测 FF 或 buffer 数作为预测特征。输入服务采用公共包装器见证的逐输入写使能、独立保持 valid，native 数据按前级 valid 使能，external 无条件捕获。valid/控制开销归各自状态所有者，实际共享链计一次。clock 采用训练数据的每 FF 面积上界，physical 采用三所有者预测面积与唯一算术面积之和的训练比例上界，root repair 为训练常量上界；这些 shared 项均为经验估计。

旧 S 没有独立的 clk→Q/setup 回归器，因此采用相对坐标：每级 launch arrival 为0，完整 S 边界和只在 capture required 中扣一次。这不是物理 clk→Q 为零的声明。六类固定边界取旧九精确周期上界的最大值；native enable/valid 使用旧条件回归器、原训练修正和50ps reserve，在九周期上取最大值。输入0.2T IO 预算只用于 port→input_service，已经由服务寄存器启动的算术路径不再扣输入 IO。每条级内依赖边显式计入训练数据导出的 wire 估计。

非算术检查覆盖 input data/enable/valid、native enable/valid、external valid、输出 IO，以及逐真实状态槽发现的 forwarding/bypass；后一项不依赖算术 cut 是否列出它。结果报告绑定目标 T、布局、节点模型、binding、原 S 和服务协议 SHA256，并带所有者特征、控制特征和非算术路径检查详情。

`StateBoundaryUse::RequireQualified` 为默认，当前会拒绝尚无完整算术联合资格的复用。显式 `CandidateReuse` 可返回真实参数的候选估计，仍不授予正式 D 资格。旧 S 原域最多64个变量位；实际 MAC32 存在65/66/67位状态，故后继 binding 显式登记最多67位的候选扩展，原 S 文件和系数完全不变。目标候选区间为250–3000ps，本轮只测500/1500/3000ps三点，不宣称连续区间已经验证。

六点验证使用两个新状态银行、ABC=T、synth/grt、真实保留 FF/独占所有权、实际 SDC/RC、逐路径连接与 Liberty 弧以及 CTS 前后 CEC。八类边界48/48、shared 三面积项及 wire 各6/6通过预设覆盖门限；每源三目标的状态面积中位误差最高1.595%，P95最高8.139%。两个 hold 违约和两个 setup 违约保持实测状态，模型复用通过不等于物理可行。实际算术相关状态保留、最终包装器/输出路径、G 的 wire 情境和完整联合评价仍待；不据此重训 S 或增大 G。

主仓库 `experiments/arithmetic/evidence/step6_real_boundary_v1` 保存后继证据和可搬移回放；完整六点原始产物、实际归档恢复及费用身份见其中记录。

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
