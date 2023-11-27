[![icon](/data/pixmaps/idbutton.png?raw=true)](https://www.darktable.org/) darktable [![GitHub Workflow Status (branch)](https://img.shields.io/github/actions/workflow/status/darktable-org/darktable/ci.yml?branch=master)](https://github.com/darktable-org/darktable/actions/workflows/ci.yml?query=branch%3Amaster+is%3Acompleted+event%3Apush) [![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/470/badge)](https://bestpractices.coreinfrastructure.org/projects/470) 
=========

darktable是一款开源摄影工作流程应用和非破坏性原始开发工具，是摄影师的虚拟暗房和放大机。它通过数据库管理您的数字底片，让您通过可缩放的光照图查看它们，并使您能够开发原始图像、增强它们并将它们导出到本地或远程存储。

![screenshot_lighttable](https://user-images.githubusercontent.com/45535283/148689197-e53dd75f-32f1-4297-9a0f-a9547fd4e7c7.jpg)

darktable **不是** 免费的Adobe®Lightroom®替代品。

[https://www.darktable.org/](https://www.darktable.org/ "darktable homepage")

## 目录

1. [文档](#文档)
2. [官网](#官网)
3. [运行要求](#运行要求)
   - [支持平台](#支持平台)
   - [硬件](#硬件)
4. [安装](#安装)
   - [最新发行版](#最新发行版)
   - [开发快照](#开发快照)
5. [从旧版本升级](#从旧版本升级)
6. [获取插件](#获取插件)
7. [构建](#构建)
   - [依赖项](#依赖项)
   - [获取源代码](#获取源代码)
   - [获取子模块](#获取子模块)
   - [编译](#编译)
   - [深入阅读](#深入阅读)
8. [使用](#使用)
   - [测试版/非稳定版](#测试版/非稳定版)
   - [常规/稳定版本 ](#常规/稳定版本 )
9. [贡献](#贡献)
10. [FAQ](#faq)
   - [我的相机插上后为什么没有被检测到?](#我的相机插上后为什么没有被检测到?)
   - [我的镜头在Darktable中为什么没有被检测/纠正?](#我的镜头在Darktable中为什么没有被检测/纠正?)
   - [在Lighttable视图中，缩略图与在Darktable视图中预览看起来不一样的原因是什么？](#在Lighttable视图中，缩略图与在Darktable视图中预览看起来不一样的原因是什么？)
11. [Wiki](#wiki)
12. [邮件列表](#邮件列表)

文档
-------------

Darktable用户手册在 [dtdocs](https://github.com/darktable-org/dtdocs) 仓库中维护。

Lua API文档在 [luadocs](https://github.com/darktable-org/luadocs) 仓库中维护。

官网
-------

网站([https://www.darktable.org/](https://www.darktable.org/))在[dtorg](https://github.com/darktable-org/dtorg)存储库中维护

运行要求
------------

### 支持平台

* Linux (64-bit)
* FreeBSD (64-bit)
* Windows (64-bit), 8.1 w/ [UCRT](https://support.microsoft.com/zh-cn/topic/update-for-universal-c-runtime-in-windows-c0514201-7fe6-95a3-b0a5-287930f3560c) 及以上
* macOS 11.3 及以上

*不支持大端平台*

*32位平台不被正式支持-它们可能工作，也可能不工作。*

*Windows 支持尚不成熟，存在不影响 Linux 的错误。如果可能的话，更推荐在 Linux 上使用 Darktable。*

### 硬件

（可运行最低值/**推荐**最低值）：
* 内存: 4 GB / **8 GB**
* CPU: Intel 奔腾(Pentium) 4 (双核) / **Intel 酷睿（Core） i5 4×2.4 GHz**
* 显卡: 无/**Nvidia CUDA核心1024，4GB，兼容OpenCL 1.2**
* 可用磁盘空间：250 MB/**1 GB**

*Darktable可以在轻量级配置上运行（甚至在树莓派上），但请注意，像降噪、局部对比度、对比度均衡器、修饰或液化等模块可能会变得非常慢，无法使用。*

*GPU不是强制性的，但是强烈建议使用以获得更流畅的体验。建议使用Nvidia GPU以保证安全，因为一些AMD驱动程序在某些模块（例如局部对比度）上表现不可靠。*

安装
----------

如果最新版本仍然无法作为您分发的预构建软件包提供，您可以按照[以下](#构建)说明自行构建该软件。

### 最新发行版

4.4.2 (stable)

* [下载Windows可执行文件](https://github.com/darktable-org/darktable/releases/download/release-4.4.2/darktable-4.4.2-win64.exe)
* [下载macOS(Intel)可执行文件](https://github.com/darktable-org/darktable/releases/download/release-4.4.2/darktable-4.4.2-x86_64.dmg)
* [下载macOS(Apple Silicon)可执行文件](https://github.com/darktable-org/darktable/releases/download/release-4.4.2/darktable-4.4.2-arm64.dmg)
* [为Linux发行版安装原生软件包或添加第三方存储库](https://software.opensuse.org/download.html?project=graphics:darktable:stable&package=darktable)
* [安装Linux的Flatpak软件包](https://flathub.org/apps/details/org.darktable.Darktable)
* [其他操作系统](https://www.darktable.org/install/)

*当使用预编译的包时，请确保它已使用Lua，OpenCL，OpenMP和Colord进行构建。 这些是可选的，如果丢失了它们，将不会阻止darktable运行，但是缺少它们会降低用户体验。 可以通过运行带有 `--version` 命令行选项的darktable来检查它们。*

### 开发快照

该开发快照反映了主分支的当前状态。它用于测试，通常不安全。请参阅下面的注意事项，了解使用主分支的警告和注意事项。

* [安装 Linux 本机软件包和存储库](https://software.opensuse.org/download.html?project=graphics:darktable:master&package=darktable) (每日更新).
* [为 Linux（AppImage）、macOS 和 Windows 提供了二进制包，这些包每晚都会更新](https://github.com/darktable-org/darktable/releases/tag/nightly) (仅x86_64架构).

从旧版本升级
----------------------------

当从旧版本更新darktable时，您只需要安装最新版本。
现有的文件将被保留。

然而，新版本偶尔需要更改库数据库的结构（包含darktable已知的全部图像列表及其编辑历史记录）。如果出现这种情况，您将会收到提示，要求升级数据库或关闭软件。

**迁移到一个较新的数据库结构/版本意味着您的编辑（包括新编辑和旧编辑）将不再与较旧版本的darktable兼容。**升级是明确的。较新版本始终与较旧的编辑兼容，但较新的编辑通常与较旧版本不兼容。

darktable会在新版本升级时自动备份库数据库(例如在`~/.config/darktable/library.db-pre-3.0.0`中)，因此如果需要，您可以通过还原此备份来恢复到之前的版本（只需将其重命名为`library.db`）。

如果您尝试使用旧版本的软件打开较新的数据库，使用新功能进行的任何编辑部分将被丢弃，并且您将丢失它们。这也适用于附带 XMP 文件。

如果您计划在两个版本（新/不稳定和旧/稳定）之间定期移动，请参阅下面的详细信息，了解如何安全地执行此操作。

获取插件
--------------------

扩展和插件使用Lua脚本语言，可以在[这里](https://github.com/darktable-org/lua-scripts)下载。在darktable中，Lua支持是可选的，因此请确保在构建时您的系统上安装了Lua解释器及其开发文件（安装包`lua-dev` 或 `lua-devel`，具体取决于发行版），或者确保您正在使用的包已经使用此库构建。

扩展程序允许导出各种媒体和网站，合并/堆叠/混合HDR，全景或焦点包围，应用基于AI的面部识别，管理标签和GPS数据等。

构建
--------

### 依赖项

兼容编译器/工具链：
* Clang: 12 及以上
* GCC: 12 及以上
* MinGW-w64: 10 及以上
* XCode: 13.2 及以上

必要依赖项（最低版本）：
* CMake 3.18
* GTK 3.24.15
* GLib 2.40
* SQLite 3.15 *(但是强烈建议使用3.24或更高版本)*
* Exiv2 0.25 *(但至少需要使用 ISO BMFF 支持构建的 0.27.4，才能导入佳能 CR3 原始格式。)*

必要依赖项（无版本要求）：
* Lensfun(用于自动镜头纠正) (注：不支持 alpha 0.3.95 和 git master 分支)
* Little CMS 2

可选依赖项（最低版本）：
* OpenMP 4.5 *（适用于CPU多线程和SIMD矢量化）*
* LLVM 3.9 *（用于编译时进行OpenCL检查）*
* OpenCL 1.2 *（用于显卡加速计算）*
* Lua 5.4 *（适用于插件和扩展脚本）*
* libgphoto2 2.5 *（用于相机远程连接）*
* Imath 3.1.0 *（适用于16位“半”浮点TIFF导出和更快的导入）*
* libavif 0.9.2 *（用于AVIF的导入和导出）*
* libheif 1.13.0 *（用于HEIF/HEIC/HIF导入；如果没有libavif，也用于AVIF导入）*
* libjxl 0.7.0 *(用于JPEG XL导入和导出)*
* WebP 0.3.0 *（用于WebP的导入和导出）*

可选依赖项（无版本要求）：
* colord、Xatom *（用于获取系统显示颜色配置文件）*
* G'MIC *（用于.gmz压缩LUT支持）*
* PortMidi *（用于MIDI输入支持）* 
* SDL2 *（用于游戏手柄输入支持）* 
* CUPS *（用于打印模式支持）* 
* OpenEXR *（用于EXR导入和导出）*
* OpenJPEG *（用于JPEG 2000导入和导出）*
* GraphicsMagick 或 ImageMagick *（用于其他图像格式的导入）*

要在Linux系统上安装所有依赖项，您可以使用您发行版的源仓库（只要它们是最新的）：

#### Fedora 和 RHEL/CentOS

```bash
sudo dnf builddep darktable
```

#### OpenSuse

```bash
sudo zypper si -d darktable
```

#### Ubuntu

```bash
sed -e '/^#\sdeb-src /s/^# *//;t;d' "/etc/apt/sources.list" \
  | sudo tee /etc/apt/sources.list.d/darktable-sources-tmp.list > /dev/null \
  && (
    sudo apt-get update
    sudo apt-get build-dep darktable
  )
sudo rm /etc/apt/sources.list.d/darktable-sources-tmp.list
```

#### Debian

```bash
sudo apt-get build-dep darktable
```

#### 安装缺失的依赖项

如果您的系统缺少强制依赖项，则软件构建将失败，并出现诸如`未找到软件包XXX`或`此系统上没有提供命令YYYY`之类的错误。如果您看到这些错误中的任何一个，您应该找出您的发行版中提供缺失软件包/命令的软件包，然后安装它。这通常可以在您的包管理器（而不是您的发行版默认提供的程序管理器）或通过互联网使用搜索引擎来完成。您可能需要首先安装一个包管理器（例如Debian/Ubuntu上的APT，或Fedora / RHEL上的DNF）。

虽然他的方法可能有些繁琐，但只需要操作一次。查看[安装 Darktable 的页面](https://github.com/darktable-org/darktable/wiki/Building-darktable)上的一行命令，这将在最常见的 Linux 分发版上安装大多数依赖项。

### 获取源代码

#### 主分支（不稳定）

主分支包含最新版本的源代码，旨在：

- 作为开发人员的工作基础,
- 为测试人员追赶缺陷,
- 对于愿意牺牲稳定性来获取新功能而不等待下一个版本的客户。

主分支不保证稳定性，可能会破坏您的数据库和XMP文件，导致数据和编辑历史丢失，或暂时破坏与以前版本和提交的兼容性。

有多危险？大多数情况下，它相当稳定。与任何滚动发布类型的部署一样，错误出现的频率更高，但修复速度也更快。然而，有时这些错误可能会导致您图片的编辑历史出现损失或不一致。如果您不需要在未来重新打开您的编辑，这没关系，但如果您管理房地产，这可能不是好事。

在备份了`~/.config/darktable`目录和你打算用主分支打开的任何图片的XMP文件后，你可以按照以下方式获取源代码：

```bash
git clone --recurse-submodules --depth 1 https://github.com/darktable-org/darktable.git
cd darktable
```

请参阅下面的“使用”部分，了解如何在不损坏常规稳定安装和文件的情况下开始测试安装不稳定版本。

#### 最新稳定版本

4.4.2

Darktable 项目每年在夏至和冬至各发布一个主要版本，并以偶数标记（例如4.0，4.2，4.4，4.6）。次要修订版以第三位数字标记（例如4.0.1，4.0.2），主要提供错误修复和相机支持。您可能希望自己编译这些稳定版本以获得更好的计算机性能：

```bash
git clone --recurse-submodules --depth 1 https://github.com/darktable-org/darktable.git
cd darktable
git fetch --tags
git checkout tags/release-4.4.2
```

### 获取子模块

请注意，[libxcf](https://github.com/houz/libxcf.git), [OpenCL](https://github.com/KhronosGroup/OpenCL-Headers.git), [RawSpeed](https://github.com/darktable-org/rawspeed), [whereami](https://github.com/gpakosz/whereami) 和 [LibRaw](https://github.com/LibRaw/LibRaw) 都是通过 git 子模块进行跟踪的，因此在签出 darktable 后，您也需要更新/签出子模块：

```bash
git submodule update --init
```

### 编译

#### 简单方法

警告：如果您之前已经构建了darktable，请不要忘记首先使用命令`rm -R`删除`build`和`/opt/darktable`目录，以避免不同版本之间的冲突文件。已报告许多奇怪的行为和短暂的错误，这可以追溯到构建缓存没有正确使更改的依赖项无效，因此最安全的方法是完全删除之前构建的二进制文件，并从头开始。

darktable 提供了一个 shell 脚本，可以在单个命令中自动处理在 Linux 和 macOS 上为经典案例的构建。

```bash
./build.sh --prefix /opt/darktable --build-type Release --install --sudo
```

如果您想在常规/稳定版本旁边安装测试版本，请更改安装前缀：

```bash
./build.sh --prefix /opt/darktable-test --build-type Release --install --sudo
```

该选项仅为您的体系结构构建软件，包括：

- `-O3`优化级别

- SSE/AVX支持（如果检测到）

- OpenMP 支持（多线程和矢量化）（如果检测到）

- OpenCL（GPU卸载）支持(如果检测到)

- Lua脚本支持（如果检测到）

如果您希望将Dartkable与其他应用程序一起显示，您只需要添加一个符号链接：

```bash
ln -s /opt/darktable/share/applications/org.darktable.darktable.desktop /usr/share/applications/org.darktable.darktable.desktop
```

现在，您定制的 Darktable 已经准备好了，就像任何预包装的软件一样可以使用了。

#### 手动方式

或者，您可以使用手动构建来传递自定义参数。

##### Linux

```bash
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/opt/darktable/ ..
cmake --build .
sudo cmake --install .
```

##### macOS

查看 [Homebrew](https://github.com/darktable-org/darktable/blob/master/packaging/macosx/BUILD_hb.txt) or [MacPorts](https://github.com/darktable-org/darktable/blob/master/packaging/macosx/BUILD.txt) instructions.

##### Windows

查看 [these instructions](https://github.com/darktable-org/darktable/tree/master/packaging/windows).

### 使用

#### 测试版/非稳定版

要在不损害常规/稳定版本的文件和数据库的情况下使用darktable的测试版本，请使用以下命令在终端中启动darktable：

```bash
/opt/darktable-test/bin/darktable --configdir "~/.config/darktable-test"
```

并确保在首选项 -> 存储 -> XMP 中将“为每个图像创建附带文件”选项设置为“从不”。 这样，常规/稳定版本将像往常一样在 `~/.config/darktable` 中保存其配置文件，测试/不稳定版本将在 `~/.config/darktable-test` 中保存，并且这两个版本不会产生数据库冲突。

#### 常规/稳定版本 

只需从桌面应用程序菜单中启动它，或者从终端中运行`darktable`或`/opt/darktable/bin/darktable`。如果安装过程中没有在应用程序菜单中创建一个启动器，则运行：

```bash
sudo ln -s /opt/darktable/share/applications/org.darktable.darktable.desktop /usr/share/applications/org.darktable.darktable.desktop
```

您可以在`~/.config/darktable`中找到darktable的配置文件。如果您在启动时遇到崩溃，请尝试在终端中禁用OpenCL并使用`darktable --disable-opencl`启动darktable。

### 深入阅读

有一份[Ubuntu/Debian相关发行版](https://github.com/darktable-org/darktable/wiki/Build-instructions-for-Ubuntu)或[Fedora及相关发行版](https://github.com/darktable-org/darktable/wiki/Build-Instructions-for-Fedora)的完整的构建指令列表。这些构建指令可以很容易地适应许多其他Linux发行版。


贡献
------------

您可以为darktable项目做出贡献的方式有很多：

* 撰写关于darktable的博客文章
* 创建darktable的教程
* 帮助扩展[用户Wiki](https://github.com/darktable-org/darktable/wiki)或[用户手册](https://github.com/darktable-org/dtdocs)
* 回答[用户邮件列表](https://www.mail-archive.com/darktable-user@lists.darktable.org/)或[pixls.us论坛](https://discuss.pixls.us/c/software/darktable/19)上的问题
* 在[开发者邮件列表](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)上分享您的想法
* 测试[版本](https://www.darktable.org/install/)
* 审核[拉取请求(Pull Requests)](https://github.com/darktable-org/darktable/pulls)
* 开始[对darktable进行黑客攻击](https://www.darktable.org/development/)，并查看[开发人员指南](https://github.com/darktable-org/darktable/wiki/Developer's-guide)


FAQ
---

### 我的相机插上后为什么没有被检测到?

为了支持最新相机，请检查您是否已安装最新的[gphoto2库](http://www.gphoto.org/ "gphoto2 homepage") 。

### 我的镜头在Darktable中为什么没有被检测/纠正?

镜头校正配置文件由Lensfun提供，它包含2个部分：程序和数据库。大多数Linux发行版都提供了足够新的程序版本，但提供了过时的数据库版本。如果[Lensfun](https://lensfun.github.io/)已正确安装，则在终端中运行以更新其数据库：

```bash
lensfun-update-data
```

或者

```bash
/usr/bin/g-lensfun-update-data
```

### 在Lighttable视图中，缩略图与在Darktable视图中预览看起来不一样的原因是什么？

对于从未在Darktable中编辑过的RAW文件（当你刚刚导入它们时），Lighttable视图默认显示相机放置在RAW文件中的JPEG预览。加载此JPEG文件更快，并在导入大量图像时使Lighttable视图更具响应性。

然而，此JPEG缩略图由相机的固件处理，具有专有算法、颜色、清晰度和对比度，可能与Darktable中处理（在Darktable视图中打开图像时所看到的）的颜色、清晰度和对比度不同。相机制造商不公布他们在其固件中执行的像素处理的详细信息，因此其外观不能完全或轻易地被其他软件复制。

但是，一旦在Darktable中对RAW图像进行编辑，Lighttable缩略图应该与Darktable预览完全匹配，因为它们以相同的方式进行处理。

如果您从不希望在Lighttable视图中看到RAW文件中的嵌入式JPEG缩略图，您应该在首选项->Lighttable中设置“从不使用来自大小的嵌入式JPEG代替RAW文件”选项。
Wiki
----

* [GitHub wiki(英文)](https://github.com/darktable-org/darktable/wiki "github wiki")
* [开发者 wiki(英文)](https://github.com/darktable-org/darktable/wiki/Developer's-guide "darktable developer wiki")


邮件列表
-------------

* 用户 [[订阅](mailto:darktable-user+subscribe@lists.darktable.org) | [存档](https://www.mail-archive.com/darktable-user@lists.darktable.org/)]
* 开发者 [[订阅](mailto:darktable-dev+subscribe@lists.darktable.org) | [存档](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)]