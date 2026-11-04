PMUX 项目书 LaTeX 版

主文件：main.tex
图片目录：images（须与 main.tex 一起保留）

编译方法
1. 本机安装含中文宏包的 TeX Live 或 MiKTeX 后，在本目录运行：
   xelatex -interaction=nonstopmode -halt-on-error main.tex
   xelatex -interaction=nonstopmode -halt-on-error main.tex
2. 在 Overleaf 新建项目，上传本目录全部文件，设置主文件为 main.tex，
   编译器选择 XeLaTeX，然后点击 Recompile。
3. 也可使用 Tectonic（XeTeX 引擎）编译。准备文本不代表编译完成，实际结果以对应版本的渲染QA为准。

内容和模板
本版与 Word 版来自同一正文内容源，保留模板五部分及原有层级。
采用 A4、正文12磅、1.5倍行距，上下边距25.4mm、左右31.75mm。
Fandol 为可随 TeX 环境获得的中文字体。Word 使用的字体及排版引擎
可能不同，因此两个版本的换行和分页不保证完全一致。
本版是依据用户提供的 Word 模板制作，并非官方原生 LaTeX 模板。

修改方法
可直接编辑 main.tex；修改图片时保留文件名或同步修改\includegraphics。
不要把编译成功等同于全部实验或比赛要求达标，正文保留实测范围和未完成项。
