# ImgBurn（C 语言复刻工程）

本工程以 C 语言逐步复刻 ImgBurn 2.5.8.0 的功能和操作行为。当前已完成迁移、首轮审计，并逐步补齐镜像构建、视频布局和格式解析，尚未完成完整复刻；部分路径已完成实体测试，其余路径仍需分别验收。

## 开发约定

- 主项目名称为 `ImgBurn`，开发环境为 Visual Studio 2026，程序代码使用 C17 和 Windows 原生接口。
- 一句完整代码保持在同一行，过长时再换行；新增或修改的注释使用中文。
- 优先按原版基准补齐功能与行为。改变原版行为的缺陷修复和功能改进在复刻完成后讨论；迁移或构建必需的调整单独记录。
- 功能声明应有证据，分别记录静态审查、离线测试、真实设备操作和人工验收。

## 目录与版本控制

| 路径 | 用途 | 公开追踪 |
| --- | --- | --- |
| `src/` | 程序 C 源码及源码资源 | 是，按后缀白名单 |
| `include/` | 程序头文件 | 是，按后缀白名单 |
| `ImgBurn.sln`、`ImgBurn.vcxproj` | 可移植的 VS 2026 构建配置 | 是 |
| `.local/` | 本机迁移记录、审计、辅助脚本、测试、原始基线和参考资料 | 否 |
| `out/` | 编译输出 | 否 |
| `.vs/` | IDE 个人状态 | 否 |

`.gitignore` 使用默认忽略、显式放行的规则。测试代码、日志、设备信息、绝对路径、本机隐私资料、参考程序及光盘镜像不进入公开仓库。添加其他类型的程序资源前，应先审查来源和是否适合公开。

本工程不附带原版 ImgBurn 的二进制、解包资源或反编译参考资料。开源许可证尚待项目所有者确定。

## 构建

安装 Visual Studio 2026 的“使用 C++ 的桌面开发”工作负载、MSVC v145 和 Windows SDK，然后打开 `ImgBurn.sln`。虽然工作负载名称包含 C++，本工程所有程序源文件均按 C17 编译。

选择 `Debug` 或 `Release`，平台选择 `Win32` 或 `x64`，生成解决方案。输出为 `out/<平台>/<配置>/ImgBurn.exe`。也可在 VS 开发人员命令提示符中执行：

```bat
msbuild ImgBurn.sln /m /p:Configuration=Release /p:Platform=x64
```

直接运行 `ImgBurn.exe`，或传入 `gui`、`--gui`，可打开原生桌面主界面；运行 `ImgBurn.exe --help` 可查看现有命令行子命令。主界面与命令窗口正在逐项接通，尚未实现的菜单项保持禁用。

原版图像可作为可选本机资源：默认从 `.local/reference/extracted_resources` 读取，构建后复制到程序旁的 `resources/`，并在资源存在时嵌入主图标。也可用 MSBuild 属性 `ImgBurnResourceSource` 指定其他资源目录。公开仓库只包含加载代码，不包含这些图像、资源清单和本机路径；没有资源时仍可构建，界面使用绘制图形和文字按钮。运行时资源目录相对于程序位置解析，不依赖启动时的工作目录。现有提供素材未发现缺失，加载子集已扩至 30 张 PNG；主窗口六个模式按钮及 Write、Read 页的主要分区已按原版表单补齐布局、字体与图片状态，其他页面继续逐项对照。

主窗采用原版 Tahoma 字体及 665×385 的内容布局基准，另外保留实际状态栏高度；按钮按 DPI 缩放原图尺寸，区分普通、悬停、按下及禁用状态。Write 页左侧显示已确认的来源摘要，右侧保留介质信息，后台完成消息更新控件；卷标和应用标识只显示实际读取到的值，不猜测 UDF 版本或实现标识。原刻录按钮区分可读取介质与受支持的可写空白盘，完整格式、容量和能力检查仍由原写入流程执行。队列、图表、AWS、速度设置、多份刻录、Book Type 和装载/弹出等尚未接通的控件保持禁用。手动 L0、成品视频候选及 CD 追加窗口集中在“工具 → Write扩展”。本轮的隐藏原生窗口及离屏绘制验证不代表当前远程桌面逐像素验收。

Read 页按原版来源/目标区、介质信息和数据/音频双读取速度区布局，保留扇区查看、光盘信息、目标浏览和文件夹入口。分卷、CD-TEXT、原始 MDS、会话首 INDEX 00、软件重试及成功后弹盘位于“工具 → 读取选项”，点“应用”保存，取消保留旧值；参数继续由原后台读取流程执行。两个速度框为保留已有自定义倍率而继续可编辑，与原版下拉列表样式的差异单独保留。目标总容量和可用空间在后台只读查询，失败/取消清为未知，换路径或换页不接收旧结果，可通过重新选择同路径或刷新重试；这些值是查询时快照，实际读取仍做原有空间检查。Read 来源的卷标、实现标识及文件系统尚无可靠盘内返回值，显示未知，不借用 Write 镜像信息；完成后加入刻录队列、批量读取、图表和托盘按钮仍禁用。Read 大动作图和设备图的原版运行时赋值、目标双击/编辑态及可见桌面像素效果仍待确认。

API 调用者可显式设置 `BurnConfig.keepCurrentWriteSpeed=true`，CLI 可用 `--speed KEEP` 沿用设备当前写入速度，此时不发送速度设置命令；默认false保留原有请求语义，`writeSpeedKbps` 的0、0xFFFF或0xFFFFFFFF仍表示请求最高速度。现有GUI、CLI及Discovery的默认配置保持原行为，GUI尚无保留当前速度的独立选择。

单层 DVD+R 的既有写入核心已完成一张实体盘验证：生产 Build 生成的 ISO9660/Joliet/UDF1.02 数据镜像经写入、轨道关闭与封盘，内置校验和另一次独立只读校验均完整一致；两份回采均通过独立文件系统提取，Windows UDF 挂载的全部文件也逐个匹配。本次使用受限验收入口调用生产管线，沿用设备当前速度，没有发送 OPC、托盘移动或速度设置命令；必要的卷锁和防取出保护成对恢复。GUI 实际刻录点击、其他尺寸与来源布局、其他驱动器及 DVD-R/DL 的成功实体验收仍须分批完成。

单层 BD-R SRM 已完成一次固定数据源的实写、封盘及独立只读恢复验证。第50阶段预留接口返回1117后，第51阶段新回采确认已有空预留轨；显式不再预留，完整写入15936扇区并成功同步、关闭轨道和封盘。光驱此时改报BD-ROM，原调用因旧Profile判断返回失败，记录保留；第51阶段据实际响应修正该判断，随后用最终源码仅作完整只读回采，全SHA同源，三命名空间各8文件及Windows UDF全部文件匹配。没有重写或换盘，仍使用已计账的同一张BD。该结果限本次设备、来源及生产执行核心，GUI点击、更多来源/尺寸/设备和多层BD仍待分别验收。

BD物理信息可通过 `bd-info` 查询，并已加入GUI现有介质信息文字：分别显示当前逻辑Profile、PIC确认的物理BD类型、层数、类别/版本及介质ID。封盘后报告BD-ROM的BD-R不会因此被误称为压制盘；可读容量和空白轨1空余分别标明，不把已记录长度当成整盘名义容量，也不补造卷标或文件系统信息。

```bat
ImgBurn.exe bd-info R:
```

该命令及GUI后台介质查询采用passive打开，不管理MCN、不锁盘、不设置速度或托盘。传统`info`只追加BD摘要，保留原设备/厂商查询与打开行为，不能把它当作上述受限入口。`mmc_read_bd_disc_structure`目前解释物理BDR格式1/3、Class1/Version2 BDR三层/四层格式4/5，以及Class0/Version2物理BDW格式1/2/3的SL/DL公共信息，核对完整4100字节响应、DI固定112字节单元及跨单元一致性；末尾EB数据不当作DI填充。BDXL只解释共同头和固定介质ID区域，size/channel保留原始码，不解写策略或推算容量。BDO、continuation、混合/未知布局或未支持的ID编码明确为未知/不支持，不影响既有ISO Read/Verify资格。介质ID不是唯一盘序号；双层、BDXL及BD-RE摘要目前只有模型证据，最近的实盘信息验收为第52阶段已封盘单层BD-R。BD-R写能力核对设备声明的Class0/Version1或Class1/Version2支持位，不再把任意BD-R类别位非零视为本路径可用。

BD-RE摘要按[JIS X6232:2017](https://kikakurui.com/x6/X6232-2017-01.html)分别解释BDW专用层标记和介质ID，兼容逻辑Profile0040/0043，不能把只读Profile0040当作压制BD-ROM。size/channel保留原始码，不据此补造名义容量或宣称全部物理参数合规；ID缺失明确为未知。此摘要本身不修改介质；BD-RE格式化、覆写和清零使用下述明确选择的独立流程。

BD-RE信息另追加可选的格式容量列表，按[原版格式容量说明](https://forum.imgburn.com/topic/19688-imgburn-bd-r-defect-management/)保留设备返回的DT/FT、块数、原始TDP及列表顺序。`mmc_read_format_capacities`用单次23h读取最多31个描述符；未知、重复或非递增FT也原样报告，不挑选格式。BD-RE的当前/最大描述符中，DT1的TDP是最大备用区簇数，DT2是已分配备用区簇数，不能当作扇区字节数相乘；DT3合称无介质或容量未知，不据此覆盖就绪状态或把驱动容量当盘容量。列表与原READ CAPACITY/轨1空余分开；附加查询失败显示未知，保留原命令退出值和GUI读写资格，不发送FORMAT UNIT或改动备用区。

BD-RE已接通明确确认后的格式化、数据镜像覆写和清零。格式化先只读列出设备返回的FT30/31原索引，再明确选择认证0（快速重格式化）/1（无认证）/2（完整认证）/3（快速认证）；锁后重新核对整份选择和能力，只提交一次FORMAT UNIT，随后等待设备并检查实际RFC/READ CAPACITY容量与备用区。格式化不等于清零，命令已开始后的取消只记录请求并等待；传输失败可能已经改盘，不自动重试。FT30按实际容量至少满足请求核对，FT31采用设备实际结果，不把被忽略的请求块数当最终容量。

```text
ImgBurn.exe bdre-format <drive> --list
ImgBurn.exe bdre-format <drive> --index N --certification 0..3 --confirm-erase
ImgBurn.exe bdre-write <drive> <image> --overwrite [--verify]
ImgBurn.exe bdre-erase <drive> --quick|--full --confirm-erase
```

覆写仅接受已格式化、可随机写的BD-RE和2048字节来源；普通`burn`默认范围保持，通过新命令/API显式`bdReOverwrite`或GUI本次覆盖确认才进入。写入从LBA0覆盖来源范围，其外旧数据可能保留；使用正常WRITE10（原版BD-RE FastWrite默认关闭），不把仅适用于BD-R的VNR位套到BD-RE。沿用设备速度，不发送预留、关闭轨道或封盘，写后同步并复核格式/容量，可回读比较。GUI有单独的格式化窗口，无默认描述符/认证；确认只属于当前任务，换盘、刷新和任务结束清除候选与选择。

快速清零按原版说明处理前800扇区（更小用户区则到其末尾），完整清零覆盖全部逻辑用户区，均同步并逐扇区回读零值；只针对已格式化BD-RE，不自动格式化，不承诺清除备用区或重映射旧内容。清零取消在命令边界停止，已发出的写入不能撤销；它与FORMAT UNIT的取消等待语义分开。所有新流程已通过模拟设备/完整数据核对，尚无BD-RE实盘验收。

BD-R新增明确选择的带备用区格式化入口，仅接受声明为新的空白SRM盘。先保留设备FT32列表原索引，再选择具体描述符；核心在锁前后核对空白轨、能力与整份容量列表，只提交一次原版FT32/FST1请求，不自动回退FT00、重试或写入数据。接受命令后等待设备结束；取消只记录请求，超时也不代表物理操作已停止。完成后以实际空白轨用户块、缺陷管理状态和备用区块数核对，不套用BD-RE的READ CAPACITY或RFC DT2要求。已格式化、已有数据、预留轨、POW或其它不满足新盘条件的介质不进入本入口。

```text
ImgBurn.exe bdr-format <drive> --list
ImgBurn.exe bdr-format <drive> --index N --confirm-format
ImgBurn.exe burn <drive> <image> --bd-write-type INCREMENTAL --bd-vnr-off --verify
```

Write可显式选择原版Incremental或SAO的MODE页参数；依据MMC，BD介质可忽略该页，不能据此推断物理写法、免预留、追加或留盘开放。未选择时保留既有入口行为，`--bd-no-reserve`仍是独立选项。`--bd-vnr-off`关闭BD-R的VNR并使用普通WRITE10；这不自动建立备用区，没有备用区也不承诺缺陷替换。GUI“本次BD-R写入 / DVD双层L0”窗口仅把选项交给本次写入，任务创建或上下文变化即清除；BD-R格式化有独立菜单，无默认索引及BD-RE认证选项。上述路径已有离线模型和完整数据核对，尚无新的实盘及可见GUI交互验收。

## 当前功能状态

程序目前覆盖设备访问、MMC、缓冲区、镜像构建、视频布局、单元预览、UDF、音频、镜像解析、桌面主界面和部分厂商控制。四种构建配置已验证可以生成，但构建成功不代表功能与原版一致。

| 功能范围 | 当前实现和缺口 |
| --- | --- |
| Read | GUI/命令行支持 DVD/Blu-ray 的 2048 字节数据 ISO，以及经检查的单会话、单轨 MODE1 CD ISO。新增单会话音频、混合轨道、MODE1/MODE2 CD 的 2352 字节 CUE/BIN 读取，使用 Q 子通道核对轨道、索引与地址，支持符合已检查边界的扩展预间隙和 SCMS 周期识别。自动提取 Q 的盘级 MCN 与逐音轨 ISRC；可显式读取 CD-TEXT，保留原始 CDT 并在 CUE 中引用。支持有界重试、取消、完整传输检查及目标保护。受检查的单轨 MODE1 CD、单层及已结束的单会话双层 DVD、已识别的单层及受支持常规双层 BD-R/BD-RE 可读取为单文件或分卷 MDS 数据配套，保留 DVD 的 OTP/PTP 和物理层位置；精简 MODE1/MODE2 的 2352 字节 MDS（含分卷）和按 Full TOC 分会话导出 CUE/BIN 已接通，新增目标文件系统 Auto 分卷、完整容量预设/自定义、数据/音频速度、可调重试和成功后弹盘；文件系统有效长度选择等其余设置仍待实现，更多原版子通道兼容边界待对照；负 LBA 主通道音频和任意 P–W 保留不在已实现范围，实体介质验收尚未完成 |
| Build | 已实现 ISO/Joliet 双字节序路径表、目录扇区填充、名称冲突处理、多 extent 大文件及 UDF 1.02/2.50 文件树。UDF 使用 64 位文件长度和 AED 分配表续接，2.50 使用 EFE、元数据分区与副本。支持纯 ISO、ISO/Joliet、纯 UDF 或桥接镜像；保留输入一致性和目标文件保护 |
| DVD-Video / Blu-ray 布局 | DVD 使用 ISO+UDF 1.02，补建 AUDIO_TS、过滤 VIDEO_TS、按视频文件顺序排列，并保证 IFO 末尾与 BUP 开头至少间隔 32 KiB。在输出中修正 IFO/BUP 与标题集地址；标题导航交叉检查 PGC、PTT、Cell 地址、VOBU 表和 NAV 包，支持从实际布局选取 OTP 层切换单元、加入填充、更新 SPLIP 并生成 MDS。Blu-ray 使用纯 UDF 2.50、EFE、元数据副本、64 KiB 文件对齐、分离的主备描述区及 BDMV/BDAV 目录补齐。输入需要已经制作好的视频目录；成品 DVD-Video ISO/BIN/数据 MDS 的只读换层候选分析已接通 `dvd-layers`，保留镜像真实 LBA；Write 视频 ISO 自动选层、单元预览的完整原版对照及介质验收仍待完成 |
| Write | GUI/命令行已接通空白 CD-R MODE1 SAO、DVD-R（含双层）DAO、DVD+R（含双层）和 BD-R SRM 的单会话单轨 2048 字节 ISO/BIN/数据 MDS 写入（含分卷及首卷偏移），并支持 BINARY/MOTOROLA、原生 PCM WAVE 与可解码音频、MODE1/2352、XA MODE2/2352 及混合轨 CUE 的单会话多轨 SAO 写入、索引及间隙。支持 CD-TEXT 的导入区写入，使用 CUE 文字或优先保留的原始 CDT。检查介质和容量，按介质发送布局、设置速度、预留轨道并收尾；BD-R 根据当前能力选择 VNR 或普通写入。CD-R、DVD-R 支持设备能力允许的测试写入；命令行/API 可显式请求 OPC。支持 CD-RW、单层 DVD-RW 快速/完整擦除、取消、保护恢复和写后回读比较。Write 页可显式后台查询成品 DVD-Video 单元候选并手工选填 L0，不自动评分或选最佳单元。DVD+R DL 可由 OTP MDS、无层信息的数据来源或显式 L0 设置层容量；DVD-R DL 使用介质固定 L0，均按层分块并回读。缺失解码器的音频、非 XA MODE2、仅首层双层写入、视频 ISO 的自动单元候选、其他介质格式化及完整设置仍待实现。另有原版以外的实验 CD-R MODE1 TAO 数据会话 API，可显式建立首会话、追加、留盘开放或封盘并完整回读；增加独立纯 ISO9660 地址重定位、只读规划、专用 `append-iso` 命令及 Write 页追加扩展窗口；默认规划、明确选择后才允许写入，不合并旧树，不改变原刻录按钮或 `burn`，独立 ISO 执行核心已完成两会话实体闭环，GUI 点击流程及更多设备仍待验收；多数原有介质路径仍未实体验收 |
| Verify | GUI/命令行支持 2048 字节数据光盘可读性检查和 ISO/BIN/单轨数据 MDS 比较，新增单会话音频、混合轨道、MODE1/MODE2 CD 可读性检查及受支持的原始 CUE/BIN 比较。音频按原版默认仅检查可读性，可显式开启样本比较。无参考镜像时可按 Full TOC 检查完整多会话的轨道数据区，跳过会话间隙。支持重试、取消、完整传输检查、失配计数及结束布局复查；支持多会话 CUE 在各会话轨道数据区比较及精简 MODE1/MODE2 的 2352 字节 MDS 完整主通道比较；完整选项及真实介质流程尚未验收，其他音频格式取决于可用解码器 |
| Discovery | GUI/命令行已接通内存全零数据的写入与回读比较，可选择全部空余扇区或自定义扇区数，支持速度、取消及设备测试写入。当前介质范围为空白 CD-R、单层 DVD-R/+R 和 BD-R SRM；BD 的全容量测试不预留固定长度轨道。可擦写介质、双层、超刻、AWS、图表及完整选项仍待补齐，实体刻录未验收 |
| 桌面界面、设置、任务队列 | 已有快速向导、六种页面、文件/查看/模式/工具/帮助菜单及 Build 的输入/输出菜单。已接通来源选择、镜像计算和构建、日志、CUE/MDS 生成窗口、镜像信息和只读扇区查看器；可选加载原版主图标、向导及部分按钮图像。完整工具窗口、方案保存载入、设置、队列、图表和高级布局编辑仍待补齐 |
| 音频、CUE、MDS、层切换点 | CUE 支持 UTF-8、UTF-16 LE/BE、本机 ANSI、CR/LF/CRLF 换行、BINARY/MOTOROLA、混合扇区尺寸、INDEX 00–99、FLAGS、TITLE/PERFORMER/SONGWRITER/COMPOSER/ARRANGER/MESSAGE、DISCID、UPC、CDTEXTFILE 和多会话；下一会话前要求 LEAD-OUT，导出器保留该声明。WAVE、AIFF、MP3 等音频按解码后的实际 PCM 计数。MDS 支持 1.x 多会话 CD、DVD、多轨道、非零数据偏移及分卷引用，提供 OTP/PTP DVD 描述生成接口，PTP 可保留 L1 大于 L0 的布局。CD 多会话 CUE 一次写完、完整格式兼容和刻录集成仍待完成 |
| 厂商控制与换盘设备 | 有部分命令封装；参数覆盖及真实硬件行为未验收 |
| 原版命令行接口 | 当前使用子命令，尚不兼容原版 `/MODE` 等接口 |

功能范围以 [ImgBurn 官方说明](https://www.imgburn.com/) 和原版随附的命令行说明为参考。当前属于具有部分桌面操作流程的核心原型，不能认定为完整复刻或可交付的刻录软件。

本机审计记录、原始基线、复现脚本及结果保存在被忽略的 `.local/` 下，不属于公开发布内容。实验 CD-R TAO 数据会话已在一张 CD-R 上完成首会话、两次追加及封盘后的三份载荷回读验证，其范围与多数仍只经文件/协议验证的原有刻录路径分开记录；尚未完成所有介质及原版逐项人工验收。

数据 MDS 工作流命令示例：

```bat
ImgBurn.exe read R: disc.mds
ImgBurn.exe verify R: disc.mds
ImgBurn.exe burn R: disc.mds --verify
```

Write/Verify 共用逻辑数据读取接口，支持单会话、单轨、LBA 0 起始、标准数据控制标志的 2048 字节 DVD 或 MODE1 CD MDS。按描述中的首卷偏移和有效轨道长度跨卷读取，不将文件前缀、末尾多余字节或 MDS 描述本身当作光盘数据；全部引用文件在操作期间保持只读打开。可使用已解析的相对路径和 `*.mdf` 等引用。原始音频、交错子通道、多轨和多会话 MDS 不进入数据 MDS 应用流程；多轨、多会话 CD 使用 CUE，原版 MDS 应用同样限定单会话单轨。独立 Verify 可比较双层 MDS 的逻辑数据，不检查实体层位置；Write 支持下述 DVD+R DL 路径。

数据MDS写入BD-R SRM或显式BD-RE覆写时，来源DVD的层数、OTP/PTP和层提示仅作来源描述；完整逻辑数据仍按原偏移与分卷连续写入并回读，不据此设置BD目标层界。来源OTP和PTP保留原版DWORD提示，包括零端点和下溢回绕；这些值不恢复成物理BD容量。PTP核对两层起点、路径标记及低24位跨度之和与完整逻辑长度的模24投影，使大容量描述不因地址高位省略而被拒绝；公开层提示与此校验分开，端点为0时提示仍为0。文件、轨道和双结构范围继续检查。DVD/CD目标保留严格资格，对双层来源要求L0非零且小于数据总长、OTP两层比例和对齐有效，指定其它目标L0也不能绕过。BD读取为ISO已支持；已识别的单层、下述受支持常规双层BD-R/BD-RE及BD-R XL三层/四层可读取为单文件或分卷MDS，封盘后逻辑BD-ROM与物理BD-R分开核对。未知物理格式、BDO及超出支持边界的常规双层来源仍拒绝MDS，普通ISO读取不增加这些限制。

DVD+R DL 支持单轨 2048 字节 ISO/BIN、单层数据 MDS 和受支持的 OTP 双层 MDS，含首偏移与分卷。默认优先保留 MDS 的层位置；普通数据来源没有层信息时，按 `ceil(总扇区数 / 32) × 16` 选择完整 ECC 块平衡两层；已固定介质没有源层位置时沿用实际 L0。CLI `--layer-break <sectors>` 或 GUI“工具 → Write扩展 → 双层 L0 扇区（留空自动）”可显式指定非零、16 扇区对齐的 L0，并覆盖 MDS 层值；窗口需点“应用”保存，取消保留原值和已有候选选择，优先级依据[原版作者说明](https://forum.imgburn.com/topic/2566-where-is-my-layer-break-sector/)。

DVD+R DL 要求空白、单会话、单轨介质和有效双层写入能力，查询最大/已固定 L0，在锁定后复查，再用 SEND DISC STRUCTURE 格式 20h 设置并回读确认；已固定且相同的位置不重复设置。按层边界拆分传输块，同步缓存、关闭轨道后按 [MMC-5](https://www.13thmonkey.org/documentation/SCSI/mmc5r04.pdf) 的功能 5 完成最小半径收尾，结束和校验前重新装载后均复查层容量，比较完整源数据。

DVD-R DL Sequential（0015h）新增固定层 DAO，要求有效的 002Fh/RDL 能力；用 READ DISC STRUCTURE 格式 20h 取得介质固定 L0，按固定位置分块，预留整轨并由 DAO 同步结束，不向它发送 DVD+R DL 的设置层容量或功能 5/6 收尾。支持能力允许的测试写入、速度、取消、保护恢复及写后比较，校验前仍复查介质和固定层位置。来源 MDS 的 L0 与本次固定目标位置分开处理，显式用户值必须与固定位置相同；[原版作者也区分 +R DL 可选范围与 -R DL 固定位置](https://forum.imgburn.com/topic/6895-creating-double-layer-isos-with-imgburn/)。Layer Jump（0016h）没有随 DAO 开放。

本轮双层路径要求两层实际都有数据、L0 非零且 16 对齐，并满足介质容量和 L0 不短于 L1。仅写首层、PTP/未对齐的双层 MDS、追加会话和更多格式化仍不支持；手动值不绕过来源格式边界。普通数据平衡并非 DVD-Video 单元候选算法，视频可继续使用 Build 生成的受支持 OTP MDS/已知单元位置；成品视频 ISO 的只读单元候选已由 `dvd-layers` 提供，Write 自动选择仍待接通；成品镜像分析不插入填充或重排源镜像。上述结果为文件及协议验证，未执行实体双层刻录。

```bat
ImgBurn.exe burn R: data.iso --verify
ImgBurn.exe burn R: data.iso --layer-break 1913760 --verify
ImgBurn.exe burn R: video.mds --verify
```

CD-R 追加会话现有 `mmc_plan_cd_mode1_append` 只读规划 API，输入已解析的盘信息、下一不可见轨、Full TOC、同一状态下的当前写参数页及 2048 字节来源扇区数。要求已有完整会话、CD-R 仍可追加且最后会话为空，盘信息/轨信息/Full TOC 的会话和真实轨号一致；只在当前 TAO 写类型、标准 MODE1/2048 参数和有效非负 NWA 能确认数据起点时，返回下一会话/轨号、原末会话 A2、实际 NWA、数据起止 LBA（终点不含）、用户数据预算及扣除来源后的余量。函数本身不访问设备、不发送命令、不选择写模式；由下述实验 ISO 的只读规划与显式执行入口复用，原空白盘检查与写入路径保持不变。追加独立 ISO 的数据范围不等于合并或导入旧文件树。

地址依据为 [MMC-5 Revision 4](https://www.13thmonkey.org/documentation/SCSI/mmc5r04.pdf) 的 6.22.3.1/Tables 376–378、6.26.3.4/Table 488、6.27.3/Tables 499、502–509 及 7.5.4/Tables 663、665、667。Full TOC 只包含完整会话，空新会话须由盘和不可见轨信息确认；不从 0 地址或固定会话间隔推算 NWA，也不使用盘信息中 CD 实为 MSF 的 lead-in/lead-out 字段冒充 LBA 容量。Free Blocks 仅作为当前用户数据块预算，规划不生成导入区、预间隙或结束策略，也不承诺完整会话收尾空间。

SAO 的 NWA 在 Table 509 脚注 5 中指向首轨导入区后的第一块，不能直接当作数据 INDEX 01 或固定加 150；本 API 明确拒绝 SAO，停在尚未确认的布局边界。另有独立的实验执行 API `pipeline_write_cd_mode1_session`，仅使用已经明确为 TAO 的 MODE1/2048 参数快照，不通过切换 SAO 来推算地址。音频/混合、包写入、部分录制或已预留轨、CD-RW、DVD/BD 追加和多会话文件系统合并均不在范围。独立纯 ISO9660 的地址重定位另由下述文件视图及实验 ISO API 提供；SAO 追加与导入旧文件树仍未实现，TAO 原始数据扩展的实体证据不向这些能力外推。

追加写入按用户明确选择作为原版以外的扩展开发。原作者说明，原版会最终关闭写出的光盘，支持从 CUE 把多个会话一次写完，不能留盘开放后再追加；两者不能混称为同一个复刻缺口，见[原作者说明](https://forum.imgburn.com/topic/23030-burning-multiple-images-in-burner-queue/)。

实验 API `pipeline_write_cd_mode1_session` 输入至少 300 个 2048 字节扇区的 BIN 原始载荷，借用已有设备句柄；`allowBlank` 显式允许建立首会话，`finalizeDisc` 明确选择本次关闭后是否允许下一会话。要求 CD-R、当前已为 TAO/MODE1、有效 NWA 及欠载保护，锁定后复查盘/轨/参数/Full TOC，用实际 NWA 写入；不解释或重定位 ISO 文件系统，不提供 CUE/音频/混合、MDS 或 DVD/BD 追加。函数只选择会话开放/封盘参数，不执行 OPC、速度设置或托盘操作；关闭轨道/会话后核对完整旧 TOC 并逐字节回读本次载荷，正常结束恢复原写参数。部分写入或关闭失败保留失败状态，不猜测重写或收尾；调用者须检查返回值以及结果中的 writeStarted/sessionClosed/verified，避免失败后重复写盘。封盘后光驱可能改报 CD-ROM；独立只读 API `pipeline_verify_cd_mode1_session` 可据已保存的数据范围重新验证，避免为重验重新发写入命令。现有 `burn`/Write 页仍沿用原空白盘 SAO 路径，未自动调用此实验 API。

第四十一阶段实体只读基线对照原版确认 CD-R/可写容量与 NWA；修正 ATIP 误将 CD-R 响应中非零的 CD-RW 子类型位视为不支持，同时保留可擦写/受限介质拒绝。现场还发现奇数 Full TOC 分配长度在该光驱上少传一字节，读取改为偶数字节分配并仍要求全部声明字节完整；不把短响应本身当作有效 TOC。测试载荷不含文件系统，因此实体样本是原始数据验证盘，不是可在资源管理器中浏览的文件盘。

独立 ISO 的只读地址视图由 `imgburn_iso_session_open/set_start/read/close` 提供，输入为从 0 寻址的完整 2048 字节 ISO。源文件保持只读打开，按目标数据起始 LBA 转换 PVD/Joliet SVD 的路径表位置、根记录、L/M 及可选路径表和全部目录记录的 extent，支持多 extent 文件及目录/路径表跨扇区；文件内容、名称、长度和卷块数保持原值。文件尾部已有的完整扇区 padding 原样保留，容量按来源实际扇区数计算。重新设置基址始终基于源地址，溢出不改变旧视图；输出不插入旧会话前缀，是映射到目标 LBA 的会话载荷，不能直接按普通独立 ISO 挂载，也不引用或合并旧文件树。

实验 `pipeline_write_cd_iso_session` 与 `pipeline_verify_cd_iso_session` 消费同一视图，在设备查询前检查来源，并在已有 TAO 规划及写前复查后绑定实际 NWA；原 BIN API 继续按原始字节处理。ISO 执行入口要求至少 300 个实际来源扇区且仅接受纯 ISO9660，已通过专用 `append-iso` 命令及 Write 页追加扩展窗口接通，原刻录按钮与 `burn` 保持原行为；纯 ISO9660 的执行核心已通过下述两会话实体写入、回读与 Windows 文件访问验证，GUI 点击流程仍待独立验收。文件视图可重定位 ISO+Joliet，但 MODE1 执行入口在任何设备命令前拒绝 Joliet：[ECMA-119 第5版](https://ecma-international.org/wp-content/uploads/ECMA-119_5th_edition_december_2024.pdf) 的资料性附录 C 忠实重印 1995 年 Joliet 规则，其中 C.5.1.5 对多会话 Joliet CD-ROM 有 MODE1 限制；这不是普通 ISO9660 主体的一般禁令，也不是现代 Windows 接收行为的实测结论。地址字段与数量的区别另据 [ECMA-119 第2版](https://ecma-international.org/wp-content/uploads/ECMA-119_2nd_edition_december_1987.pdf) 及 [libisofs 多会话实现](https://dev.lovelyhq.com/libburnia/libisofs/raw/branch/master/libisofs/ecma119.c) 核对。

专用 `append-iso <drive> <file.iso> [--write] [--allow-blank] [--finalize]` 明确属于追加扩展，默认只读查询并显示会话/轨号、旧 lead-out、实际 NWA、数据范围、来源扇区数及容量余量。`--allow-blank` 显式允许建立首会话；`--write` 才执行且始终完整回读，默认留盘开放，`--finalize` 选择本次封盘。`--allow-blank`/`--finalize` 单独出现仍仅规划；未知/重复参数和不支持来源在打开设备前拒绝。只读规划复用 `pipeline_plan_cd_iso_session`，与执行共用 TAO/BUFE 能力、布局与容量约定，执行仍复查实际 NWA。专用入口使用不修改 MCN、也不预先锁卷的被动打开方式；Ctrl+C 转交取消事件，进入执行 API 后显示 writeStarted/sessionClosed/verified，不在失败后自动重试或猜测收尾。原设备打开、Read、GUI 和 `burn` 流程保留原行为。

```bat
ImgBurn.exe append-iso Q: session-1.iso --allow-blank
ImgBurn.exe append-iso Q: session-1.iso --allow-blank --write
ImgBurn.exe append-iso Q: session-2.iso --write --finalize
```

第一条只读规划；第二、三条为显式实体写入用法。本轮实体测试经受限的所选计划验收入口调用同一执行核心，未通过普通 CLI 或 GUI 点击触发。必须使用符合已验证子集的纯 ISO9660 来源，现有已封盘测试 CD 不会因 `--allow-blank` 获得追加能力。

Write 模式下“工具 → Write扩展 → CD-R 独立 ISO 追加（扩展）...”打开独立窗口，默认不查询或写入；“只读规划”在后台读取并列出全部 13 项布局值。“使用此计划”只确认当前计划，再点击“写入并回读”才进入执行。空白首会话和本次封盘均由窗口显式选择，默认为不允许空白盘、留盘开放。来源在规划和选择期间保持只读锁；来源、驱动器、选项、刷新、关闭/取消或离开 Write 页会清除派生计划与写入选择，Read 控件和已有 DVD L0 状态按原规则保留。写入授权仅消费一次，完成或失败后须重新规划选择；`pipeline_write_cd_iso_session_planned` 在初次和最终 NWA 复查时核对所选计划的全部 13 字段，布局变化停止，不把旧计划套到新布局。窗口更新、状态和进度沿用现有后台消息模型，线程不访问控件。原版没有后续追加能力，此窗口是明确标记的扩展，不作为原版窗口外观基准。

当前 ISO 来源子集不包含 UDF-only/ISO+UDF 桥接、引导/El Torito、Rock Ridge/SUSP 等未知目录扩展、XAR、交错、多卷集、已重定位的会话载荷或原始 2352 字节 BIN。项目 Build 默认含 UDF 桥接，不能直接作为本入口的受支持来源；应明确选择纯 ISO9660。合法但未支持的格式与截断、双端序冲突或地址越界分别返回不支持或读取/结构错误。纯 ISO9660 执行核心的实体结果见下段；文件视图与协议覆盖不因此扩大为其他格式或全部设备的兼容声明。

独立 ISO 追加已用一张 CD-R 完成单台光驱实体闭环：两个不同的纯 ISO9660 各408扇区，先建立开放首会话并完整回读，通过后在实测非零 NWA 11810追加第二会话并封盘。生产所选计划API复查全部13字段；两次写出和封盘后的两段回采都与独立重定位参考逐字节相同，8个源文件SHA匹配，第一会话原始字节未改变。Windows在用户重新装载同盘后识别第二会话，4个文件逐SHA通过；这是独立新文件树，旧树没有合并。试验使用被动打开和受限命令审计，未发送OPC、托盘、速度或擦除命令，第二步仅设置并恢复必要的会话结束参数。另一光驱只读预检时欠载保护未启用，按既有门槛停止，没有为了通过测试修改它。此结果不代表普通刻录/GUI点击、其他光驱、其他格式或全部剩余写入路径已实体验收。

Read 选择 `.mds` 目标时，支持受检查的单会话单轨 MODE1/2048 CD、经物理格式查询确认的单层 DVD，以及已结束的单会话双层 DVD。CD 使用原版可载入的精简描述，记录标准 150 扇区预间隙元数据，数据文件仍从 LBA 0 起，不把负地址间隙保存为主通道数据。双层从 READ DISC STRUCTURE 格式 00h 的 PFI 取得 OTP/PTP 和实际层位置：OTP 换算反向编号的第二层地址，PTP 分别查询两层，核对它们的总长与 READ CAPACITY。读取结束前再次核对布局，再发布保留该层位置的 MDS；PTP 可保留 L1 长于 L0 的来源，未对齐的原层位置也按原值保存。该 Read 入口不套用制作新刻录镜像的对齐和容量限制，但输出并不因此获得当前 Write 尚未支持的布局能力。现有 Write 可接续符合其边界的 OTP MDS；独立 Verify 比较完整逻辑数据。

BD-R/BD-RE 的 MDS 使用原版共享生成器的虚拟 DVD-R 形式：版本 1.3、单逻辑数据轨、完整 2048 字节扇区长度和 Unicode 文件列表。单层保留原有结构；常规双层新增虚拟 OTP 及两份相同的 4100 字节结构。虚拟 PFI 地址按原版保留低 24 位，完整数据长度另存于逻辑字段。双层从已确认的 DI 首末 PAA 计算首层物理 DataZone 跨度，用作虚拟描述的 L0；它包含物理备用区，不是已证明的用户 LBA 换层点，逻辑数据仍连续读取，也不据此设置 BD 刻录目标层界。

常规双层 MDS 当前支持普通 120mm、channel1 的 Class0/Version1 BDR 格式1/3及 Class0/Version2 BDW 格式1/2/3，逐个核对首层 DI 的地址一致性。总逻辑扇区数 `N` 和虚拟首层跨度 `L0` 须满足 `ceil(N/2) <= L0 <= 0xFD0000`，允许 `N <= L0` 的部分录制来源，实际只保存完整的 `N` 个扇区。读取前后核对逻辑 Profile、物理摘要及首末 PAA，变化时不发布 MDS。独立的来源解析入口保留更广的原版OTP提示，旧严格MDS布局API的DVD接受域不变；CLI按编码层数区分DVD/DL，GUI另列描述层数，不由提示是否为零猜层数。原版已实际载入部分盘、相等边界、不足半程、零端点及回绕提示的本地合成MDS；零端点时原版不显示LB，下溢时显示无符号DWORD值。这是文件载入对照，尚无实体双层 Read 验收。

已识别的 Class1/Version2 BDR XL三层/四层格式4/5使用独立的Read虚拟缓存路径，复刻原版在4100字节响应中扫描36个固定112字节槽、最后第0层DI命中覆盖前值的规则，包含EB区域中的命中。原始槽24/28字节及标签按原版DWORD算式保存，不改成XL标准物理地址偏移，也不声称是真实首层容量。虚拟提示与OTP位分开保存；提示为零且OTP开启时保留原版11h单结构。读前后同时核对摘要、选中槽、标签、原始字和虚拟提示。普通SL/DL物理布局API保持原接受域，不因这个Read入口而放宽。单文件、显式分卷及Auto共用完整逻辑流，`N <= INT32_MAX`；BDO、BD-RE XL及未知布局仍待补齐，普通BD ISO不增加这些查询。XL目前只有原版静态合同和模拟设备验证，尚无实体XL读取验收。

“工具 → 生成MDS文件”已接通大容量来源，按所列顺序描述2048字节整扇区文件，逻辑总长为1至INT32_MAX扇区。提供原版的DVD-ROM、DVD-R、DVD-RW、DVD+R、DVD+RW五种描述类型，默认DVD-ROM；这些名称用于MDS元数据，BD大小的数据同样可以保存，不据文件大小猜物理介质。可选PTP/OTP换层提示，显式非零值须16对齐且小于总扇区数，输入框支持完整32位数值，不再截为七位；生成时不套用新刻录DVD的容量和两层比例限制。直接添加数据文件而未设置换层、或显式覆盖为0时，保存单层描述并清除OTP位；描述输入的继承规则见下文。原有Read和DVD刻录生成接口各自保留。

Tools默认使用原版UTF16文件列表及每卷独立扇区数，也可在另存为类型中选择ANSI。默认将输出目录下的子目录保存为相对路径，其他来源保存基本文件名；与输出文件同主干的来源改用对应扩展名通配。勾选“保留完整路径”时保存完整路径并跳过通配，适合将MDS另存到其他目录。来源解析现支持低24位地址回绕的大PTP描述，以及同主干无扩展名来源生成的裸“*”引用；后者以描述文件所在目录和去扩展名的基本名定位，父目录中的点不作为扩展名。五种已识别类型均可作为受支持布局的数据来源，文件引用和完整逻辑长度由后续读取核对；生成成功不会放宽Write的介质与层布局资格，旧严格布局解析入口仍保留既有接受域。非整扇区来源、其他描述格式展开及自动加入队列仍待接通。原版已实际生成40GiB的DVD-ROM单层及DVD-R/OTP样本；PTP回绕和裸引用目前依原版静态合同和离线模型验证，尚无本轮原版可见载入或实体BD验收。

MDS另存为类型按原版提供ANSI、Unicode和所有文件；本工程每次默认Unicode，ANSI或所有文件类型保存为ANSI名称。本次选择交给raw及 .dvd 后台生成，旧API默认Unicode；编码偏好跨次保存仍属于完整设置待办。ANSI使用系统代码页及原版名称长度规则：路径和通配处理后先完整转换，再仅写UTF16码元数加1个字节。因此多字节名称可能被截断或没有终止零，生成成功不保证能重新打开；优先使用Unicode可保留这些名称。转换结果比原版请求还短的情况在发布目标前返回不支持，不复制未知缓冲区尾字节。此ANSI行为依据原版静态链和本地文件验证，本轮没有新增原版ANSI可见取样。

Tools新增单独选择 .dvd 文本描述，按列表顺序展开完整数据卷，可引用BD大小的数据；这不是DVD实盘测试。支持带BOM的UTF-8、UTF-16 LE/BE及无BOM的系统ANSI，逐行去除首尾控制空白，实际文件优先于元信息。未勾选换层时继承 LayerBreak 的有效正int64低DWORD值，显式0清除、显式非零按16对齐覆盖；OTP/PTP使用本次窗口选择。继承值可以不16对齐，但生成时仍须小于完整逻辑N；生成前再次核对各卷长度，文件不可用、长度不符或输出失败时保留旧目标。

.dvd 输入限单独一个描述、最多512个非空2048字节整卷、逻辑N不超过INT32_MAX，文本不超过4MiB。简单文件名按描述目录定位，带反斜杠目录的相对路径按原版使用当前工作目录，完整路径按自身定位；不自动剥引号；缺失文件按下述有界规则尝试替换或追加后缀。正斜杠和驱动相对路径等未闭合组合暂不支持。自动恢复失败后，未知带等号元信息忽略，零值、负值及无效层数字保留先前提示；坏Unicode按本项目边界拒绝。该 .dvd 入口不扩充普通DataSource格式，混合选择或多个描述暂不支持；依据为原版静态合同及离线验证，可见窗口和实体多层验收仍分开记录。

.dvd 自动恢复保留原版的顺序敏感门槛：只有去空行后的总行数为1，或等于此前已处理元信息行数加1时，才在直接文件查找失败后尝试候选。依次尝试描述基本名换成记录扩展名、记录基本名按 .ISO/.IMG/.000/.001 替换扩展名、给完整记录基本名追加 .iso/.img/.000/.001。例如 image.dvd 记录 old.bin 时，先试 image.bin，再试 old.iso 等，最后试 old.bin.iso 等；扩展名大小写沿原版风格，第二组跳过与原名不分大小写相同的重复项。所有候选仍在原记录目录，记录无目录时才用描述目录，不跨目录泛搜。首个现存非目录项即选中，若它为空、非整扇区或超出项目范围，后续会失败，不改选其它候选。恢复早于等号识别，因此末尾元信息行在门槛成立时也可能先被识别为数据；交互查找、DI及PDI非整边界等其余描述布局和更多路径组合仍待接通。

Tools新增 .lst 文件列表输入：物理文本须在10字节至1MiB之间，第一原始行与 IMAGELIST 完整比较、不区分大小写，不先去空白；支持BOM/系统ANSI。识别后删除首行，后续行去首尾控制空白并删空行，每行按原顺序作为一个完整2048字节数据卷，不解析LayerBreak、注释或嵌套描述，继承提示为0。普通相对文件名和相对子目录均按列表所在目录定位，绝对路径保持；只有一个数据项找不到时，可用完整列表路径换成该项扩展名尝试一次恢复，跳过列表自身，保留原版大小写风格。正斜杠、驱动相对和单反斜杠根相对路径等未闭合组合暂不支持。

选择的首项是已识别LST时，只展开首项列表，按原版忽略其余选择；非首项LST不展开。未识别的头或尺寸回到普通整文件生成流程，保留原有二进制 .lst 数据用法；识别头之后的坏正文、缺卷及超范围错误不会回退把描述当数据。列表仍限512卷、逻辑N不超过INT32_MAX，不改变普通DataSource的格式入口；单独API失败时整体清零。本轮接通的是文件列表与后台生成，可见窗口及实体多层验收仍分别记录。

Tools新增GI的外置数据卷表输入（类型88888888h），按表中顺序展开完整2048字节数据卷，继承层提示为0。原版先剥除记录中的反斜杠和冒号目录，仅用文件名在GI所在目录定位，不按记录的原目录或当前工作目录寻找；不Trim文件名，空槽或空尾名跳过，重复卷保留。单项缺失时按跳过空项后的实际卷数，只尝试一次GI同主干换成该项扩展名的恢复，跳过GI自身。头部单字节计数最多255槽，逻辑N不超过INT32_MAX，剩余文件名含正斜杠的组合暂不支持。

GI按原版先读取176字节识别，成功后才读取至多1MiB解析列表；两次字段位置选择分别按各自读取长度计算。首项GI识别成功只展开它，忽略其余选择；未识别的头或类型才回整组raw，识别后的坏表、未终止名称及缺卷不回退。内嵌数据类型22222222h使用下述单卷受检入口，其它尚未闭合的数据偏移布局仍返回不支持。该入口只用于Tools，普通DataSource与刻录资格不因GI支持而扩大；原版依据、离线文件验证与可见窗口、实体多层验收分别记录。

Tools接通单卷内嵌GI和受检PDI（1至512卷）：GI数据起点156字节，PDI仅首卷跳过304字节，后续卷从文件起点读；MDS引用原始文件并保存首卷字节偏移，轨道及footer只计载荷。PDI按原版两次读取18字节，首14字节识别TTAFMPVOBCDIMG，卷数取头内16位值，本项目最多512卷。后卷名是在完整首卷路径后直接附至少两位十进制序号，如 image.pdi01、image.pdi02、image.pdi100，不换扩展名、不插点，也不使用 image.pdi00。首项PDI识别后只使用其卷列表，未识别才回整组raw，已识别但损坏或不满足资格时不回退。

带头入口要求首卷去头后非空整2048、至少17扇区，并在头后第16逻辑扇区包含CD001标识；先行扇区候选存在识别冲突则返回不支持。PDI后卷须为非空整2048原始数据，顺序拼接且不再去头，总逻辑N不超过INT32_MAX；非整卷需要跨卷carry/补齐，仍不支持。该标识只确认本批数据起点，不等于完整ISO9660合规或原版全部自动扇区识别。生成前重新核各卷物理长度和总载荷长度，失败保留旧目标。已明确布局的Tools输入API也支持只作用于首卷的字节偏移及后续整卷；零偏移仍保留原完整数据卷路径。普通DataSource不新增直接GI/PDI入口，可使用生成的MDS载入数据。

不分卷时先写新的同名 ISO，若已存在则使用 `.001.iso` 等新文件名；完整读取后才发布引用它的 MDS。`--replace` 仅授权替换 MDS 描述，已有 ISO 和分卷始终保留。取消、失败或结束时层布局、BD 摘要变化均删除本次新建文件并保留旧描述。GUI 和现有 CLI `read` 命令直接使用此流程；普通 ISO 输出不依赖新增的 PFI/PIC 查询。Read 已接通下述文件系统 Auto 分卷及所需同名 MDS 发布；完整布局文件 Auto/Yes/No 策略、按文件系统有效长度裁剪、物理/保护元数据保存及更多原版设备兼容仍待补齐。上述结果为文件和协议验证，尚未在实体双层介质验收。

Read 的 `--split <MiB>` 为每卷最大容量（按输出扇区的 2048 或 2352 字节向下对齐），0 或省略表示不分卷；非零分卷只用于 MDS 目标。超过一卷时按 `.i00`、`.i01` 等顺序保存，末卷仅含剩余完整扇区，不产生空尾卷；只有一卷时仍使用 ISO。任意卷重名都会改用 `.001.i00` 等另一整组文件名。程序先预留整组新文件，完成全部数据和 Flush/Close 后才发布按顺序列出所有分卷的 MDS；失败或取消时只清除本次创建的文件。最多支持 512 卷，Write/Verify 共用现有跨卷数据源。GUI Read 页提供 Auto、不分卷、650/700/780/810/870/890 MiB、1/2/4/4.37/4.4/4.45/4.5/4.55/4.6/4.65/4.7/7.95/23.3/46.6 GiB，以及可直接输入的自定义 MiB；手动大小用于 MDS，Auto 也可用于数据 ISO。单位为二进制容量，小数 GiB 先换算字节，再按实际输出扇区向下对齐。Auto 查询目标本地或网络卷的文件系统，FAT 系列每卷不超过 4 GiB 减 1 字节，NTFS/exFAT 不额外分卷；普通数据 ISO 确需分卷时改为发布同名 MDS 与 I00/I01，不产生一个不完整 ISO。这个转换继续执行各介质的 MDS 资格检查；已识别的单层、受支持常规双层及BD-R XL三层/四层使用上述虚拟DVD-R描述，未知物理格式及超出支持范围的来源返回不支持。显式 4 GiB 是容量预设，需文件系统适配时选择 Auto。分卷工作流依据[原版设置指南](https://forum.imgburn.com/topic/6232-the-imgburn-settings/)；名称使用原版的 I00/I01 形式，已有输出的保护和换组策略沿用本工程边界。

```bat
ImgBurn.exe read R: disc.mds --split 2048
ImgBurn.exe verify R: disc.mds
ImgBurn.exe burn R: disc.mds --verify
```

Read 页保留数据和音频读取倍率；“工具 → 读取选项”提供0–20次软件重试（默认20）、成功后弹盘及原有分卷/格式选项。CLI 对应 `--speed <Nx|MAX>`、`--audio-speed <Nx|MAX>`、`--retries <0..20>`、`--eject`；音频速度用于原始 CD，按已知会话首轨与后续轨道类型切换。CD/DVD 使用 SET CD SPEED，Blu-ray 使用 32 位 SET STREAMING；未显式指定的 API/CLI 速度保留当前设备设置。只有数据和描述完整发布后才弹盘，失败或取消不弹；若弹盘本身失败，已发布镜像仍保留并明确报告。GUI 切换模式保留本轮输入，完整设置文件持久化仍待实现。

```bat
ImgBurn.exe read R: disc.iso --split auto --speed MAX --retries 20 --eject
ImgBurn.exe read R: disc.mds --raw-cd --split auto --speed 16x --audio-speed 4x
```

原始数据 CD 可显式使用 `read R: disc.mds --raw-cd`，GUI Read 页选择 MDS 后，在“工具 → 读取选项”勾选“原始数据 CD（2352 字节 MDS）”。读取通过 Q 核对单会话、单轨、LBA 0 起始的 MODE1 或 MODE2 主通道，完整保留 2352 字节，按实测模式发布精简 AA/AB 描述。可与 `--split <MiB>` 合用，单卷使用 BIN，多卷使用 I00/I01 顺序文件列表，CLI/GUI 按 2352 字节向下对齐卷容量；重名换整组，失败、取消或最终 TOC 变化均清除本次新建文件。Verify 按首卷偏移及文件列表比较全部主通道，包括扇区头、子头和校验区，独立 Read 不重建这些内容；XA Form1/Form2 和非 XA 的原始 MODE2 均按字节保存，不套用 Write 的 XA 归一化。默认 MDS 仍保存 2048 字节数据，raw MDS 不保存 CD-TEXT、交错子通道或额外索引。原版已载入有效 XA Form1、Form2 以及实际 MODE1 分卷输出；很小首卷的 API 边界样本触发原版文件系统探测 EOF，未算载入通过。音频、多轨、多会话 CD 由 CUE 承接，不据元数据解析能力开放原版不支持的 MDS 应用。

多会话 CD 的 CUE Read 默认使用 Full TOC 的各会话首个 INDEX 01 到 A2 lead-out，每会话保存独立 BIN，Q 核对轨道、模式、索引和地址。默认导出接通首个 11,400 扇区、其后 6,900 扇区的会话间隔。显式使用 `--session-pregaps`，或在 GUI Read 模式的“工具 → 读取选项”勾选“包含多会话首轨 INDEX 00”，可先由实际 Q 确认后续会话首 INDEX 00 的起点，再保存完整首00至 A2；支持由更长首00形成的非默认 INDEX01 间隔。探测不计完成进度，读失败不被当作起点。原版每会话仍自动生成 150 帧暂停，故 BIN 将这前150帧原样保留为未引用前缀，CUE 的 INDEX 00 只引用其后的存储部分；恰150帧时省略零长 INDEX00。不能用已确认的 CUE 规则表示的布局明确返回不支持，不用自定义 LEAD-IN 或猜测 PREGAP 填差。会话间其余 lead-in/out 主通道载荷和负 LBA 首轨音频未读取。发布前重解析核对会话、轨道与 A2，结束复查完整 TOC 和介质类型。

Verify 对照 CUE 与 Full TOC 的会话、轨道起点、类型和 A2；默认范围仍为各会话 INDEX01 至 A2。显式 `--session-pregaps` 或 GUI Verify 页的同名选项会用 Q 确认首00边界，额外比较镜像实际声明的会话首 INDEX00。自动150帧只检查可读性，不猜测任意源文件前缀，也不假定其内容为零；音频默认只检查可读性，`--compare-audio` 显式比较样本。进度与结果分别统计实际比较和仅可读扇区，探测请求单独处理。上述仍为文件及协议验证范围，实体多会话介质尚未验收。

```bat
ImgBurn.exe read R: disc.cue --session-pregaps
ImgBurn.exe verify R: disc.cue --session-pregaps --compare-audio
ImgBurn.exe verify R: --readable --session-pregaps
```

Discovery 命令示例（会向所选光盘写入数据）：

```bat
ImgBurn.exe discovery R:
ImgBurn.exe discovery R: --sectors 2295000 --speed 4x
ImgBurn.exe discovery R: --sectors 2295000 --test
ImgBurn.exe discovery R: --no-verify
```

按[原版作者说明](https://forum.imgburn.com/topic/4319-how-to-use-discovery-mode/)，Discovery 从内存连续生成 2048 字节扇区的全零用户数据，不建立临时 ISO 或文件系统。默认写入全部空余扇区并在重新装载后回读比较；`--sectors` 指定自定义数量，`--speed` 接受 MAX 或倍率，未指定时为 MAX。GUI 提供全部/自定义、容量复制、MiB 换算、速度、测试模式和校验选项。测试模式仅用于设备支持的 CD-R、DVD-R，跳过回读校验；`--no-verify` 关闭比较，`--no-cycle` 关闭校验前的托盘重载，`--opc` 显式请求功率校准。非测试模式会实际消耗空白光盘。当前不支持超刻、重写盘、双层或完整原版高级设置，命令行仍使用本工程子命令接口。

Read 数据镜像命令示例：

```bat
ImgBurn.exe read R: disc.iso
ImgBurn.exe read R: disc.iso --retries 20 --replace
ImgBurn.exe read R: disc.mds
ImgBurn.exe read R: disc.cue
ImgBurn.exe read R: disc.cue --cdtext
```

默认执行 20 次单扇区额外软件重试，不修改光驱的硬件重试设置。多扇区读取失败后尝试逐扇区恢复，无法恢复时返回失败；不会用零填充冒充完整镜像。`--retries` 接受 0–20，`--replace` 明确允许在成功完成后替换已有目标。GUI 也可选择来源光驱、目标 ISO 并执行读取或取消；命令行连接控制台时可用 Ctrl+C 请求取消。镜像先写入同目录的临时文件，完整读取、冲刷及最终容量检查通过后再发布；取消或失败时尝试移除本次临时输出并保留原有目标。驱动器访问仍取决于系统权限及设备对 SPTI 的支持。

CD 的 ISO 路径只接受能够以 2048 字节用户数据保存的单轨 MODE1 布局，依据 [TOC](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddcdrm/ns-ntddcdrm-_cdrom_toc) 与[会话信息](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddcdrm/ns-ntddcdrm-_cdrom_toc_session_data)核对轨道和会话，再检查原始扇区头。音频、混合轨道及 MODE2 可选择 `.cue` 原始读取；当前仅支持单会话 CD，要求驱动器返回完整主通道及格式化 Q 数据。

原始读取保留 LBA 0 到 lead-out 之前的全部 2352 字节主通道，CUE 记录轨道模式、FLAGS、INDEX 00–99 及结束地址。Q 用于检查 CRC、轨道和绝对/相对地址，不写入 BIN；若非位置 Q 遮住不能确定的索引边界，会明确失败。不会推测缺失索引或填零。先完成新 BIN，最后发布 CUE；已有 BIN 不覆盖，有同名文件时依次使用 `.001.bin` 等文件名。明确替换 CUE 后，旧 BIN 仍保留；失败或取消时尝试删除本次创建的文件。该文件保护策略尚未完成与原版输出命名行为的逐项对照。

CUE/BIN Read 默认通过 READ SUB-CHANNEL 查询盘级 MCN 和音轨 ISRC，同时从 READ CD 的 ADR2/ADR3 扫描获取编号；双路径依据[原版作者说明](https://forum.imgburn.com/topic/11942-bad-or-no-isrc-and-mcn-codes-from-cd-audio-discs/)和官方更新记录。有效编号分别导出为 CATALOG 和 ISRC，与 CD-TEXT 的 UPC/文字 ISRC 独立。ADR3 自身没有轨号，只有前后验证的位置 Q 同属一轨时才采纳，不按 TOC 起点猜测 INDEX00 中的归属。无有效值、全零编号或设备明确不支持不抹除另一来源已取得的值；坏可选编号仅忽略该样本，CLI、GUI 日志和状态回调给出提示。传输失败、坏 Q CRC、有效编号冲突或前后有效查询不一致会结束读取并保护旧输出。当前未完整复刻异常驱动器的全部兼容策略，也未保存任意 P–W 子通道。

扩展预间隙读取允许新数据轨 INDEX00 的前段保留旧轨控制和数据结构，核对最后 150 帧的新结构、实际切换位置及 INDEX01；无法确定的边界仍拒绝。SCMS 根据位置 Q 的每四帧翻转周期识别，要求充分且唯一的相位证据，再导出 SCMS 标志；普通 DCP 保持不变。非周期拷贝位变化、证据不足或其他控制位错误不推测为 SCMS。已有普通短预间隙仍可原始读取，写入时另按合规布局检查。

CD-TEXT 读取使用 READ TOC 格式 5，与逐扇区 Q 检查分开。命令行 `--cdtext` 只接受 CUE，GUI 在 CUE 目标下提供“读取 CD-TEXT”开关；默认关闭。检查响应长度、pack CRC、序号、block、计数及 size information，再于读取结束前比较原始 pack。驱动器不支持该请求、传输不完整、数据非法或前后变化均返回失败，不忽略显式请求。光盘没有文字时正常生成 CUE/BIN，不生成空 CDT。

有文字时将不含设备响应头的完整 18 字节 pack 流保存为 CDT，CUE 使用 `CDTEXTFILE` 引用，并尽可能提取可无损表达的标题、演唱者、作词者、作曲者、编曲者、留言、光盘 ID 和 UPC。多 block、其他编码及未映射字段保持原始字节；不可转换的内容不替换为问号。先完成新的 BIN/CDT，再原子发布 CUE；旧 CDT 不覆盖，同名时使用 `.001.cdt` 等文件名。取消或失败时尝试删除本次创建的文件。原版 CDT 的全部历史变体和实体驱动器返回尚未完成对照。

文件工具可检查 CDT，或从 CUE 导出 CDT；不会执行刻录：

```bat
ImgBurn.exe cdtext-info disc.cdt
ImgBurn.exe cdtext-build disc.cue exported.cdt
ImgBurn.exe cdtext-build disc.cue exported.cdt --replace
```

按照[原版作者说明](https://forum.imgburn.com/topic/25180-cdt-filecd-text/)，CUE 指定的 CDTEXTFILE 存在时优先使用该文件，缺失时使用 CUE 文字；已有但损坏的文件明确失败。当前生成器编码英语单 block 的 Latin1 文字，支持 TITLE/PERFORMER/SONGWRITER/COMPOSER/ARRANGER/MESSAGE、DISCID、UPC 及轨道 ISRC；不能无损表达的字符、字段和容量明确拒绝。导出使用临时文件发布，默认不覆盖，且不允许覆盖输入 CUE、音源或 CDT。Write 已接通受支持 CD-R 的 CD-TEXT SAO 导入区写入；文件生成、协议验证仍不代表实体刻录和播放器显示已验收。

数据 Verify 命令示例：

```bat
ImgBurn.exe verify R: disc.iso
ImgBurn.exe verify R: --readable --retries 0
ImgBurn.exe verify R: disc.cue
ImgBurn.exe verify R: disc.cue --compare-audio
```

Verify 默认不执行额外单扇区软件重试，可用 `--retries 0–20` 指定；多扇区失败后仍会尝试逐扇区读取。GUI 按[原版流程](https://forum.imgburn.com/topic/6392-the-imgburn-functions/)提供“与镜像文件比较”开关，默认开启；关闭后检查整盘可读性，开启时需要选择镜像。ISO 或按 2048 字节分扇区的 BIN 比较镜像声明的范围。原始 CD 的 2352 字节 BIN 需要通过 CUE 提供轨道布局。任务可以取消，结果区分内容比较、仅检查可读性和失配扇区，不能把可读性通过理解为全部内容一致。

单会话 CUE 校验支持 BINARY 的 AUDIO、MODE1/2352、MODE2/2352，以及 MOTOROLA、原生 PCM WAVE 和可解码音频，允许多个 FILE、保存在文件中的 INDEX 00 与后续索引，按真实文件偏移读取。音频的 PREGAP/POSTGAP 按生成的静音比较，不将其误算为源文件偏移；独立原始比较仍不接受数据轨生成间隙；写后校验按 SAO 规则生成间隙并重建数据头与纠错字段。核对 TOC 的轨道数、INDEX 01、音频/数据类型及 lead-out；数据轨默认比较全部 2352 字节，音频默认只检查设备返回的扇区可读性。此默认值依据原版配置及[作者对音频校验的说明](https://forum.imgburn.com/topic/20476-audio-cd-verify/)，原因是驱动器读取与写入的音频样本偏移会影响逐字节比较。Verify 命令的 `--compare-audio` 显式开启音频样本比较，不作偏移补偿；完整设置窗口尚未接通该选项。

原始校验默认只请求主通道；显式包含多会话首 INDEX00 时另读取 Q 用于边界确认，不比较 Q、CD-TEXT、MCN、ISRC 等子通道内容。音频生成间隙及原生 PCM WAVE 尾帧按零填充处理，只引用有效 PCM。受支持的精简单轨 MODE1/MODE2 的 2352 字节 MDS 与多会话 CUE 可按上述范围比较；其它扇区尺寸及原始 MDS 变体仍待接通。内容比较期间源描述和数据保持只读共享，结束复查布局，任何读错、失配、取消或布局变化均不报告成功完成。

Write 和 CUE Verify 在访问介质前完整准备音源：原生 PCM WAVE 直接读取锁定的有效数据区，其他音源解码为临时 44.1 kHz、16 位双声道 PCM；末扇区补零。重复 FILE 与同文件分轨复用已锁定的解码结果，非零解码长度缓存还须与实际扇区数一致。临时 PCM 随最后一个句柄关闭自动删除，失败和取消均释放来源与临时文件。当前单个音源最多 359849 扇区，无输出 30 秒或总解码耗时 30 分钟会报错。解码器缺失、非零退出、空或不完整样本会在更改设备前失败；已有解码结果不代表与原版系统解码器逐格式完全一致。

SPTI 命令即使只读取光盘，也要求设备句柄具有读写访问权；这不代表执行写入。Windows 权限、设备驱动和策略会影响该权限，管理员身份并非所有环境的统一前提，详见 [Microsoft 的 SCSI Pass Through 说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddscsi/ni-ntddscsi-ioctl_scsi_pass_through)。本轮在授权的管理员只读复测中已成功通过虚拟光驱完整读回样本，哈希一致；此前同一设备的非管理员进程无法获得所需访问权。程序会给出权限提示，不自动提权，也不修改设备权限或系统安全策略。

“工具 → 驱动器 → 扇区查看器”提供单扇区的十六进制和 ASCII 显示，可输入十进制 LBA、前后翻页、刷新和定位最后扇区。按照[原版说明](https://forum.imgburn.com/topic/6392-the-imgburn-functions/)，默认显示 2048 字节用户数据，CD 可选 RAW 2352 字节或 RAW 加 96 字节子通道的 2448 字节。读取在后台执行，可请求取消；失败或更改来源时清除旧显示。命令行使用相同读取核心：

```bat
ImgBurn.exe sector R: 16
ImgBurn.exe sector R: --last
ImgBurn.exe sector R: 0 --raw
ImgBurn.exe sector R: 0 --raw-sub
```

当前扇区查看器的 CD 路径仍限于受支持的单会话 TOC，音频轨不能按 2048 字节用户数据读取。原始子通道仅按设备返回的字节显示，不解码 CD-TEXT、ISRC 或保护信息，也不代表 Read 模式已经支持完整子通道镜像。托盘按钮、更多寻址方式及完整窗口布局仍待对照补齐，实体光驱的 RAW 和子通道返回尚未验收。

Write 当前支持空白 CD-R MODE1 SAO、DVD-R（含双层）DAO、DVD+R（含双层）和 BD-R SRM，ISO/BIN 必须非空且按 2048 字节分扇区。DVD/BD 使用轨道信息检查有效 NWA、起点和可写空间，不以空白盘的 READ CAPACITY 结果作为写入前提。容量检查包含末尾占用：DVD 使用 16 扇区 ECC 块，BD 使用 32 扇区簇。DVD+R 的补齐行为与[原版作者说明](https://forum.imgburn.com/topic/14049-size-of-dvd-contents-smaller-than-actual-iso-size/)一致。程序不修改源镜像，写后校验比较源镜像声明的扇区范围。DVD-R DAO 设置并回读写入参数页，在同步缓存时结束光盘；DVD+R 和 BD-R 则另外关闭轨道并结束光盘，核对光盘和最后会话均已完成。

CD Full TOC 与多会话可读性检查：

```bat
ImgBurn.exe cd-toc R:
ImgBurn.exe verify R: --readable
```

`cd-toc` 显示所有完整会话的首末轨、类型、各轨 INDEX01 和每会话 A2，依据 [MMC-5](https://www.13thmonkey.org/documentation/SCSI/mmc5r04.pdf) 格式2二进制 MSF 换算。无镜像 Verify 默认检查各会话 INDEX01 至 A2，跳过会话之间未声明为轨道数据的区域；显式 `--session-pregaps` 才进一步用 Q 确认和检查会话首00。进度累计实际检查的扇区，状态显示会话与真实 LBA，结束复查 Full TOC 和介质类型。原单会话接口保留既有行为。

多会话校验的进度总量和完成量使用实际检查的扇区数，当前物理 LBA 和会话号另在状态文字中显示，避免把跳过的间隙算作已读数据。GUI 不选择比较镜像时走同一路径；Read 的介质信息可列出多会话范围，并按前述已确认规则导出各会话 CUE/BIN，显式首 INDEX 00 由 Q 定位。真正自定义导入/导出区、少于 150 帧首00及负 LBA 主通道仍缺可表示的原版基准；无参考可读性检查与可导出镜像的边界分别保留。当前功能已通过文件和协议模型验证，尚未在实体多会话光盘验收。

CD-R 数据镜像采用与[原版 MODE1 写入日志](https://forum.imgburn.com/topic/17325-stuck-writing-leadin/)一致的 SAO 写入类型。根据 [MMC-5](https://www.13thmonkey.org/documentation/SCSI/mmc5r04.pdf) 检查 CD Mastering 能力、SAO 和 CUE 缓冲长度，设置 MODE1 参数并回读确认，再发送单轨布局。设备生成导入区、两秒预间隙和导出区，主机从 LBA 0 发送原始镜像的 2048 字节用户数据，不添加 DVD/BD 的 ECC 簇填充。ATIP 与 CUE 使用二进制 MSF，空白盘的容量在锁定后再次核对；结束后核对实际 TOC 的起点、类型和导出位置。此数据入口仅支持普通、非受限、空白 CD-R，单轨至少 300 扇区，不自动补齐更小镜像；不进行超刻、高容量扩展、TAO、RAW、数据多轨或追加会话。文件和协议验证尚未证明实体设备的导入/导出、纠错或实际刻录效果。

CD CUE 写入按[原版音频 CD 工作流程](https://forum.imgburn.com/topic/5555-how-to-write-an-audio-cd-from-music-files-using-imgburn/)从 Write 模式载入。当前支持 AUDIO、BINARY 的 MODE1/2352 与 XA MODE2/2352，以及它们的混合轨道；音频允许 BINARY、MOTOROLA、标准 44.1 kHz、16 位双声道 PCM WAVE，以及可由 FFmpeg 解码的 FLAC、MP3、AIFF 等音源。来源保持只读锁定，流式发送 2352 字节帧；MOTOROLA 转为小端样本，WAVE 只读取有效 PCM 并将最后不足一帧的部分补零，同时核对解码长度缓存。支持最多 99 轨、多个或重复 FILE、INDEX 00–99、PREGAP/POSTGAP、CATALOG、音频 ISRC 及适用的 DCP/PRE/4CH/SCMS 标志。完整 CUE 先与容量核对，结束后核对全部轨道起点、控制标志与导出位置。

首轨另向 LBA -150 发送 150 扇区零载荷 WRITE，由设备生成标准两秒预间隙；显式首轨 PREGAP 是额外间隙。首轨存储的 INDEX 00 位于非负 LBA，按源文件内容写入，并随额外 PREGAP 向后移动。仅整个镜像的第一轨省略 INDEX 00 时，会保留 FILE 零帧至非零 INDEX 01 之间的内容作为隐含 INDEX 00；显式首轨 INDEX 00，以及后续 FILE 的首个声明 INDEX，均省略此前未引用的文件内容。这些地址规则已用原版单会话镜像信息窗口逐项对照，解析、Write 和 Verify 使用一致的偏移，CUE 导出保留原始 FILE 相对索引。Read 从 LBA 0 起保存主通道并记录观察到的首轨 INDEX 00。上述能力不包括负 LBA 主通道音频；[原版作者也说明异常负地址首轨不能由普通 BIN/CUE 准确重写](https://forum.imgburn.com/topic/11679-imgburn-bincue-audio-cd-image-fails-to-mount-in-daemon-tools/)。

每轨从 INDEX 01 起的实际内容至少 300 扇区；PRE/4CH 变化需要两秒暂停。切换到不同数据结构时要求至少 225 扇区扩展预间隙，前段使用旧轨结构、末 150 扇区切换为新轨结构。数据转音频还检查前数据轨末 150 扇区零正文和音频前 150 扇区暂停，不自动修改短间隙或补齐轨道。

MODE1 数据正文保留，地址、EDC 与 P/Q 纠错码按目标 LBA 重建；XA 保留重复子头和 Form1/Form2 正文并重建相应校验，使用 XA 会话参数与数据形式。MODE2 的非重复子头在更改设备状态前拒绝，尚不支持无 XA 子头的原始 MODE2。写后数据校验使用相同 SAO 内容规则，音频沿用原版默认可读性检查；独立 Verify 的 `--compare-audio` 可比较完整音频样本，仍未补偿真实设备偏移。其他数据扇区尺寸、多会话和 MDS 写入仍待接通，不能据此认定完整 CD 刻录已复刻。

CD-TEXT 写入从驱动器报告的 track 0 起点开始，检查 R-W 写入能力，锁定后再核对导入区地址。按 MMC SAO 的 CUE 格式 41h、写入参数类型 3，每扇区发送 96 字节原始 R-W，每次最多 682 扇区，持续到 LBA -151；再发送两秒预间隙及原有主通道。每帧顺序循环四个 18 字节 pack，补全写出 CRC，不改变输入 CDT。负地址与分块方式也有[原版日志对照](https://forum.imgburn.com/topic/25716-io-error-write-error/)，但该日志记录的是一次失败写入，不作为成功介质证据。

CATALOG 是 Q 子通道的 MCN，与 CD-TEXT 的 UPC 独立，映射依据[原版作者说明](https://forum.imgburn.com/topic/22155-supported-cd-text-keysfields-in-imgburn/)。只有 CATALOG/ISRC 时保留原主通道及 Q 写入流程，不额外生成 CD-TEXT。有效 CDT 可保留多语言、未知字符集和未映射字段；CUE 文字生成仍限上述 Latin1 范围。UPC 当前接受 13 位数字，其他原版输入变体尚待对照。Read 已接通 ISRC/MCN 的查询和原始 Q 提取，不将文字区 ISRC 冒充为已读到的 Q 数据。

```bat
ImgBurn.exe burn R: disc.iso --verify
ImgBurn.exe burn R: disc.iso --speed 2.4x --verify
ImgBurn.exe burn R: disc.iso --reserve --verify --no-cycle
ImgBurn.exe burn R: disc.iso --opc --verify
ImgBurn.exe burn R: disc.iso --test
ImgBurn.exe burn R: bluray.iso --speed 4x --verify

REM 显式BD-R SRM不再预留，保留当前速度并关闭校验前托盘循环
ImgBurn.exe burn R: bluray.iso --bd-no-reserve --speed KEEP --verify --no-cycle
```

默认最高速度，DVD+R（含双层）默认不预留轨道，`--reserve` 显式启用；DVD-R DAO 和默认 BD-R SRM 路径仍预留，CD-R SAO 不发送预留命令。新增 `BurnConfig.bdNoReserve=true` / `--bd-no-reserve` 明确选择本次不发新的BD预留，不能与`--reserve`合用，其他盘型拒绝；它是依据[MMC-5](https://www.13thmonkey.org/documentation/SCSI/mmc5r04.pdf)增加的显式选择，不声称是原版已确认的设置，也不在预留失败后自动回退。它接受全新空白SRM轨，或与本次来源按32扇区上取整完全相符的已有空预留轨1，并严格核对后继未用轨2及锁后状态；不接通非空追加或部分数据恢复。默认预留仍须留空余簇，无新预留可以占用已确认可写轨的全部簇容量，此满容量边界仅有模型证据。设备速度请求以 CD 176.4 KB/s、DVD 1385 KB/s、BD 4495.5 KB/s 计算 1x；CD/DVD 使用 SET CD SPEED，BD 使用 SET STREAMING 的32位速率请求；`--speed KEEP`不发送调速命令，GUI仍默认最高速。`--opc`按[原版设置](https://forum.imgburn.com/topic/6232-the-imgburn-settings/)默认关闭。`--verify`在写入和封盘检查成功后回读，GUI默认开启；校验前默认重新装载介质，`--no-cycle`可关闭。取消在命令之间生效，已写入扇区不能撤销；失败不自动重写，尽力同步并恢复保护。完整原版失败恢复选项仍待实现。

BD-R已成功收尾后允许改报BD-ROM，仍要求完整盘状态、结束会话和单数据轨布局匹配，重载也须与已接受的结束快照一致。SPTI诊断分别记录Windows接口结果、返回范围、有效SCSI状态与sense；无有效返回时明确报告不可用，不把请求初值或旧sense解释成设备成功。错误回调是有界摘要，完整诊断可通过`OpticalDrive.lastCommand`及`spti_format_command_error`取得；后续命令会覆盖当前诊断，调用方应及时保存。

`--test` 仅用于当前支持且报告 Test Write 能力的 CD-R、DVD-R（含 Sequential DL）；DVD+R（含双层）和 BD-R 会拒绝测试写入。测试模式先回读参数确认测试位已生效，结束时退出并检查介质仍为空白。按[原版功能说明](https://forum.imgburn.com/topic/6392-the-imgburn-functions/)，GUI 勾选测试模式后取消并禁用校验，退出后恢复先前选择；底层接口和命令行也跳过测试后的回读校验。显式合用 `--opc` 仍请求实际功率校准，可能写入介质校准区。BD-R 默认接通空白单会话、单数据轨、NWA=0 的顺序写入；显式不再预留另允许上文严格匹配的已有空预留轨，不配置备区或 POW；按[原版默认设置](https://forum.imgburn.com/topic/6232-the-imgburn-settings/)在设备支持时使用 WRITE (12) 的 VNR 位，否则使用 WRITE (10)，软件回读校验独立执行。BD-R RRM、非空盘追加、多会话、其他数据 CUE、原始 CD MDS 和 DVD-R DL Layer Jump 尚未接通，多层 BD 的逐层报告与实测也未完成。厂商参数、完整缓冲策略、写入重试及真实设备兼容仍待补齐。

可通过“工具 → 驱动器 → 擦除光盘 → 快速/完整”擦除 CD-RW 和单层 DVD-RW，或使用以下命令。省略选项时执行快速擦除；`--quick` 与 `--full` 不能同时指定。

```bat
ImgBurn.exe erase R: --quick
ImgBurn.exe erase R: --full
```

擦除使用 BLANK 的最小/整盘类型，读取后台进度并在结束后核对空白状态；DVD-RW 从受限覆写格式转回顺序记录格式。快速擦除清理管理信息，数据区可能保留旧内容；完整擦除由设备清除介质，当前没有逐扇区读回验收。开始前可取消，命令开始后的取消会等待设备结束再释放保护，并报告取消；已发生的擦除不能撤销。状态读取失败或两小时等待超时会报告失败，设备的物理操作仍可能继续。DVD+RW、DVD-RAM 的格式化/覆写擦除及厂商 SmartErase仍未接通；BD-RE使用下述独立格式化和用户区清零，不套BLANK，也不代表已接通 CD-RW/DVD-RW 的镜像写入流程。

第二阶段已通过 Win32/x64 离线回归：合计 190 项原生断言及 89 项独立检查。合成样本包含 191 个文件、192 个目录，覆盖多扇区路径表、目录边界、空目录、中文、长名称别名、CRC32 碰撞及重复内容。使用独立读取器提取并核对文件名称、目录集合和 SHA-256，另检查 UDF 标签 CRC、分区地址、主备描述序列及元数据副本。x64 解析回归另经 AddressSanitizer 检查；两个架构已运行 MSVC 静态分析，原始诊断保存在本机。此结果属于离线验证，不等同于原版逐项对照、完整格式认证或真实设备验收。

第三阶段已用 Win32/x64 完整构建并独立提取一个 4 GiB+32773 字节的文件，通过 ISO、Joliet、UDF 的长度与 SHA-256 比对；另验证 Blu-ray UDF 2.50 中的大文件内容。128 GiB 和 512 GiB 稀疏样本只验证元数据、AED 续接链、标签 CRC、地址边界及独立读取器的边界读取，不计作完整内容验收。DVD/Blu-ray 使用合成目录验证文件集合、输出补丁、对齐和提取哈希，不能据此认定已通过播放器或格式认证。

第四阶段加入原版 2.5.8.0 的实际导出对照，修正 DVD MDS 的轨道模式、会话头、物理格式字段和 IFO/BUP 间距。使用独立制作工具生成含四章的小型视频和含 105 章、约 5.24 GB 的双层视频目录，再分别由原版和本程序生成镜像。两个独立读取器完整读取各镜像的文件并核对 SHA-256；双层样本的全部 VOB 内容与标题集 IFO/BUP 导航补丁一致，所选单元起点与各自 MDS 的 L0 长度一致。文件系统元数据及其占用不同，因此两份完整 ISO 的物理地址和整体哈希不要求相同。此结果仍属于文件镜像对照，不等同于播放器或实际双层刻录验收。

独立验证使用 pycdlib、7-Zip、libudfread 及单独编写的结构检查器。读取器存在各自限制：pycdlib 1.14 对桥接大文件可能复用首段 ISO 的短长度记录，且不支持 2.50 元数据分区；7-Zip 23.01 不跟随 AED 续接链。因此大文件内容和续接链分别交叉使用不同读取器，未将读取器错误算作通过。相关依赖与详细结果全部保留在本机忽略目录。

视频布局命令示例：

```bat
ImgBurn.exe build dvd.iso authored-dvd --dvd-video
ImgBurn.exe dvd-layers authored-dvd
ImgBurn.exe build dvd-dl.iso authored-dvd --dvd-video --dvd-break 1:1:54
ImgBurn.exe build dvd-menu.iso authored-dvd --dvd-video --dvd-menu-break 2:2:1:1
ImgBurn.exe dvd-layers authored-dvd --show-interleaved
ImgBurn.exe build bluray.iso authored-bluray --bluray
```

`--dvd-break` 的三个数字为候选列表中的标题集、PGC 和 Cell，必须使用当前目录实际列出的候选。菜单使用 `--dvd-menu-break VTS:LU:PGC:Cell`，其中 VTS 为 0 时表示 VMGM，LU 为从 1 开始的菜单语言单元。默认按 DVD+R DL 的 OTP 容量约束计算；可用 `--l0-min`、`--l0-max`、`--capacity` 在支持的容量范围内指定边界。`--seamless` 保留该单元的无缝标志；受影响单元的前一单元含执行命令时会拒绝此选择。镜像补丁不修改源 IFO/BUP。DVD 构建同时生成 MDS，超过单层容量时需要明确选择可用单元。

文件夹输入的 `dvd-layers` 与 `build` 可用 `--show-interleaved` 显示和选择交错单元，对应原版的 Show Interleaved Cells 选项。角度块只列首角度的单元；选择后同步更新块内全部角度及同一物理单元的其他 PGC 引用。菜单选择也同步处理其他语言单元中的相同物理引用。程序核对菜单 LU/PGC 表边界、角度块首末关系以及首个 ILVU 的 IFO/NAV 地址。

`dvd-layers` 也可只读分析本项目当前生成的 ISO9660 + UDF 1.02 DVD-Video 成品 ISO/BIN，以及 `DataImageSource` 已支持的对应 2048 字节数据 MDS，包含首卷非零字节偏移、分卷及有效轨道长度。新入口直接从 ISO 目录取得 VIDEO_TS 文件的单 extent 和真实 LBA，检查 UDF 1.02 基本桥描述符、IFO/BUP 相等及镜像内已修正的 VOB/标题集地址，再复用既有 PGC/PTT/Cell 地址表和 VOB NAV 交叉核对。

```bat
ImgBurn.exe dvd-layers movie.iso
ImgBurn.exe dvd-layers movie.bin --l0-min 0 --l0-max 2086912 --capacity 4173824
ImgBurn.exe dvd-layers movie.mds
```

成品镜像只列实际已经 16 扇区对齐、L0/L1 均非空、L0 不短于 L1，并满足给定 `DvdMediaProfile` 最小/最大 L0 及总容量的单元；`Actual-LBA` 是该单元在原镜像中的位置，`Padding` 恒为 0，`Total` 保持源的逻辑扇区数。不会调用 Build 重排、取整或补 padding，也不根据 MDS 自带的层位置伪造候选。有效导航但没有符合条件的单元时返回成功并明确显示零候选；无效或未支持格式另报错误。程序打开期间来源及全部配套文件保持只读共享，不发送任何设备命令。

本轮成品入口限于当前项目的 ISO+UDF1.02、单 extent DVD 文件及现有 DataImageSource 数据格式。UDF-only、UDF2.50、复杂角度/交错导航不支持，成品输入不能使用 `--show-interleaved`；已有文件夹显示和 Build 选择保持。UDF 检查只确认基本桥元数据，尚未将 UDF 文件树与 ISO 命名空间做全量一致性核对，不宣称任意第三方 DVD 镜像兼容。分析 API/CLI 及下述 Write 页显式候选查询均已接通；Write 自动评分/最佳单元选择、成品镜像补丁仍未实现。查询及选择本身不发送设备命令，选定 L0 沿用现有 Write 参数转发与介质检查。

Write 页选择现有 ISO/BIN/数据 MDS 后，可明确点击“工具 → Write扩展 → 查询视频换层单元...”。查询在后台执行，候选窗口显示 VTS/PGC/Cell、域与语言单元、VOB/Cell、程序、标题/章节、实际 LBA、语言及 SPLIP；同一物理 LBA 的不同 PGC/LU 引用各占一行。列表不预选、不评分，选中并点击“使用所选单元”或双击后才将该实际 LBA 填入双层 L0 配置值，可从同一扩展菜单查看。该值作为明确指定值转发，保留原有手工值覆盖 MDS、留空按 MDS/数据自动规则处理的优先级；普通数据自动平衡不称为视频单元选择。

窗口分别提示成功、合法零候选、不支持格式、读取/导航错误和取消。直接关闭、取消或 Escape 会清除本次候选与派生 L0；来源变化、过时完成结果或手工覆盖 L0 也会释放派生状态。独立手工输入的 L0 不因候选取消而被清空。同来源 Write→Read→Write 保留已明确选择的值。列表及已选状态存续期间保持来源描述和全部配套文件的只读句柄，防止来源被改写/替换；换源、取消、手工覆盖或退出后释放，不从界面线程执行镜像解析。

查询不自动寻找最佳单元，不重排、补 padding、修改镜像，也不扩大 UDF-only 或复杂交错支持范围。候选采用现有标准双层容量约束，查询不读取光驱实际容量；真实目标是否支持指定位置仍由原有写入流程检查。本轮验证没有执行实体刻录、擦除、OPC、托盘或速度设置。

DVD 视频预览命令示例：

```bat
ImgBurn.exe dvd-preview authored-dvd --title 1:1:2
ImgBurn.exe dvd-preview authored-dvd --menu 2:2:1:1
ImgBurn.exe dvd-preview authored-dvd --menu 0:1:1:1 --ffmpeg path-to-ffmpeg.exe
```

标题使用 `VTS:PGC:Cell`，菜单使用 `VTS:LU:PGC:Cell`。预览按[原版的单元预览流程](https://forum.imgburn.com/topic/4643-how-to-write-a-dvd-video-double-layer-disc-using-imgburn/)提取所选单元及前一播放单元，支持播放/暂停、逐帧、首尾定位、进度定位、相邻单元切换和 1:1 显示。VMGM/VTSM 菜单使用各自 VOB；交错角度沿同一单元的 VOBU 指针读取，可跨 VOB 分卷。画面按 IFO 中的 PAL/NTSC 制式和 4:3/16:9 比例显示，解码依赖 PATH 中的 FFmpeg 或 `--ffmpeg` 指定的程序，不依赖原版安装。

预览支持 AC3、LPCM 音轨及 Silent 选择，按 IFO/PGC 的音轨映射选择实际数据流，按视频起始时间戳对齐音频。输出使用 48 kHz、16 位双声道 PCM，播放时根据音频设备位置推进视频；暂停、定位和切轨同步处理。原版样本中 MPEG 和 DTS 未列入可选播放音轨，本程序保持这一界面边界。底层元数据与解码接口的格式能力不等于原版界面的可选范围。

BMP 按钮使用保存对话框导出当前帧的原始尺寸、24 位 BMP，不包含窗口控件。视频解码后由 C 实现原版的逐行/隔行色度重建、整数颜色转换和四像素打包行为，显示与导出使用同一帧数据。Info 独立显示画面比例、帧率、制式、扫描方式、源时间戳、NAV 扇区/VOB、VOB/Cell 标识、按钮数、视频平均码率以及音轨和播放状态。开启 Info 时按原版在预览画面绘制红色菜单按钮边框，关闭后隐藏；边框不写入 BMP，也不执行按钮命令。菜单 PCI 按钮项可读取并检查位置、方向关系及命令字节。完整 HLI 状态继承、SPU 高亮/字幕合成及 DVD 虚拟机不属于已实现范围。此处的视频菜单能力与主程序菜单、命令窗口分别记录。

标准 44.1 kHz、16 位双声道 PCM WAVE 使用原生读取；其他音频格式和重采样依赖 PATH 中可用的 [FFmpeg](https://ffmpeg.org/documentation.html)，支持范围取决于其解码器。本工程不附带 FFmpeg，缺少解码器或解码失败时会返回错误。

当前明确限制：镜像仍采用 32 位扇区地址，容量上限约为 8 TiB，实际布局还需要预留元数据空间；DVD-Video 的每个 IFO/BUP/VOB 文件限于 1 GiB−2048。源路径及镜像相对路径受 `MAX_PATH` 限制，名称分量最多 127 个 UTF-16 代码单元，暂不支持代理项字符。ISO 和 Joliet 使用有界唯一别名，UDF 保留受支持的原名。引导镜像、其他 UDF 修订号、MDS 2.x/MDX 和完整保护信息处理仍未实现；MDS 制作工具当前生成 DVD 描述，Read 另可生成受支持的精简单轨 MODE1/2048 CD 描述及上述单层 BD 数据描述。

同 BIN 跨会话在每个会话首个声明索引处重置光盘逻辑原点，源 INDEX 始终使用真实 FILE 坐标：各会话数据可以连续存放，也可以在文件中留有未引用的间隔，不能从源字节偏移里统一扣除物理会话间隙。后续会话的 `REM LEAD-OUT` 同样使用 FILE 终点，解析后再换算为绝对 A2；显式声明可裁去未引用尾部，音频导出保留相同坐标。会话默认150帧暂停与存储 INDEX00、PREGAP 独立累加。来源不足时不伪造有效轨道，完整特殊格式兼容仍需继续对照。

第五阶段补齐 VMGM/VTSM 多语言菜单单元的解析与层切换，检查相邻 LU、PGC 及命令表边界，并加入角度块和交错单元的候选与整块 SPLIP 更新。使用两个标题集、英法双语菜单及约 5.25 GB 的自制视频目录，由原版与两个架构分别生成镜像；全部 VOB 内容和 VTS IFO/BUP 补丁一致。另用受控结构样本将 160 个 VOBU 重排为两角度交错单元，与原版对照候选和两个角度的补丁，并独立核对完整文件哈希、NAV 地址及重排前后的音视频载荷。该交错样本用于结构验证，尚未经过播放器或格式认证。

CUE 按原版拒绝 CDG 轨道，REM PREGAP 仍作为注释处理；完整特殊格式兼容尚未完成。对解码音频，非零 `REM FILE-DECODED-SIZE` 按原版作为源文件解码长度缓存，载入时直接用于计算轨道范围，导出时保留；没有有效缓存时实际解码测量。缓存长度不等于已验证的音频内容，源文件仍须存在，实际解码结果需另行核对。`LEAD-OUT` 可仅引用源的一部分；导出器保留末会话的结束地址。MDS 的多会话、多轨道及交错子通道元数据解析不能视作原版应用载入或写入支持：原版应用限定单会话、单轨道，且本阶段的单轨音频及交错子通道样本被原版拒绝，写入流程仍需按实际对照边界集成。第六阶段补齐原版可载入的精简单轨数据 CD MDS：允许省略 TOC 块并保留 lead-in 计数，同时继续校验轨道数、地址、长度和文件范围。原版拒绝完整 TOC 多块描述的应用行为与元数据解析能力分别记录。

第六阶段加入菜单及标题的静音视频预览，并通过独立物理扫描核对完整单元内容，包括跨分卷单元和两个交错角度；另核对原版实际写出镜像中的单元字节及独立解码帧哈希。第七阶段加入音频播放、音轨选择、BMP 和 Info，使用独立制作的多音轨及菜单按钮样本核对 PCM、精确定位、信号频率、帧时间戳和 BMP 像素。已核对原版同帧 BMP 的尺寸，解码像素仍有差异，尚未达到逐像素一致。

第八阶段对照原版菜单样本，确认 Info 按钮边框与 BMP 导出的关系，并修正音频解码缓存的载入和导出语义。同一实际 12 秒的音频，原版分别按声明的 11 秒、13 秒载入；程序据此计算元数据并保留每个 FILE 的独立缓存。另核对 MDS 通配引用及交错子通道的完整字节。BMP 受控色块结果符合 BT.601 范围转换，但动态同帧样本仍有像素差异，不能据此认定解码器已完全复刻。

第九阶段修正原版同帧 BMP 的主要像素差异：两组逐行色块及一组隔行色块与原版逐像素一致；720×576 标题第 75 帧、菜单第 15 帧的每通道平均绝对差分别降至约 0.0175、0.0205（0–255），NTSC 720×480 首帧约为 0.0179，动态解码仍未达到完全一致。CUE 补齐 CR 单独换行及混合行尾，并验证不同文本编码。另用原版确认精简单轨数据 CD MDS 的按扇区对齐分卷可载入，跨扇区分卷仍被拒绝；两个架构完整拼接内容的哈希与原始镜像一致。

第十阶段开始补齐主程序界面，菜单层级、模式顺序和快捷键按原版核对，快速向导及 Build 页采用相近布局。镜像计算和构建在后台执行，支持取消并保护原有目标；CUE/MDS 窗口调用现有格式接口。Build 已接通文件/文件夹来源、文件系统、UDF 版本、卷标及 DVD/Blu-ray 布局选项。双层 DVD 单元选择仍须使用命令行，直接构建到光盘尚未接通。Write/Verify 页面连接现有核心接口，但尚未通过真实介质验收，不能据界面入口认定其完整复刻。

第十一阶段接通 Read 的数据 ISO 主体及 GUI，核对原版默认的软件读取重试次数，并修正 SPTI/MMC 对实际返回字节数的检查。两个架构通过读取、短传输、坏扇区、重试、取消及发布失败的隔离回归，并分别完整读回超过 4 GiB 的样本，输出全量 SHA-256 与源文件一致。测试使用真实文件和替代设备响应，不能视为实体光驱验收；本机只读虚拟光驱测试受 SPTI 访问权限限制，未计为通过。

第十二阶段加入单会话 CD 的原始 CUE/BIN 读取和数据 Verify 的可读性、镜像比较流程。使用受控的音频、MODE2、混合轨道样本，对两个架构输出的全部主通道字节和索引进行独立核对；混合轨道中的数据提取后与原版生成的 ISO 整体哈希一致，并由独立读取器打开。原始轨道样本属于结构测试，尚未证明实体光驱的音频读取、纠错或子通道行为。另通过用户确认的管理员只读复测，确认 SPTI 可用、虚拟 ISO 完整读回一致并卸载本次样本挂载；第十一阶段的权限阻碍在该测试环境已解决，不等同于实体介质验收。

第十三阶段补齐单会话原始 CD 的可读性和 CUE/BIN 校验闭环，使用本程序 Read 实际输出进行回读比较。两个架构分别验证原版默认的音频忽略行为、显式音频比较、单 BIN 与多文件偏移、Motorola 音频、数据头部与内容失配、短传输、源读错、取消、结束 TOC 变化及源文件锁定。独立核对完整输出、数据/音频范围、修改扇区数量和源文件哈希。此结果属于隔离设备响应和文件内容验证；没有新增实体光驱校验，也不能视作原版实际设备逐项对照。

第十四阶段接入可选本机图像并加入扇区查看器，同时修正 GET CONFIGURATION 短响应仍被当作有效配置的实现错误。两个架构分别验证单扇区读取、取消、越界、短传输、结束介质布局变化和显示内容；独立逐字节核对读取结果与十六进制文本，并对原版 PNG 的透明度、预乘颜色和绘制像素进行核对。Read/Verify 的数据和原始 CD 回归继续通过，新增模块另经内存检查。实际检查了向导、构建页及隔离设备的扇区窗口；这些检查不代替实体介质验收。没有原版资源的两个架构也已独立构建通过。

第十五阶段将 Write 从通用命令原型推进到单层 DVD+R 数据镜像流程，修正轨道 Packet 位、16 位轨道/会话编号、短信息响应及短 WRITE 传输检查。收尾按 [MMC-5 草案](https://www.13thmonkey.org/documentation/SCSI/mmc5r04.pdf)区分关闭轨道和结束 DVD+R 光盘。使用文件承载的隔离设备分别验证 Win32/x64 的完整写入、回读、重新装载、容量、短传输、源读错、收尾失败、取消和状态恢复；独立核对完整镜像字节、UDF 提取和设备命令顺序，并检查非整 ECC 尾部的源数据与设备补齐区域。新增回归另经 x64 内存检查，已有 Read/Verify 回归继续通过，没有执行真实光盘写入。

第十六阶段加入空白单层 DVD-R DAO、受设备能力限制的测试写入及可选 OPC。检查完整能力描述、52/56 字节写入参数页、测试位回读和退出，覆盖设置被忽略、短传输、OPC 失败、取消及恢复失败。两个架构的隔离设备完整写入与回读内容经独立核对，测试模式的数据流与源镜像一致且模拟介质数据区保持空白。实际检查隔离 GUI 的测试/校验互斥和选择恢复；已有 Read/Verify 回归继续通过，新增回归另经 x64 内存检查。本阶段未向实际光驱执行写入或 OPC，也未完成原版实际设备逐项对照。

第十七阶段补齐 CD-RW、单层 DVD-RW 的快速/完整擦除、后台状态与进度、取消等待和结束检查，接通 GUI 原有擦除菜单及命令行，并修正完整文件路径被误截为驱动器盘符的解析错误。隔离回归覆盖旧设备忙时 TUR 返回成功、固定/描述符格式进度、短响应、介质变化、设备错误、超时、取消和保护恢复。独立核对 BLANK 命令参数与模型数据区的保留/清除结果，已有 Write/Read/Verify 回归继续通过，新增回归另经 x64 内存检查。实际检查隔离 GUI 的菜单及两种确认窗口并选择“否”，模拟数据哈希保持不变；未擦除实体光盘，也没有据文件模型认定实际介质清除效果。

第十八阶段接通 BD-R SRM 的数据镜像写入与 GUI/命令行入口，检查完整 BD Write 能力描述、VNR 支持、32 位速度、32 扇区簇容量、预留及轨道/光盘收尾。修正收尾与重新装载时遗漏最后会话状态的实现检查。两个架构使用隔离设备验证完整写入和回读、VNR 回退、16x 速率、非整簇末尾、短传输、错误和取消；独立核对所有输出字节、UDF 提取文件哈希、命令顺序和参数，新增回归另经 x64 内存检查。已有 DVD、擦除、Read/Verify 回归继续通过。全部仍属文件和协议验证，未执行实际刻录。实机验收按项目所有者安排，待功能开发完成后先验证 BD、CD、DVD，再验证可擦除光盘。

第十九阶段接通 CD-R MODE1 SAO 数据镜像写入、GUI 测试写入能力及命令行 CD 速度换算。两个架构使用隔离设备核对 ATIP、完整 CUE、写入参数、收尾 TOC、最小轨道及非整簇末尾，覆盖短传输、错误、取消、测试模式退出和写后比较。独立逐字节检查原版生成镜像的模拟写入/读回输出，并提取文件核对哈希；实际命令行入口连接同一隔离设备，检查 CD/DVD/BD 的不同速度单位。已有 DVD、BD、擦除与 Read/Verify 回归继续通过，另经 x64 内存检查；这些结果不代替实机刻录验收。

第二十阶段接通 BINARY/MOTOROLA 多轨音频 CUE 写入，以及音频间隙的回读比较；修正上一阶段仅声明首轨预间隙却未发送对应零载荷 WRITE 的实现遗漏。两个架构与内存检查使用独立指定的 TOC 和 PCM 样本，核对单轨、多轨、多文件、字节序、生成间隙、后续索引、元数据、99 轨和错误/取消，完整比较输出字节、CUE 参数与命令顺序。GUI 工作者和实际 CLI 入口通过隔离设备验证，旧模式回归继续通过。尚未操作实体光盘，也未把协议模型结果当作实际介质验收。

第二十一阶段加入原生 PCM WAVE 供给、首轨额外预间隙、MODE1/XA 数据及混轨 SAO 写入和写后规范化校验。使用原版只读载入对照首轨 PREGAP；隔离设备输出与独立生成的 PCM、扇区头、EDC 及 P/Q 校验参考逐字节比较，覆盖重复 WAVE、非整帧尾、双向混轨、扩展间隙、错误校验字段重建和不支持格式的提前拒绝。旧 Read/Verify、DVD、BD、擦除及 CLI 回归继续检查。该阶段尚未处理扩展预间隙旧控制位和 SCMS，后续第二十二阶段补齐受检查的范围，仍不代表全部原始轨道变体。所有验证仍为文件、协议和只读原版界面对照，未执行实体刻录。

第二十二阶段接通 FLAC、MP3、AIFF 和非标准 WAVE 的解码供给及 CUE 校验，复用可取消的锁定音源，完整检查解码退出结果和临时文件生命周期；保留原版 REM 缓存的元数据语义，写入与校验时另核对真实内容。Read 增加扩展预间隙前段旧结构和 SCMS 周期处理。隔离写入输出与独立 FFmpeg 命令行解码结果逐字节比较，无损格式另与原始 PCM 核对；新增 Read 输出由独立 Q CRC、轨道索引和完整 BIN 检查器验证。已有模式回归继续检查，所有证据均保持本机忽略；尚未执行实体刻录或宣称与原版所有解码器一致。

第二十三阶段加入 CD-TEXT pack 的解析、保留及受支持元数据生成，接通 MMC 格式 5、Read 的 CUE/CDT 输出、GUI 读取开关和文件命令。修正 CUE 解析对缺失 CDTEXTFILE 的过早拒绝，使文件导出能够按原版规则回退到 CUE 文字。独立使用 cdrdao 官方编码器产物及另一份 CRC、跨包字段和 size information 检查器核对，不将编码和解析相互通过作为唯一依据；覆盖多 block、Latin1、TAB 重复、容量、未知编码、错误数据、取消和输入/输出保护。CD-TEXT 的实际 SAO 写入、原版 CDT 全部变体和实体介质验收仍待完成。

当前验证仍不代表完整 DVD 播放器或介质验收。原版星级评定、更多实际多角度/多故事样本及介质验收仍待补齐；PTP 解析与描述文件生成不代表可按 PTP 写入可录双层介质。后续继续补齐 Read/Verify 的剩余原始轨道能力，逐项推进 Write、Discovery 及其菜单、命令窗口和剩余格式兼容。原版缺陷修复与额外改进仍在复刻完成后讨论。

第二十四阶段接通 CD-TEXT 的 SAO 导入区、R-W 能力及负 LBA 检查，补齐原版支持的 CUE 文字字段，并修正本实现将 CATALOG 误映射为文字区 UPC 的错误。独立编码参考核对每帧 R-W、跨块循环及完整导入区，隔离设备模型检查主通道、命令顺序、输入锁、错误和取消；Read 与文件工具同步保留新增字段。原有模式继续回归，实体刻录、播放器显示及更多驱动器兼容仍待验收。

第二十五阶段接通首轨存储 INDEX 00 的 SAO 写入，修正显式首索引和后续 FILE 前缀的地址计算，使解析、Write、Verify 与原版单会话样本一致。六组原版只读镜像信息与两个架构的正式文件命令核对总长及轨道范围；独立 PCM、数据扇区和设备 CUE 参考核对完整输出，并检查 Read 输出再 Verify、GUI 后台、实际 CLI、解码缓存、错误和取消。标准负地址预间隙仍由设备生成，不将正区隐藏内容宣称为负 LBA 音频支持。多会话新 FILE 非零索引与默认会话间隙的组合仍待原版对照，实际介质验收仍未完成。

第二十六阶段接通 Read 的盘级 MCN、逐音轨 ISRC 双路径提取及 CUE 导出，读取前后复查有效查询值，并保留缺失、坏样本、冲突和轨道归属不确定的边界。使用标准位布局、独立 Q 编码与 CRC 参考核对编号；Read 输出再交给生产 Write/Verify，逐字节检查完整主通道和设备 CUE，覆盖 GUI 后台、实际 CLI、CD-TEXT 来源分离、错误、取消与旧输出保护。既有模式继续回归；协议模型和文件闭环不代表已在实体光驱读取或刻录验收。

第二十九阶段接通带 OTP 层信息的 2048 字节 MDS 到 DVD+R DL 的实际写入主流程，包含双层能力、L0 容量设置与回读、逐层数据传输、最小半径收尾及写后校验。独立二进制描述和非零字节参考核对分卷偏移、两种层切换位置及全部输出；GUI 后台和正式 CLI 进入同一生产路径，覆盖取消、设置失败或未生效、固定层容量、收尾错误及重新装载后的层位置。四种构建及相关既有 Write、Read/Verify MDS、Discovery 回归通过，专项另经 x64 内存检查。大容量扇区计算与小容量完整文件验证分别记录，不冒充实体双层介质验收；自动层位置选择及更多输入仍待后续。

第三十阶段接通双层 DVD 的 PFI 布局读取与 MDS/ISO 保存，补成受支持 OTP 来源的 Read→MDS→DVD+R DL Write→Verify 文件/协议闭环。PTP 分别读取两层，保留 L1 长于 L0 和原层位置；Read 输出不擅自取整或套用制作新镜像的介质上限。独立解析输出 MDS、核对全部 ISO/回写字节和设备命令，覆盖 GUI 后台、正式 CLI、未对齐来源、取消、结束时层变化及错误 PFI。四配置和相关旧流程回归通过，专项另经 x64 内存检查；大容量稀疏样本只验证元数据，未执行实体光盘读取或刻录验收。

第三十一阶段接通 Read 显式分卷输出、MDS 顺序文件列表和 GUI/CLI 入口，沿用先数据后描述及旧文件保护。独立逐字节拼接核对非整传输块分卷、整除无空卷、整组重名换组、OTP/PTP、取消和失败清理，以及分卷 Read→Write→Verify；另接通已有原版证据支持的精简单轨 MODE1/2048 CD MDS 读取与分卷闭环。原始音频/交错子通道 MDS 和多会话未据此开放，后续继续各自的原版对照与实现。四种构建、相关既有回归及专项内存检查通过；仍未执行实体光驱验收。

第三十二阶段接通 Full TOC 与每会话 A0/A1/A2、轨道和 lead-out 结构，接入 CLI 会话显示、GUI 介质信息、Read 前置识别及无参考多会话可读性 Verify。独立二会话/四轨模型核对全部 1,403 扇区返回内容，确认没有读取 11,400 扇区会话间隙；检查短响应、错误、重试、取消、结束布局变化和旧输出保护。既有单会话 Read/Verify、Write 和 Discovery 相关回归继续通过，专项经 x64 内存检查；原版只读输入对照和实体介质验收分开记录，多会话 CUE 导出/比较及原始 CD MDS 仍继续按证据推进。

本阶段另用原版两 FILE、两会话音频 CUE 核对后 FILE 的非零 INDEX 01、存储 INDEX 00 和显式 PREGAP：会话首默认 150 帧暂停与这些内容独立。解析器据此补齐独立新 FILE 分支的地址计算；两个架构的四组正式 CUE 命令核对起点、长度、总长及源偏移。该观察不外推同一 BIN 跨会话或所有数据会话变体。精简 MODE1/2352 MDS 另已获原版只读载入确认，尚未据此接通本程序的原始 MDS 设备流程。原版启动发生真实光驱只读枚举，无盘且未执行实体读盘、写入、OPC、托盘或设置修改。

第三十五阶段收敛原始 CD 的数据 MDS 与 CUE 分工，接通 MODE2/2352 精简 MDS Read/Verify 和 MODE1/MODE2 主通道分卷。有效 XA Form1/Form2、实际 MODE1 分卷、实际 MODE2 Read 输出与既有特殊会话 CUE 按原版文件载入分别对照，完整主通道、分卷边界、回读比较及既有会话00回归由独立文件/协议模型核对。原版音频 MDS 拒绝和很小首卷 EOF 单独保留为反例；原版应用单会话单轨边界不等同 parser 可解析的元数据范围，真实介质验收仍待用户安排。

第三十六阶段补齐 Read 常用流程的 Auto/完整预设/自定义分卷、可调软件重试、数据与音频读取速度及成功后弹盘，接通 CLI、GUI 页面和后台。独立文件/协议模型核对文件系统上限、实际数据内容与读取范围、速度命令、混合会话00速度归属、发布与弹盘顺序，沿用既有 MDS/特殊会话回归；这不代表已在真实 FAT、网络卷、实体测速或弹盘验收。文件系统有效长度、完整布局文件策略、任意 P-W、负 LBA 及完整设置持久化仍按现有边界另行推进。

第三十七阶段推进剩余写入：无 MDS 的 DVD+R DL 数据镜像可自动平衡 ECC 层容量，GUI/CLI 可显式选择 L0 并覆盖来源元数据；新增 DVD-R DL Sequential 固定层 DAO 与测试写入、分层进度及写后比较。独立协议模型核对完整非零写出、源偏移/分卷、跨层边界、两类介质各自的能力与收尾、取消和结束/重载层位置变化，既有 Read/MDS/特殊会话继续回归。普通数据平衡、视频单元候选、仅首层、Layer Jump、追加多会话及可擦写格式化分别保留边界，实体介质验收仍待用户安排。

第三十八阶段新增已生成 DVD-Video 镜像的只读单元候选分析。真实 extent 与 DataImageSource 跨卷/首偏移读取共用既有导航核对，候选仅使用成品实际对齐 LBA。独立已知导航夹具逐字段核对单元、VOB、章节、镜像地址和容量过滤，记录完整来源 SHA 不变和新入口零设备命令；原文件夹导航/Build 换层及相关 Read/MDS/双层 Write 继续回归。原版确认实际小镜像的 ISO9660/UDF1.02 载入，但其提示小镜像无需双层，未列候选；候选字段采用既有原版导航基准与本轮独立结构核对，不冒充原版大容量或实体介质验收。

第三十九阶段把成品 DVD-Video 只读候选接入 Write 页的显式后台查询、列表和手工选择提示。候选与只读来源一同持有，来源切换/取消/失效清派生选择，手工覆盖释放候选；原有 Read 控件及 L0/MDS 优先级保留。纯文件/隐藏真实窗口模型逐字段核对列表、L0 转发、切页与取消消息，查询计零设备命令、来源 SHA 不变；新增 ListView 窗口销毁通知触发的生命周期问题已在本轮修正并经 x64 ASAN 复验。原版字段与明确选择约定复用既有标题/菜单观察，不将本轮模型或代理 GUI 检查称为原版完整界面/实体介质验收。

第四十阶段增加 CD-R MODE1 追加数据范围的只读规划，复用盘/轨信息和 Full TOC，先确定 TAO 下 NWA 的用户数据语义，不猜测 SAO 预间隙或文件树合并。

第四十一阶段按用户授权将追加写入作为原版以外的实验扩展。独立 API 支持明确建立首会话、追加原始 MODE1 数据、留盘开放和封盘，未接入原有 Write 页或 burn 命令。在同一张 CD-R 上依实际 NWA 写入三个各 8 MiB 的会话，使用独立回采及生产只读验证接口核对全部源 SHA 与旧会话数据不变；没有为兼容问题重写载荷或换第二张盘。现场发现的 ATIP 子类型兼容、奇数 Full TOC 分配及封盘后 CD-ROM Profile 三处具体阻断均定点修正；第二/三次原调用的停止记录及后续只读恢复分别保留，不把恢复说成原调用已成功。四配置及必要规划/MMC/原空白盘门槛回归通过，新增协议模型经 x64 ASAN；GUI 集成、原版的一次性多会话 CUE、SAO 追加、ISO 地址重定位/文件树合并与其他介质实体写入仍未覆盖。

第四十二阶段为独立 ISO 增加只读地址重定位视图，并接入单独的实验 ISO 会话写入/校验 API；既有 BIN、CLI/GUI 和空白盘路径保持原行为。独立构造的两个命名空间、可选路径表、多 extent、空文件、跨扇区记录及尾 padding 样本，逐字段核对实际 NWA、全部地址、非地址字节和文件载荷，覆盖不支持格式、截断/结构冲突、取消、溢出及输入锁。Win32/x64 共 364 项断言、x64 ASAN 182 项断言通过，117 份完整输出通过独立读取和 117,687 项字段核对，四配置构建通过；生产 Build 的纯 ISO、ISO+Joliet 与 UDF 拒绝边界另作兼容检查。既有同一张 CD 的三个原始数据会话由生产校验接口与独立回采只读核对，全部 SHA 不变、设备写入或设置命令为零、新增耗材为零。这些证据不代表新 ISO 的实体写入、挂载或播放器验收；Joliet 仅文件视图、UDF/引导等更多重定位格式、旧文件树合并及界面集成继续保留边界。

第四十三阶段接通独立纯 ISO9660 的专用 `append-iso` 命令和同约定的只读规划 API，默认仅查询，显式 `--write` 才进入 TAO 执行并完整回读。新增不管理 MCN/卷锁的被动设备打开，保留旧设备打开、burn、Read 与 GUI 行为。正式命令行入口在 Win32/x64 共 78 个场景及 x64 ASAN 39 个场景通过，27 个计划逐 13 字段一致，15 份完整写出流通过独立读取及 11,970 项字段核对；被动打开另经 162 项原生与 81 项 ASAN 断言，四配置构建通过。已由生产 Build 准备两份各 408 扇区的纯 ISO，并独立提取全部 8 文件核对 SHA；尚未实际刻录或挂载。当前现场尝试仅完成设备属性只读查询，因进程未取得 SPTI 命令访问权而以 error 5 停止，0 条 SCSI 命令，不能记为封盘识别实体验收。没有修改权限或绕过访问限制，新增耗材为零；实盘只读复验及下一张 CD-R 的两独立会话验收留待用户方便时安排。

第四十四阶段把独立纯 ISO9660 追加接到 Write 页的专用扩展窗口，沿用现有后台消息、只读规划、明确选择及一次执行约定。真实隐藏 Win32 控件/消息与生产协议模型通过 Win32/x64 共 412 项原生、x64 ASAN 206 项判据；18 份完整输出通过独立读取及 14,343 项字段核对，33 个计划逐 13 字段一致，另逐字段验证所选计划失配即拒绝。默认状态、选项/来源/驱动失效、取消/晚到结果、一次消费、Read/L0保留及96/144 DPI控件边界均检查，四配置构建通过。用户后续提供旧只读工具的成功命令输出：两条查询确认当前三会话盘已封盘并按预期拒绝追加，写入/设置计数为零；这是人工只读证据，不是新 GUI 或新 ISO 的实体刻录验收。桌面工具启动受 Q 卷识别阻断，本轮未查看当前远程屏幕或进行可见布局/逐像素原版验收；现有素材清单未发现缺失，新扩展没有扩大格式边界，新增耗材为零。

第四十五阶段按原版表单及既有截图推进主窗和 Write 外观，补齐 Tahoma 字体、六模式按钮、来源/目标/设置区及图片状态；左侧后台来源摘要与右侧介质信息分开，原刻录按钮增加可写空白盘候选状态，已有 L0/视频候选/CD 追加入口移到 Write扩展菜单。现有素材未发现缺失，配置 PNG 从14增至29。Win32/x64 各1476项、x64 ASAN 1476项通过，分别覆盖206项必要后台/协议回归、784项隐藏控件/状态/绘图分派、486项图片合同；另有x64及ASAN各38项纯文件摘要检查，四配置构建通过。原版85个命名PNG与本机素材逐SHA一致，76条控件记录及160次绘图分派按96/144 DPI核对；现有来源、夹具和素材不变，零实体设备命令与新增耗材。原生装饰线、Start/设备图的近似运行时映射、当前RDP可见效果及完整逐像素一致性仍需后续检查，其他页外观和未实现控件继续按范围推进；本轮未执行实体刻录。

第四十六阶段按原版DFM推进Read主页的图片、布局与状态，保留第45阶段主窗/Write成果。目标容量/空余空间由后台只读查询，原有分卷、CD-TEXT、raw MDS、会话首00、重试及成功后弹盘移入读取选项，Apply保存、取消保留；补齐同路径显式重试、无光驱刷新、150%DPI设备图行高及Build模式遗留标志误禁Read目标菜单。四配置构建通过，Win32/x64与x64 ASAN共6420项GUI/协议/图片判据通过，另有x64/ASAN各66项目标摘要检查；每架构Read新增595项、62条DFM控件记录、84次绘图分派，30图像合同及42条704扇区ISO/MDS完整输出记录独立核对通过。全部来源、素材及旧证据SHA不变，零实体光驱命令与新增耗材。Read盘内摘要仍未知，Start/设备图的运行时绑定、目标编辑态、原版下拉列表样式差异及当前可见桌面像素效果继续如实保留边界，未扩大格式或执行实体刻录。

第四十七阶段完成独立纯ISO9660追加执行核心的两会话实体测试及Windows文件访问核对，首会话和第二会话封盘后完整SHA保持一致；旧三会话数据盘也先作完整只读回采回归。本轮沿用未改动的50份生产C/H及已通过四配置的程序，新增受限验收器x64/ASAN各68项、独立文件6正例/16反例/3个CLI例及8份模型回采交叉检查。实体阶段分步执行，任何失败即停止，不自动串写或重试；实际新消耗1张CD-R。未扩大Joliet/UDF、文件树合并或介质边界，GUI实际点击及其余介质仍按后续批次验证。

第四十八阶段完成单层DVD+R普通数据写入核心的实体闭环。先据MMC核对固定包16扇区、NWA和轨道关闭/封盘语义，新增显式沿用当前写入速度的API尾字段，默认false保留旧行为；没有扩大GUI、CLI或格式边界。只读资格检查后，用一张DVD+R写入15936扇区的生产ISO9660/Joliet/UDF1.02镜像，498次连续写入、关闭轨道和封盘均成功；内置及独立只读校验各比较全部15936扇区，两份完整回采的SHA与源相同，三命名空间各8文件独立提取一致。Windows无需重新插盘即识别UDF，8文件大小/SHA及完整文件集合匹配。受限写入协议模型x64/ASAN各52项、只读响应模型各194项和四配置新构建通过，默认速度语义、必要失败边界及旧素材/夹具身份保持。没有发送OPC、托盘移动、速度设置或MCN命令，新增耗材1张单层DVD；实体验收限本次源布局、单台设备和生产执行核心，不代表GUI点击或其他介质已通过。

第四十九阶段准备单层DVD-R Sequential的受限DAO验收入口，沿用生产管线，限定已知对齐镜像、空盘和经查询确认的NWA0；按MMC核对002F能力、页05设置及读回、整轨预留和SYNC收尾，禁止套用DVD+R关闭功能。独立协议模型x64/ASAN各73项、每架构42场景通过，4份完整模拟回采均与固定15936扇区源逐SHA一致，每份三命名空间各8文件独立提取匹配；覆盖能力/参数拒绝、预留失败、取消、短读写及结束状态，并单独记录预留已可能改变介质的尝试。生产50份C/H、来源、407份旧素材/夹具和四配置产物散列保持，四配置沿用上阶段构建。本轮仅在新插入DVD+R上执行4条只读确认查询，该盘保持空白、耗材账本未变；新的DVD-R实体验收器未连接设备运行，DVD-R的实际写入、最小来源、固件收尾耗时与文件访问仍待换盘后验证。

第五十阶段推进BD-R SRM受限验收。只读确认空白单层0041h介质、NWA0、可写轨道容量和SVNR，按MMC及BD物理格式依据独立核对原始响应；固定15936扇区数据源，分步调用既有生产管线，限制32扇区簇、53预留、VNR或WRITE10回退及同步/关闭顺序。主体与入口守卫模型在x64/ASAN各151项通过，共9786条CDB及8份完整模拟输出/回采独立核对，每份三命名空间各8文件匹配。生产50份C/H未变，四配置明确复用旧构建。本次实体预留请求一次返回Windows I/O错误1117，无可用设备sense，数据写入次数为零，立即停止且保护恢复；按可能改变介质保守计入一张BD，不自动重试。未发送OPC、速度、托盘或MCN命令，未执行DVD-R测试；BD实写、回采、Windows挂载和GUI点击仍待成功验收。

第五十一阶段新增BD-R SRM显式不再预留写入、已有空预留轨的严格匹配、CLI保留当前速度与SPTI有效返回诊断，并修正实盘封盘后改报BD-ROM造成的后置误判。最终源码四配置新构建通过，BD API/CLI的x64/ASAN共310项、47236条CDB独立核对通过，真实结束响应直接用于规则回归；SPTI与旧Write必要回归另行通过。实体用同一已计账BD写15936扇区，498次VNR写、同步及两步关闭成功；原调用因旧Profile检查返回失败，未重写。修正后仅只读完整回采，源SHA、三命名空间各8文件及Windows UDF文件集合/大小/SHA全部一致，失败原件与只读恢复分开记录。无新增预留、OPC、速度、托盘或MCN，保护恢复；未新增耗材或测试DVD-R。未用第二张盘重复执行修正后的完整写入调用，更多设备/布局及GUI点击继续保留边界。

第五十二阶段补齐BD-R只读物理摘要、新bd-info命令及既有GUI后台信息显示，区分封盘后逻辑BD-ROM与物理BD-R；未知结构只显示未知，不改变数据读写资格或MDS范围。Win32/x64/ASAN各136个解析场景及x64/ASAN各28个实际CLI/worker场景通过，共1190条模型CDB及真实输出独立核对，四配置新构建零警告。已封盘BD的生产API和实际CLI分两次只读验收，13条CDB、15次IOCTL及全部字段/输出独立核对，报告单层BD-R、Class0/Version1、介质ID和15936扇区逻辑范围正确，原始DI与历史PIC一致。无写入、设置、MCN、锁或新耗材；新BD暂不需要，可见GUI像素及其他物理族/布局继续按范围验收。

第五十三阶段新增Class0/Version2物理BDW格式1/2/3的单/双层只读摘要，正确解释BD-RE专用legacy层标记、layerType4及固定footer；兼容逻辑Profile0040/0043，不由Profile猜物理盘型。公共结构与CLI/GUI生产不变，新摘要沿用既有显示，未知布局不扩展数据读写/擦除资格。最终源码四配置全新零警告构建，生产解析模型906场景及实际CLI/GUIworker模型202场景，共3705条模型CDB和1456输出字段独立核对通过，含旧BDR回归。BDW夹具为按原文构造的合成元数据；本轮未访问光驱、未进行BD-RE实体或可见GUI验收，未新增耗材。物理BDO因尚缺足够原始格式依据继续未知；BD-RE写入/擦除/格式化仍未接通。

第五十四阶段加入只读READ FORMAT CAPACITIES原始API及BD-RE可选格式状态/容量列表，保留DT/FT、原始TDP、设备顺序、重复和未知格式，不将BD-RE备区簇数误算为块长度；DT3保留无盘或容量未知的合称。新查询与既有可读容量/轨1空余分开，失败只显示未知，不改变原命令退出值、就绪或写擦资格。最终四配置全新零警告构建，底层/必要旧metadata模型483项及实际CLI/GUIworker模型100项，共1552条模型CDB及3632输出字段独立核对通过。模型验证最大30个后续描述符、原始参数极值、短传/保留位、失败清零和可选回退。本轮未访问光驱、未新增耗材，未进行真实BD-RE或可见GUI验收，也未发送FORMAT UNIT或接通BD-RE写/擦/格式化。

第五十五阶段接通BD-RE显式FT30/31格式化、已格式化介质数据覆写、前800扇区/全用户区清零和回读，增加独立CLI及GUI一次性确认流程；默认保留设备常规写校验，不将BD-R VNR当BD-RE FastWrite。最终R3四配置实际CL/Link均严格/WX且零警告，低层426、管线194、客户端84项共704个模型场景/10866条CDB独立核对通过，固定15936扇区完整回采及三个命名空间各8文件匹配。可见模拟GUI仅验窗口、只读列表和初始无选择/按钮禁用；工具未完成owned对话框交互，不冒称真实GUI设备验收。本轮无实体格式/写入或新耗材。继续推进原版已有的BD-R Incremental、BDXL及BD数据MDS；原版不支持的追加/多会话扩展留到以后，不把常用主流程收尾称为BD全部完成。

第五十八阶段接通已识别单层BD-R/BD-RE的Read数据MDS单文件与分卷生成，原版静态字段核对后使用单独的虚拟DVD-R描述，保留完整逻辑长度及PFI低24位写法；读取前后核对BD公共摘要，未知物理格式和多层MDS读前拒绝，旧CD/DVD生成器字节不变。最终R2四配置实际CL/Link严格/WX且零警告；x64/ASAN各69个实际CLI/GUIworker场景、1620条CDB独立核对通过，大容量稀疏夹具只验结构和边界抽读。实体复用已有封盘BD-R，完整读取15936扇区并生成MDS，回采与输出ISO均匹配原始源SHA，三命名空间各8文件一致；522条只读CDB及4326字节MDS经7883项独立核对，无写盘、速度、托盘或MCN设置。可见只读模拟格式窗口取得列表并保持初始禁用，选择交互仍受工具限制；多层MDS、真实BD-RE及新介质格式/写入仍待后续验收。

第五十九阶段接通常规双层BD-R/BD-RE的Read虚拟OTP MDS及分卷，按原版缓存路径取首层物理跨度，补全读取前后层字段稳定检查，逻辑读取保持连续。四配置实际CL/Link严格/WX、零警告；低层2412个模型案例、客户端x64/ASAN各102个场景通过，旧SL/CD/DVD共30对完整输出逐字节相同。原版Tools用40GiB合成稀疏ISO实际生成双层MDS，新生成器的完整头及两份结构共8392字节与之完全一致；Tools和Read的footer参数及文件名按各自调用合同分别核对，不称整个文件相同。该取样没有光驱后端，不是原版BD Read或实体双层验收；本轮未访问实体光驱、未新增耗材。BDXL的PAA偏移与普通双层不同，原版其余入口及输出合同尚未闭合，继续保留未知边界；BD-RE实体格式/覆写和完整可见UI也仍待验收，DVD继续后置。

第六十阶段补齐已录逻辑数据量小于或等于首层物理跨度的常规双层BD MDS。来源流、CLI/GUI元信息及Verify使用独立来源解析入口，旧严格布局API保留；DVD目标明确拒绝超出数据长度的来源层提示，手工目标L0也不能绕过。四配置实际CL/Link严格/WX、零警告；独立Win32的16个解析用例和5条完整数据流通过，x64/ASAN客户端各167场景、4753条模型CDB独立核对通过，旧SL/CD/DVD30对完整输出相同。原版实际载入control/partial/equal三份本地MDS，显示完整N与独立LB；当前Read生成的8422字节部分盘MDS与其中已被原版接受的合成夹具整文件相同，实际载入与原版生成输出分别记录。三个取样程序正常关闭，本轮没有真实光驱操作或新耗材；BDXL/BDO及24位回绕等未闭合范围、实体DL/BD-RE和完整可见UI仍待后续，DVD继续后置。

第六十一阶段接通已识别BD-R XL三层/四层的Read虚拟MDS链，按原版Read刷新保存固定槽最后命中的原始DWORD缓存，提示与OTP独立；来源解析新增不足半程、零端点及下溢提示，旧严格API和DVD双层写入门槛保留。四配置实际CL/Link严格WX、零警告；Win32/x64/ASAN低层3378案例、客户端x64/ASAN各247场景与7324条模型CDB独立核验通过，旧SL/CD/DVD30对完整输出保持。原版同一个进程实际载入三份本地合成MDS，确认64GiB不足半程提示、零端点无LB行及回绕的unsigned显示；新creator头与footer共8408字节和已接受的half夹具一致，仅Unicode文件名不同。程序正常关闭、电脑归还，未新增光盘操作或耗材；这不是原版BDXL Read或实体多层验收。Tools创建MDS的大容量入口、BDO等原版缺口及实体DL/XL/BD-RE仍待后续，DVD继续暂缓。

第六十二阶段接通Tools大容量MDS，补齐五种描述类型、完整路径选项和32位换层输入；使用原版Tools的独立结构及每卷footer，取消旧DVD容量门槛。来源模式补齐三种已证类型别名，旧严格解析及DVD写入边界保持。最终四配置实际CL/Link严格WX、零警告；x64/ASAN各94场景、每架构50条完整逻辑流通过独立核验。原版新生成40GiB DVD-ROM单层样本，全4320字节与独立合同相同，并复核旧40GiB DVD-R/OTP样本8420字节；PTP及多卷另有静态和模型证据。原版生成后主窗禁用，正常关闭未生效，已核保存结果后仅强制结束本轮隔离进程并确认消失，电脑归还；不伪造正常关闭日志或结束计数。本轮无新耗材，DVD继续后置；ANSI等Tools其余入口、BDO及实体多层仍待分别补齐。

第六十三阶段接通大容量PTP低24地址回绕和无扩展名裸“*”的MDS来源消费。PTP保留原版DWORD提示，独立核对两层跨度的模24总长，不猜真实物理层高位；文件名以描述基本名主干确定性替换，旧严格解析和目标写入资格保持。最终四配置实际CL/Link严格WX、零警告；x64/ASAN各59场景及实际CLI/GUI后台字段通过独立核验，每架构27条完整逻辑流、7个大源抽样和1055次DataSource读取的偏移/长度逐项核对。依据为原版机器字节及文件模型，本轮未占用桌面、未运行原版或访问光驱，没有新耗材；Tools描述来源继承、非整扇区、BDO等原版缺口和实体多层/完整可见界面仍待后续，DVD继续暂缓。

第六十四阶段接通Tools单独.DVD文本描述输入，按原版文件优先、BOM/系统ANSI及正int64低DWORD规则展开整卷数据并继承层提示，新增未勾选继承、显式0清除和非零对齐覆盖的区别；OTP/PTP及完整路径选项独立使用本次选择。正式R2四配置实际CL/Link严格WX零警告，x64/ASAN各73场景及独立字节、loader、CLI和后台worker核验通过，每架构43条完整逻辑流及1525次DataSource读取偏移/长度通过；大源仅边界抽样，不称全流验证。首轮产品缺标准头声明及模型错误码预期失败原件保留，最终源码已重新构建和验证。本轮没有桌面、原版或实体光驱操作，未耗新盘；后缀恢复、其它描述/非整扇区、ANSI输出、队列和实体多层/完整GUI仍待后续，DVD继续暂缓。

第六十五阶段补齐Tools MDS的ANSI另存为选择，raw和.DVD输入共用本次编码选项，旧API与窗口默认Unicode保持。按原版完整ACP转换后只写UTF16码元数加1的固定前缀，多字节截断与生成后的来源可用性分别验证；转换结果短于请求的未知尾字节域在发布目标前明确拒绝。四配置首次CL/Link严格WX零警告，x64/ASAN各48场景、34条完整逻辑流及旧Unicode完整字节对照通过，新增静态原版字节链与源审；ANSI无新增原版运行样本或可见SaveAs操作。本轮只有小源，无大稀疏夹具、桌面、光驱或新耗材；队列/完整设置、非整扇区及其它原版缺口继续推进，DVD实盘暂缓。

第六十六阶段接通Tools .DVD单文件门槛下的自动来源恢复，按顺序元信息计数和三组候选查找，保留原版扩展名大小写及第一现存文件优先级。第二组替换后缀、第三组给完整记录名追加后缀，不混淆两者；空或非整扇区候选选中后明确失败，不改选其它文件。正式R2四配置CL/Link严格WX零警告，x64/ASAN各59场景、47完整流和29项候选查询顺序独立核对通过，旧ANSI/Unicode完整byte控制保持。首轮C4701编译失败原件保留，显式初始化后最终源码重新编译验证；本轮仅小源、无桌面/原版/光驱或新耗材。交互查找、其它描述和非整扇区/队列等原版缺口继续后续，DVD实盘暂缓。

第六十七阶段接通Tools LST/IMAGELIST文件列表：按物理尺寸和未Trim首行识别，描述目录定位整卷来源，单项缺失时仅尝试一次同主干后缀恢复。首项已识别LST只展开它并忽略后续选择；未识别回到整组raw，已识别的坏正文不回退。正式四配置CL/Link严格WX零警告，x64/ASAN各71场景及40条完整逻辑流通过，512卷、编码、候选查询、旧ANSI/Unicode字节控制及原子目标保护均独立核对。电脑中断后只读确认源码、构建和模型结果完整，沿已有通过结果收尾，未重跑native或操作光盘。本轮无桌面、原版运行、光驱或新耗材；其它描述、非整扇区、队列及实体多层/完整可见GUI继续分别推进，DVD实盘暂缓。

第六十八阶段接通Tools GI外置整卷列表，保留原版两次读取的独立字段位置选择、ACP名称剥目录、不Trim、空项跳过、单项一次恢复及首项识别优先级。正式四配置CL/Link严格WX零警告，x64/ASAN各78场景、41条完整逻辑流及MDS字节/来源读取顺序通过独立核验，旧raw/DVD/LST完整字节控制保持。读取故障、短读、损坏表及已识别类型不回raw均有覆盖；首轮核对器遗漏预取消任务不会读取描述的预期，保留诊断后仅修核对器，未重播native或修改生产。此结果为原版静态合同和文件模型，本轮未使用桌面、运行原版或访问光驱，无新耗材；GI内嵌/PDI的数据偏移、DI等其余原版缺口及实体多层/完整可见GUI继续后续，DVD实盘暂缓。

第六十九阶段接通单卷内嵌GI和PDI的受检CD001/2048载荷，MDS保留原文件及156/304字节起点，轨道和footer只计载荷。按原版候选顺序排除更早扇区布局冲突，未知raw和多卷不强制采用头偏移；已提供布局的单卷Tools输入API另支持显式字节起点，旧零偏移路径保持。四配置CL/Link严格WX零警告，x64/ASAN各121场景及首次独立核对通过，每架构42条完整逻辑流、1406次实际来源读取逐项核对，完整容器字节与排头后的载荷分别比较，旧格式完整字节控制保持。依据为原版静态调用链和文件模型，没有原版GI/PDI新运行、可见GUI或实体多层证据；本轮无新耗材，DVD实盘继续暂缓，多卷carry/flags、更多自动识别布局及其余原版缺口继续后续。

第七十阶段接通Tools PDI整扇区多卷及首卷偏移的MDS生成：首卷跳过304字节，后卷从0开始，按完整文件名追加01、02至511，最多512卷；各有效卷须为非空整2048，非整边界的carry继续待实现。原版静态读写链确认命名、几何、footer编码和首卷偏移，原MDS读取器按实际文件重算几何；本项目输出与来源消费在该子集贯通。四配置CL/Link严格WX零警告，x64/ASAN各76场景、41条完整流及2368次实际来源读取的文件路径/偏移/长度独立核对通过，旧单卷和零偏移完整字节对照保持。首次独立核对因Unicode路径解码失败，保留全部诊断并仅修验证器UTF-8读取后通过，未重播native。此为静态与文件模型证据，本轮无桌面、原版运行、可见GUI、光驱或新耗材；DVD实盘继续暂缓，其余原版缺口与实体多层验收分别推进。
