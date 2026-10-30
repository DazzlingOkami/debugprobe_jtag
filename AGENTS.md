# AGENTS.md - 嵌入式开发指南
> 本文件是给 AI 代理 / 新开发者的项目操作手册。
> 本文件仅指导嵌入式开发相关的工作，其它开发内容不必遵守本文件。
> 本文件仅是其它开发约定的补充，如有冲突以其它约定为准。

## 文件组织结构
所有设计文档都放入到docs文件夹中。
README.md文档放在根目录。

## 工具约定
优先使用CMake构建，交叉编译器为arm-none-eabi-gcc，主机编译器为gcc

## 构建与验证
构建目录固定用仓库根目录下的 `build/`，不要另建目录（已被 .gitignore 忽略）；`build/` 不存在时先创建。

构建前先读 `.vscode/settings.json` 的 `terminal.integrated.env.*`，但**不要开启 `PICO_SDK_FETCH_FROM_GIT`**：
直接指向已检出的 SDK，并让 picotool 复用现成安装，避免 FetchContent 往仓库外写入、重复下载并现场编译。
这些变量不会跨命令保留，需与 cmake 写在同一条命令里。

macOS（2 个变量）：

    export PICO_SDK_PATH=/Users/wanggaojie/work/pico_sdk_fetch/pico_sdk-src
    export PICOTOOL_FETCH_FROM_GIT_PATH=/Users/wanggaojie/work/pico_sdk_fetch
    mkdir -p build
    cmake -S . -B build
    cmake --build build -j8

Windows（1 个变量；picotool 用仓库内 `tools/picotool`，由 CMakeLists.txt 的 `picotool_DIR` 指定）：

    set PICO_SDK_PATH=D:/code/pico-sdk
    if not exist build mkdir build
    cmake -S . -B build
    cmake --build build -j8

- **不要用 `PICO_SDK_FETCH_FROM_GIT`**：`bsp/pico_sdk_import.cmake` 在没有 `PICO_SDK_PATH` 时会走 FetchContent，向 `$PICO_SDK_FETCH_FROM_GIT_PATH/pico_sdk-subbuild` 写入并重新下载整个 SDK；路径不可写时直接以 `Operation not permitted` 配置失败。
- `PICOTOOL_FETCH_FROM_GIT_PATH` 指向的目录下需已有 `picotool/`（内含可执行文件与 `picotoolConfig.cmake`），否则 SDK 会退化为联网下载并现场编译 picotool。
- `Preload.cmake` 强制使用 `Unix Makefiles` 生成器。
- **新增文件必须重新配置 cmake**：`driver/CMakeLists.txt` 用 `file(GLOB driver/*/*.c)` 收集源文件且没有 `CONFIGURE_DEPENDS`，在 `driver/` 下新增目录或 `.c` 文件后，不重新执行 `cmake -S . -B build` 就不会参与编译；改了 `CMakeLists.txt` 同理。
- 只改源码、不增删文件时可跳过配置，直接增量编译：`cmake --build build -j8`
- 产物为 `build/xxx.elf/.bin/.hex/.uf2`；链接结束会打印 FLASH/RAM 占用。默认 `RelWithDebInfo` + `PICO_COPY_TO_RAM`，代码与常量在 SRAM 中执行，RAM 占用约 198 KB（512 KB 的 38%），新增全局变量时要留意。
- 只复查个别文件的告警：`touch <文件> && cmake --build build -j8`，再从输出里筛 `warning` / `error`。
- 改动可提交的标准：`cmake --build build` 退出码为 0 且没有新增告警（对应 Git 规范的「保证构建的连续性」）。

## 版权信息
如果未在其它地方声明版权信息，则按照MIT开源协议声明。

## 代码风格
c代码默认使用c99语法，4空格缩进，小写加下划线的命名规则
代码注释统一使用英文，不得出现中文注释
代码注释默认使用`/*  */`，尽量不使用双斜杠
代码注释要简单清晰，禁止在某些小的改动中进行长篇大论
修改代码或文档时，禁止提及历史数据。比如说将3改为2，禁止这样的注释说明。
文档中不要去写xxx已确认，直接写结论即可。
文档中不需要标注太多的文档版本信息。
## Doxygen注释规范
全部使用 `/** */` 块注释，标签统一用 `@`，参数强制标注 `[in]/[out]/[inout]`
不要在注释里放复杂 Markdown；长描述尽量简洁，**@brief 一句话概括功能**
新创建的代码文件需要文件头，至少包括@file @brief
结构体、联合体、枚举至少需要标注 @brief
在头文件中的函数声明需要Doxygen注释，C文件文件中的函数注释可选。内容包括@brief @param @return @retval

## 架构与约定
禁止使用C库的malloc分配内存，主机上的cmoke除外。如果已提供了C库外的内存分配器则允许使用。
禁止在栈上分配较大的数组或结构体
禁止使用递归调用，可控且有界的递归除外，尽量避免使用

## 测试规范
代码使用cmock方法进行主机测试。
无法使用cmock在主机上自动完成的测试项目则编写测试用例文档，包括测试方法和预期结果，指导用户测试。

## Git规范
提交信息用中文，格式：`<类型>：<一句话描述>`
- 类型前缀：
  - `修复：` bug 修复（描述具体症状与修复要点，可用 `；` 连接多个点）
  - `重构：` 结构调整（如移动源码、整理文档）
  - `优化：` 性能/体验改进
  - `文档：` 仅文档变更
  - `新增：` 新增文件或功能
  - `初始提交：` 仓库首个提交
- 提交粒度：一个逻辑变更一个提交；不要混入无关改动。
- 尽量保证构建的连续性。
