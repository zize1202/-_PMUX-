PMUX 当前候选：v3复现入口（准备包，非最终发布）

先看release.json：候选、程序哈希及尚缺的当前版本材料列在其中。
旧存档中的绝对路径和报告字段原样保留；ARCHIVE_PATH_INDEX.json给出对应包内位置。
不可将旧报告改名为当前实跑。未提供当前项目书/规模证据时，pending字段不会消失。

1. 环境与校验
需要Windows、PowerShell 7、真实Python安装和OSS CAD Suite；完整证明还需POSIX shell/make/grep目录。
在现有PowerShell 7终端进入包目录，先用 $PSVersionTable.PSVersion 确认主版本至少为7，再直接运行：
  .\Run-OfficialTests.ps1 -Mode Check
所有工具、源文件和既有证据按PACKAGE_FILES.json校验。入口不改系统设置、系统PATH或ABC。
不要调用powershell.exe或Windows PowerShell 5.1，也不要执行Set-ExecutionPolicy或添加-ExecutionPolicy Bypass。若当前终端不是PowerShell 7，先打开已安装的PowerShell 7（pwsh），再按以上步骤操作。
PythonExe应指向真实可执行程序；若python.exe解析为WindowsApps下的Store别名，可显式加 -PythonExe C:\tools\Python311\python.exe（换为实际安装路径），不修改系统别名或PATH。

2. 完整九例复现
  .\Run-OfficialTests.ps1 -Mode All -SuiteRoot C:\tools\oss-cad-suite -ShellBin C:\tools\w64devkit\bin
每次创建新的results目录，默认参数、原始九例和统一ABC均保持。
顺序为：v3完整计时批次（预热/初测/必要复测）→全部计时后的独立面积JSON捕获→候选postABC捕获→直接严格EQY。
v3计时脚本只有read_verilog和synth_intel；内置统计/检查/等待ABC均计入整个进程，面积导出另行运行并逐次核对资源清单。
新完整捕获与独立面积捕获JSON逐项一致后才进入证明。没有旧r09的两段证明入口。
程序运行成功与客观门槛通过分开报告；正式结果看all_local_objective_checks_pass，不能自动推导主观满分。
退出码0表示该模式成功；2表示执行、绑定、证明或归档错误；3表示All/Measure执行完成但面积或效率等客观门槛不满足。success只表示操作完成，All还要求all_local_objective_checks_pass才能退出0。

证明路径与证据归档：
旧版迁移All在4个长名案例触发Win32路径读取错误，原失败目录和日志保留，不改写为PASS。
新版将证明放在TEMP\pmuxq\q<唯一标识>\p；可加 -ProofWorkRoot C:\pmux_tmp 指定更短的可写父目录。每次使用全新子目录，不覆盖已有证明。
计时前会按已绑定九例接口及证明分区检查未经规约的cwd+../../../partitions路径，预留8字符并要求总长小于240个UTF-16单位；同时检查包内源文件、捕获端点及外部工具路径。超长会明确失败，需缩短ProofWorkRoot或包/工具路径后重新运行。
证明runner、初始化模型和策略未因路径修复改变。所有成功或失败证明文件原样复制到results/<本次运行>/strict，strict.archive.json逐文件记录SHA和原执行目录，复制后再核验原文件及归档完全一致。
短路径原工作目录保留以供复查；入口不自动删除。原报告中的绝对路径保持原样，按strict.archive.json映射到归档。若归档失败，该次退出2，并保留短目录供恢复，不声称完整复现通过。
这一预检针对包内固定九例和已记录的分区命名；更换EQY版本引起的不同路径结构仍需审阅新日志，不能保证任意工具或任意路径可用。

3. 仅检查迁移后的单例启动与证明（不计时、不算正式成绩）
  .\Run-OfficialTests.ps1 -Mode StrictSmoke -Case test4 -SuiteRoot C:\tools\oss-cad-suite -ShellBin C:\tools\w64devkit\bin
该模式只捕获选定候选端点并证明，明确标记非官方九例成绩。
原runner中的baseline槽在此装入当前候选，并非原默认面积基线。
gold按披露的源逻辑零初态契约；候选保留实际power_up，不在候选证明侧添加zinit/setundef。
test2按Yosys read_verilog/proc综合语义解释；该证明及映射ModelSim不等于原RTL事件敏感列表仿真。
旧默认baseline的test4严格零初态反例仍属于历史事实，不等于当前候选失败。

4. 离线重建
source包含公共commit d0e71cfb7bcafe2b437f3edc1789b69e99ecd55a的离线源码ZIP、许可、全部当前intel overlay、哈希manifest和保留的build_windows_pair.ps1。
  .\Build-From-Source.ps1 -OutputRoot C:\fresh\pmux_build -CompilerRoot C:\tools\w64devkit -FlexBisonRoot C:\tools\winflexbison -BuildPythonExe C:\tools\python312\python.exe -PrepareOnly
去掉-PrepareOnly才构建。OutputRoot及同级.offline_sources目录必须全新，已有目录不覆盖。
入口确认当前PowerShell主版本至少为7，再校验完整包、安全解压离线源，并用当前PowerShell 7的pwsh启动独立子进程；其PATH/ABC设置不影响调用者。该助手不回退到Windows PowerShell 5.1，也不改变执行策略。
保留的构建脚本要求文档所列工具版本（GCC16.2.0/CMake4.4.3/Flex2.6.4/Bison3.8.2/Python3.12.14）。
构建后的二进制须重新跑完整复现，不保证不同目录/工具链重建字节哈希相同。
官方dirty补丁未公开，公开commit加本项目overlay不冒称官方未公开二进制。

5. 波形、规模与文档
ModelSim图来自真实VCD重绘，非GUI截图；有限采样不替代EQY。test4初态图只将六个隔离前导点与同时间戳末VCD值对齐；图外周期边界可能先采样后更新异步复位。
规模证据若提供，独立于官方九例均值；单个界限内实例不覆盖所有千节点工程。
打包动作本身不执行测量、证明、重建或文档渲染；已实跑的当前候选迁移证据见release.json及evidence/migration/current_all，历史r20记录单列保留。新用户复现的结果写入本次results。源码重建助手的PowerShell 7启动校验不等于已经完成新的源码编译。
