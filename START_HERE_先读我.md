# PMUX R30 使用说明

完整解压到较短的纯英文路径，例如 D:\PMUX_R30。

## 算法展示平台

双击 visualization/index.html。页面使用本地 HTML、CSS 和 JavaScript，无需安装依赖或启动服务器。
平台提供选择器恢复、优先链分组、算术资源共享三种交互，以及九例历史数据、指标切换和数据导出。
教学模型与历史测量在界面中分别标注。平台不连接原程序或 EDA 工具。

## 运行与验证

正式运行目录是 verified_core。可在终端使用真实 Python 安装进行完整性检查：

```text
D:\Python\python.exe D:\PMUX_R30\verified_core\verification\portable_v3.py --root D:\PMUX_R30\verified_core --mode Check
```

请将示例路径换成实际安装、解压路径。使用 .ps1 入口时需要 PowerShell 7；完整测量和 EQY 的依赖及参数见 verified_core/READ_ME.txt。
原版 Yosys 对中文路径存在兼容问题，程序、测试输入和临时目录宜使用纯英文短路径。

## 源码与数据

verified_core/source 包含正式构建输入及离线源码。
comment_free_source 是便于阅读的源码副本，不作为运行或构建入口；不要覆盖回 verified_core，原工程按完整文件字节校验哈希。

平台中的性能、面积和等价性记录来自工程保存的历史报告；初始状态契约、统计口径及适配范围在平台中列明。
CURRENT_FILES.json 是本包文件索引。DELIVERY_AUDIT/core_hash_comparison.json 供独立的新旧包对比测试读取。
