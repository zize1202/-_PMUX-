# v3：按 C145 展示命令测量完整 Yosys 生命周期

这次修改测量范围，没有修改优化算法。旧 `fair_measure.py`、schema 2 的 report 及 r15 等既有结果均保留。旧 r15 的“全项通过”只适用于当时包含额外 `stat` 与 JSON 导出的进程范围；不能直接写成 v3 范围已通过，也不能把两种范围之间的时间差称为算法加速。

## 依据与保留的歧义

依据用户提供的 `FPGA创新大赛问题汇总.xlsx` 的提取记录 `work/20261101_faq_revision/faq_extracted.json`、`rules_audit.json`，以及 `equivalence/original_pmux_efficiency.txt`：

- C145.2 展示 `-s` 脚本为 `read_verilog xxx.v`、`read_verilog xxx2.v`、`synth_intel -family cycloneiv -top xxx`，没有列出独立 `stat` 或导出命令。
- C87.2 为“整个yosys运行开始到结束的时间。”；C92.2 为“综合全流程从启动到结束，eqy计时不算考核内容”。原题没有另给更具体的计时脚本。
- B92 曾明确询问网表写出是否计入，C92 只回答全流程。因此 v3 是对 C145 所展示脚本范围的可复现实现，不能推断官方明确禁止所有流程计入导出；也不能把原 v2 数据说成虚假数据。

v3 对同一语言的多个文件合并成一条 `read_verilog`，之后一条 `synth_intel -family cycloneiv -top TOP`，实际计时脚本恰两条命令。混合 `.v`/`.sv` 输入目前明确拒绝，避免把原 Verilog 源静默改成 SystemVerilog 模式。没有参数覆盖，也没有插入额外 `stat`、网表写出、证明或状态初始化。`synth_intel` 自带的统计、检查以及日志输出仍属于完整进程时间。

## 执行次序与指标

`fair_measure_v3.py` 复用未修改的 v2 Windows 采集器。同一 Yosys 进程句柄的 `GetProcessTimes` 退出时刻减创建时刻作为经过时间，`GetProcessMemoryInfo.PeakWorkingSetSize` 作为主进程峰值工作集。等待 ABC 的时间自然包含在 Yosys 生命周期中，ABC 子进程内存不混入主内存字段。结束后的峰值计数读取失败则该次测量无效。

固定次序如下，全部保留原始日志：

1. 第一个用例仅原版预热 10 次，排除成绩。
2. 每例先原版、后优化版各一次。
3. 仅当初次时间比或内存比大于 1.05，且两臂初次经过时间最大值不超过 10 秒时，执行固定 5 个完整原版/优化版复核对，不提前停止。两臂分别算算术均值后取比，初次不并入均值。以上边界、是否排除首轮及内存均值的实现细节是预先公开的解释，不声称是 FAQ 唯一指定算法。
4. 全部用例、全部计时复核结束，记录 `timed_batch_finished`，才开始面积捕获。每例每臂独立重跑同样两条命令，再附一条 `write_json mapped.netlist.json`；不调用正式计时采集器，不向效率统计写入这次导出的时间/内存。

输入、Yosys 二进制、脚本、请求的 ABC 在运行前后核验散列；实际 ABC 必须由每份日志中的引用路径解析后核验真实文件散列，不能只信 `ABC` 环境变量。门库在开始、计时与捕获阶段边界及结束核验，两臂必须内容相同。后台程序和安全软件不由此脚本更改。

## 面积与计时记录如何关联

计时记录只保存 `timed_stat_inventory`，来自 `synth_intel` 自带的唯一最终 top 统计表；不含 `cells` 字段，不假称该次进程导出了 JSON。解析器要求唯一目标 top、唯一总 cells 行、各类型行不重复且数量和一致、最终检查成功和完整脚本结束。出现额外统计、导出、未知表格格式或子模块表则拒绝确认。

独立捕获保存完整 JSON，并按实际叶实例分类 LUT、标量取反、DFF、IO 等。`$not` 取反逻辑单位依赖 `Y_WIDTH`，不能从统计表里的实例数直接推断。每臂捕获 JSON 的顶层和叶类型数量必须一致，再与该臂全部计时记录（包括首轮、全部复核、第一例预热）逐类型核对；同时要求源、二进制、门库、实际 ABC、计时脚本及命令散列绑定一致。任一不符，面积证据无效，不生成该例面积结论。

这种核对证明独立运行的资源清单一致，不证明两次网表逐比特相同，更不替代 EQY。EQY 和正确性结论仍由独立流程给出。

## 新 report 接口

- `schema = 3`，`measurement_scope` 明确标记 C145 两命令生命周期；`measurement_source_sha256` 和 `v2_helper_source_sha256` 同时记录。
- `cases[].initial[]`、`rechecks[][]` 保留同次时间与峰值工作集，以及 `timed_stat_inventory`、`command_sha256`、输入/工具散列。
- `cases[].timing_aggregate` 只汇总效率。
- `cases[].untimed_area_captures.baseline|optimized.netlist` 是独立完整 JSON 的绝对路径；相对位置为 `untimed_area/<case>/<arm>/mapped.netlist.json`。
- `cases[].untimed_area_captures.<arm>.cells.netlist_sha256`、`timed_inventory_validation` 提供网表与计时清单关联。
- 验证通过后 `cases[].aggregate` 保留 `baseline_cells`、`optimized_cells`、`area_reduction` 和原有 ratio flags，另有 `area_valid`、`area_basis`；`area_state` 为 `CAPTURED_AND_INVENTORY_MATCHED`。失败则不填伪造的面积。
- `all_timing_valid` 与 `all_area_valid` 独立；二者及全程不变性检查均满足才设置 `all_measurements_valid=true`。`contest_score` 仍为空。
- `events` 保存计时结束和捕获开始次序。v2 的 `initial/<case>/<arm>/mapped.netlist.json` 路径不再有效，绑定器必须读取 v3 的显式 capture 路径。

## 本次验证

`python -X utf8 -m unittest -v test_fair_measure_v3` 的 5 项检查通过：两命令/导出前缀、严格统计解析、先完成全部计时再导出、固定五对且排除初次、不完整/变化证据拒绝、命令及实际 ABC/源/门库散列与资源不一致拒绝。

`smoke_fair_measure_v3.py` 只用第一个用例，两个标签都指向同一原版 exe，验证工具而非算法。记录在 `v3_validation/smoke_01/measurement/report.json`：10 次预热、1 个初次对、2 次独立捕获，全部有效；计时结束事件序号 24，第一次捕获序号 25。原版捕获匹配 11 次计时/预热，另一标签匹配 1 次。此开发 smoke 不算正式九例成绩。正式九例由主任务统一批次执行。

复现命令保持 v2 的命令行参数，改用 `fair_measure_v3.py` 并使用全新 `--out` 目录；`--abc` 和两臂 `--*-techlib` 在开发模式也必须提供。脚本拒绝覆盖非空输出目录。v2 与 v3 结果应分表保存，不能拼接成一个通过报告。
