from book_settings import require_render, OUTPUT
import sys
PREPARE_ONLY = sys.argv[1:] == ['--prepare-only']
if not PREPARE_ONLY:require_render()
"""Rebuild the LaTeX edition from the same authored blocks as the Word edition."""
from pathlib import Path
import hashlib, importlib.util, json, re, shutil, math

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'prepared_text' / 'latex' if PREPARE_ONLY else OUTPUT / 'latex'
spec = importlib.util.spec_from_file_location('book_content', ROOT / 'content.py')
content = importlib.util.module_from_spec(spec)
spec.loader.exec_module(content)
OUT.mkdir(parents=True, exist_ok=True)
(OUT/'images').mkdir(exist_ok=True)

NUMBER = {6:'1.1',9:'1.2',12:'1.3',15:'1.4',18:'1.5',21:'1.6',
          27:'2.1',30:'2.2',35:'2.3',41:'3.1',43:'3.2',48:'3.3',53:'4.1',55:'4.2'}
ESC = {'\\':r'\textbackslash{}','&':r'\&','%':r'\%','$':r'\$','#':r'\#',
       '_':r'\_','{':r'\{','}':r'\}','~':r'\textasciitilde{}','^':r'\textasciicircum{}',
       '→':r'\ensuremath{\rightarrow}','≥':r'\ensuremath{\geq}','′':r'\ensuremath{^{\prime}}',
       '≤':r'\ensuremath{\leq}','×':r'\ensuremath{\times}','−':r'\ensuremath{-}', '∧':r'\ensuremath{\land}', '¬':r'\ensuremath{\neg}',
       'δ':r'\ensuremath{\delta}','₀':r'\ensuremath{{}_0}','₁':r'\ensuremath{{}_1}','₂':r'\ensuremath{{}_2}','₃':r'\ensuremath{{}_3}','⊕':r'\ensuremath{\oplus}','∨':r'\ensuremath{\lor}','∈':r'\ensuremath{\in}' }
def escape(s):
    return ''.join(ESC.get(c,c) for c in s)
def text(s):
    parts=re.split(r'([A-Za-z][A-Za-z0-9_]*\.(?:tcl|py|json|v|il))',s)
    out=[]
    for i,p in enumerate(parts):
        if i%2:
            e=escape(p).replace(r'\_',r'\_\allowbreak{}').replace('.',r'.\allowbreak{}')
            out.append(r'\texttt{'+e+'}')
        else:
            out.append(escape(p).replace('\n',r'\newline{}').replace(r'\_',r'\_\allowbreak{}').replace('/',r'/\allowbreak{}'))
    return ''.join(out)

def table_need(b):
    total=sum(b['widths']); line_counts=[]
    for row in [b['headers']]+b['rows']:
        cells=[]
        for val,w in zip(row,b['widths']):
            cap=max(1,(w/total*14.65*28.346-8)/10.5)
            cells.append(sum(max(1,math.ceil(sum(0.54 if ord(c)<128 else 1 for c in part)/cap)) for part in val.split('\n')))
        line_counts.append(max(cells))
    return min(620,45+sum(max(20,ln*15.75) for ln in line_counts))

def heading_need_points(i, ordinary=6):
    if i+1<len(content.blocks) and content.blocks[i+1]['kind'] in ('h','sub'):
        return min(690, heading_need_points(i+1)+55)
    if i+1<len(content.blocks) and content.blocks[i+1]['kind']=='table':
        return table_need(content.blocks[i+1])+40
    if i+2<len(content.blocks) and content.blocks[i+1]['kind']=='p' and content.blocks[i+2]['kind']=='equation':
        return 11*21.6
    return ordinary*21.6

def heading_need(i, ordinary=6):
    return f'\\Needspace{{{heading_need_points(i,ordinary):.1f}pt}}'

PREAMBLE=r'''% Generated from the shared content.py source; edit that source for synchronized editions.
% Compile with XeLaTeX (twice), or Tectonic.
\documentclass[UTF8,fontset=fandol,12pt]{ctexart}
\usepackage{graphicx,amsmath,amssymb,booktabs,longtable,array,geometry,fancyhdr,setspace,url,needspace}
\usepackage[hidelinks,unicode]{hyperref}
\geometry{a4paper,top=25.4mm,bottom=25.4mm,left=31.75mm,right=31.75mm,headheight=15pt,headsep=10mm,footskip=9mm}
\onehalfspacing
\setlength{\parindent}{2em}
\setlength{\parskip}{0pt}
\setlength{\tabcolsep}{4pt}
\renewcommand{\arraystretch}{1.25}
\setlength{\LTpre}{6pt}
\setlength{\LTpost}{6pt}
\setlength{\emergencystretch}{2em}
\clubpenalty=10000
\widowpenalty=10000
\hbadness=10000
\ctexset{section={format=\bfseries\heiti\fontsize{14}{21}\selectfont,beforeskip=16pt,afterskip=10pt},subsection={format=\bfseries\heiti\fontsize{12}{18}\selectfont,beforeskip=12pt,afterskip=6pt},subsubsection={format=\bfseries\fontsize{12}{18}\selectfont,beforeskip=10pt,afterskip=5pt}}
\pagestyle{fancy}
\fancyhf{}
\fancyfoot[C]{\fontsize{10.5}{13}\selectfont\thepage}
\renewcommand{\headrulewidth}{0.4pt}
\fancypagestyle{firstpage}{\fancyhf{}\renewcommand{\headrulewidth}{0pt}\fancyfoot[C]{\fontsize{10.5}{13}\selectfont\thepage}}
\urlstyle{same}
\begin{document}
\special{pdf:minorversion 7}
\thispagestyle{firstpage}
\begin{center}
\includegraphics[width=11.5cm]{images/contest_logo.png}
\end{center}
\vspace{8pt}
'''

pieces=[PREAMBLE]
assets=[(ROOT/'word'/'media'/'image1.png',OUT/'images'/'contest_logo.png')]
counts={}
for bi,b in enumerate(content.blocks):
    k=b['kind']; counts[k]=counts.get(k,0)+1
    if k=='title':
        pieces.append(r'\begin{center}{\heiti\bfseries\fontsize{18}{27}\selectfont '+text(b['text'])+r'\par}\end{center}'+'\n')
    elif k=='h':
        if b['source']==58:pieces.append('\\clearpage\n')
        if b['source'] in NUMBER:
            pieces.append(heading_need(bi,9 if bi+1<len(content.blocks) and content.blocks[bi+1]['kind']=='sub' else 6)+r'\subsection*{'+NUMBER[b['source']]+'  '+text(b['text'])+'}\n')
        else:
            pieces.append(heading_need(bi)+r'\section*{'+text(b['text'])+'}\n')
    elif k=='sub':
        pieces.append(heading_need(bi)+r'\subsubsection*{'+text(b['text'])+'}\n')
    elif k=='p':
        if bi+1<len(content.blocks) and content.blocks[bi+1]['kind']=='equation' and (bi==0 or content.blocks[bi-1]['kind'] not in ('h','sub')):
            pieces.append(r'\Needspace{8\baselineskip}'+'\n')
        pieces.append(text(b['text'])+'\n\n')
    elif k=='equation':
        pieces.append('\\[\n'+b['tex']+'\n\\]\n')
    elif k=='code':
        # The supplied Tcl command has explicit short continuation lines.
        pieces.append('\\Needspace{5\\baselineskip}\n{\\small\n\\noindent\\begin{minipage}{\\linewidth}\n\\begin{verbatim}\n'+b['text']+'\n\\end{verbatim}\n\\end{minipage}\n\\par}\n')
    elif k=='figure':
        assets.append((ROOT/'figures'/b['file'],OUT/'images'/b['file']))
        figfile=b['file']
        if (ROOT/'figures'/figfile.replace('.png','.pdf')).exists():
            figfile=figfile.replace('.png','.pdf')
            assets.append((ROOT/'figures'/figfile,OUT/'images'/figfile))
        width=min(float(b.get('width',14.4)),14.4)
        pieces.append('\\begin{center}\\begin{minipage}{\\textwidth}\\centering\n'+f'\\includegraphics[width={width}cm]{{images/{figfile}}}\n'+r'\par\vspace{4pt}{\fontsize{10.5}{15.75}\selectfont '+text(b['text'])+r'\par}'+'\n\\end{minipage}\\end{center}\n')
    elif k=='table':
        n=len(b['headers']); total=sum(b['widths'])
        need_pt=table_need(b)
        cols=''.join(r'>{\raggedright\arraybackslash}p{\dimexpr '+f'{w/total:.6f}'+r'\textwidth-2\tabcolsep\relax}' for w in b['widths'])
        header=' & '.join(r'\bfseries '+text(s) for s in b['headers'])+r' \\'+'\n'
        need='' if bi>0 and content.blocks[bi-1]['kind'] in ('h','sub') else f'\\Needspace{{{need_pt:.1f}pt}}\n'
        pieces.append(need+'{\\setstretch{1.0}\\fontsize{10.5}{15.75}\\selectfont\n\\begin{longtable}{'+cols+'}\n')
        pieces.append(r'\multicolumn{'+str(n)+r'}{c}{\normalsize '+text(b['text'])+r'} \\[4pt]'+'\n'+r'\toprule'+'\n'+header+r'\midrule'+'\n'+r'\endfirsthead'+'\n')
        pieces.append(r'\multicolumn{'+str(n)+r'}{c}{\normalsize '+text(b['text'])+r'（续）} \\[4pt]'+'\n'+r'\toprule'+'\n'+header+r'\midrule'+'\n'+r'\endhead'+'\n')
        pieces.append(r'\midrule\multicolumn{'+str(n)+r'}{r}{\small 续下页}\endfoot'+'\n'+r'\bottomrule\endlastfoot'+'\n')
        for row in b['rows']:
            pieces.append(' & '.join(text(s) for s in row)+r' \\'+'\n')
        pieces.append('\\end{longtable}\n}\n')
    elif k=='reference':
        pieces.append('\\begin{samepage}\n{\\setstretch{1.0}\\fontsize{10.5}{15.75}\\selectfont\n')
        r=b['text']
        if 'https://' in r:
            left,url=r.split('https://',1)
            url='https://'+url
            pieces.append(r'\noindent\hangindent=2em '+text(left)+r'\url{'+url.rstrip('.')+'}.'+'\n\\par\n')
        else:
            pieces.append(r'\noindent\hangindent=2em '+text(r)+'\n\\par\n')
        pieces.append('}\n\\end{samepage}\n')
    else: raise ValueError(k)
pieces.append('\\end{document}\n')
(OUT/'main.tex').write_text(''.join(pieces),encoding='utf-8')
bindings=[];pending_assets=[]
for src,dst in assets:
    if not src.is_file():
        if PREPARE_ONLY:pending_assets.append(str(src));continue
        raise FileNotFoundError(src)
    shutil.copyfile(src,dst)
    bindings.append({'source':str(src),'destination':str(dst.relative_to(OUT)),'sha256':hashlib.sha256(src.read_bytes()).hexdigest()})
meta={'main_file':'main.tex','prepared_text_only':PREPARE_ONLY,'pending_assets':pending_assets,'compiled_by_this_script':False,'contest':'2026 FPGA赛道选题二 PMUX结构优化','template_source':str(ROOT/'template.docx'),
      'template_sha256':hashlib.sha256((ROOT/'template.docx').read_bytes()).hexdigest(),
      'template_note':'根据用户提供的Word模板重建LaTeX排版，并非主办方发布的原生LaTeX模板。',
      'content_source':str(ROOT/'content.py'),'content_sha256':hashlib.sha256((ROOT/'content.py').read_bytes()).hexdigest(),
      'main_sha256':hashlib.sha256((OUT/'main.tex').read_bytes()).hexdigest(),'block_count':len(content.blocks),'block_types':counts,'assets':bindings}
(OUT/'latex-project.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2),encoding='utf-8')
(OUT/'README.txt').write_text('''PMUX 项目书 LaTeX 版\n\n主文件：main.tex\n图片目录：images（须与 main.tex 一起保留）\n\n编译方法\n1. 本机安装含中文宏包的 TeX Live 或 MiKTeX 后，在本目录运行：\n   xelatex -interaction=nonstopmode -halt-on-error main.tex\n   xelatex -interaction=nonstopmode -halt-on-error main.tex\n2. 在 Overleaf 新建项目，上传本目录全部文件，设置主文件为 main.tex，\n   编译器选择 XeLaTeX，然后点击 Recompile。\n3. 也可使用 Tectonic（XeTeX 引擎）编译。准备文本不代表编译完成，实际结果以对应版本的渲染QA为准。\n\n内容和模板\n本版与 Word 版来自同一正文内容源，保留模板五部分及原有层级。\n采用 A4、正文12磅、1.5倍行距，上下边距25.4mm、左右31.75mm。\nFandol 为可随 TeX 环境获得的中文字体。Word 使用的字体及排版引擎\n可能不同，因此两个版本的换行和分页不保证完全一致。\n本版是依据用户提供的 Word 模板制作，并非官方原生 LaTeX 模板。\n\n修改方法\n可直接编辑 main.tex；修改图片时保留文件名或同步修改\\includegraphics。\n不要把编译成功等同于全部实验或比赛要求达标，正文保留实测范围和未完成项。\n''',encoding='utf-8')
if PREPARE_ONLY:
    (OUT/'README.txt').write_text('工作稿文本源：尚未冻结、未编译、未发布。\n已有真实波形图按哈希复制；其余流程图和结果图待最终候选冻结后生成。\n原始工作目录内继续保留Word共同内容源content.py/content.json。\n不能把本目录称为完整可交付LaTeX项目或已通过逐页QA。\n',encoding='utf-8')
print(json.dumps({'output':str(OUT),'prepared_text_only':PREPARE_ONLY,'blocks':len(content.blocks),'counts':counts,'main_bytes':(OUT/'main.tex').stat().st_size},ensure_ascii=False))
