# cmp-rebar-shim

一个 Linux 树外内核模块：在 `nvidia.ko` probe 之前，重排 NVIDIA CMP 170HX 前面
的 PCIe 预取窗口，给每张卡 **64 GiB BAR1 + 32 MiB BAR3**——只需 initramfs 里一个
模块，不 patch 内核、不做自制内核镜像。

实验性项目，会改 PCI 桥窗口和厂商 GPU 寄存器，先看[安全说明](#安全与限制)。
AI 协助编写，真实硬件实测通过（见 [`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md)）。
仓库里的地址、总线号、大小、参数都来自一台实测机器，是**示例不是标准**。与
NVIDIA 无关，GPL-2.0。

## 解决什么问题

GPU 挂在 PCIe 交换机下（实测为 PEX88096）时，枚举只给 64 MiB BAR1。放大 BAR1
必须同时放大前面每一级桥窗口，而原内核做不到：桥窗口尺寸计算忽略了 64 GiB BAR
的对齐，且子窗口变大后根预取窗口永不重算。于是放大失败，凡是**必须**要这个大
BAR 的驱动都会在 probe 卡死：

| 现象 | 原因 |
|---|---|
| `NVRM: ... failed to execute Booter: status 0x65`、`RmInitAdapter failed!`、`nvidia-smi` 卡 D 状态 | [cmpunlocker](docs/CMPUNLOCKER.md) `--p2p` 驱动以 64 MiB BAR1 probe，GSP 初始化失败 |
| `cuDeviceCanAccessPeer()` = 0，`nvidia-smi topo -p2p` = `GNS` | 驱动保留 64 MiB 动态 BAR1，P2P 不可用 |

给内核 PCI 核心打补丁能根治，但要为每次内核更新维护自编译内核，在 fnOS / 飞牛OS
这类只发预编译裁剪内核的系统上根本走不通。

本 shim 从外部达成同样的最终状态：一个模块，在任何驱动 probe 前从 initramfs 加载，
只用原内核导出的 PCI 资源 API。装好后每张卡 BAR1 = 64 GiB、BAR3 = 32 MiB，驱动
打出 `NVRM: CMP BAR1: final size = 65536 MB`，P2P 用真实 64 MiB `cuMemcpyPeer`
全方向验证通过（[`tools/p2p_test.py`](tools/p2p_test.py)）。

shim **不做**：解锁显存、实现 BAR1-P2P（都归 cmpunlocker），也不是通用 ReBAR
工具——窗口大小和 PCFG 偏移只针对 CMP 170HX 和该交换树。

## 安装（配合 cmpunlocker `--p2p`）

cmpunlocker 安装器会重建 initramfs，所以本 shim 要**在它之后**装。启动时 shim 必须
先于 NVIDIA 加载；若 `/etc/initramfs-tools/modules` 里 `nvidia` 排在 shim 前面，安装
脚本会拒绝。

```bash
# 1. 先装 cmpunlocker --p2p（用哪个仓库、参数含义见 docs/CMPUNLOCKER.md）
cd /path/to/cmpunlocker
sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-iommu

# 2. 再装本 shim
cd cmp-rebar-shim
make
sudo ./scripts/install.sh

# 3. 冷重启（断电再上电）
# 4. 验证
sudo ./scripts/verify.sh
```

第 2 步会：把 `.ko` 装到 `/lib/modules/$(uname -r)/extra/`，写
`/etc/modprobe.d/` 的 `options cmp_rebar_shim mode=1 commit=1`，把模块追加到
`/etc/initramfs-tools/modules`，再 `depmod` + `update-initramfs`。手动与 dracut
等价命令见 [`docs/INSTALL.md`](docs/INSTALL.md)。

若先装了 shim、后装 cmpunlocker，请在其之后重跑 `sudo ./scripts/install.sh`。若
NVIDIA 已以 64 MiB BAR1 加载，靠 initramfs 在**下次**启动让 shim 先跑，而不是往
运行中的驱动上再 insmod。

## 先试（不写硬件）

```bash
make

sudo insmod ./cmp_rebar_shim.ko mode=0   # probe：GPU、BAR、ReBAR 能力、桥窗口
sudo insmod ./cmp_rebar_shim.ko mode=2   # 资源测试：384 GiB 窗口放得下吗（纯内存）
sudo insmod ./cmp_rebar_shim.ko mode=1   # dry-run：每座桥的计划 + 根窗口容纳检查
dmesg | tail -80
sudo rmmod cmp_rebar_shim
```

dry-run 重点看 `root window`、`top needs`（实测 3 卡为 384 GiB）、
`top allocation test: ret=0`、每卡 `-> 64G/32M`。计划不对就别装，见
[`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)。

仅当 NVIDIA 未加载时可手动 commit：

```bash
sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia 2>/dev/null || true
sudo insmod ./cmp_rebar_shim.ko mode=1 commit=1   # 应见 "REBAR ctrl now ..." 和 64G BAR
sudo modprobe nvidia && sudo modprobe nvidia-uvm
```

shim 失败就别加载 NVIDIA，冷重启。

## 验证

```bash
sudo ./scripts/check_boot_logs.sh   # 只读日志，不调 nvidia-smi
python3 tools/p2p_test.py           # 真实 CUDA P2P 拷贝
```

健康冷启动：所有 `cmp_rebar_shim` 行**先于** `NVRM` 出现，随后
`NVRM: CMP BAR1: final size = 65536 MB`。你机器上地址会不同，以**大小**和这个
顺序为准。`CMP BAR1: keeping stock size` 在有 `RMForceStaticBar1=1` 时是良性的，
以 `final size` 为准。完整期望输出见 [`docs/VERIFY.md`](docs/VERIFY.md)。

## 工作原理

`mode=1 commit=1` 时，在 root 挂载和 NVIDIA probe 之前，shim：

1. 找到 CMP 卡（`10de:20c2` / `10de:2082`），向上走到交换机顶部桥；
2. 计算每座桥需要的预取窗口（每卡 64 GiB + 32 MiB），检查固定 384 GiB 顶层窗口
   （起始 64 GiB）能放进根预取窗口；
3. 每张卡：强制 D0、等 PCFG 就绪、经 BAR0 MMIO 解锁 ReBAR（`CYA=0x30`、
   `BAR1_CONFIG=0x8000000a`、size 16），把 BAR1/BAR3 改成 64 GiB / 32 MiB；
4. 释放旧子窗口、写入新桥尺寸、插入顶层窗口、`pci_bus_assign_resources()`、写桥
   寄存器、恢复 memory decode。

只动预取窗口和 CMP 设备，不改内核、发行版预编译模块和 NVIDIA 驱动。细节见
[`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md)。

## 布局（实测树）

根预取窗口 `0xe40000000-0x7fffffffff`；shim 使用顶层窗口
`0x1000000000-0x6fffffffff`（384 GiB），位于 iGPU 孔径
`0x7fd0000000-0x7fe01fffff` 之下。

| 总线 | 窗口 |
|---|---:|
| 01 / 02 | 384 GiB（CMP 交换树顶部） |
| 03 / 04 | 256 GiB（GPU 05/06 分支） |
| 07 / 08 | 128 GiB（GPU 09 分支） |
| 05 / 06 / 09 | 64 GiB + 32 MiB（每总线一张 CMP） |

最终 BAR：

| GPU | BAR1 | BAR3 |
|---|---|---|
| `05:00.0` | `0x1000000000-0x1fffffffff` | `0x2000000000-0x2001ffffff` |
| `06:00.0` | `0x3000000000-0x3fffffffff` | `0x4000000000-0x4001ffffff` |
| `09:00.0` | `0x5000000000-0x5fffffffff` | `0x6000000000-0x6001ffffff` |

实测环境：3x CMP 170HX（`10de:20c2`）、PEX88096、fnOS `6.18.18.c1107-trim`
（x86_64，gcc 12.2.0）、cmpunlocker `--p2p` `nvidia.ko` 610.57.04、
`amd_iommu=on iommu=pt pcie_aspm=off`。完整配置与日志见
[`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md)。

## 安全与限制

- 会改桥窗口和 GPU BAR；拓扑不符、顶层窗口被占、PCFG 偏移不对都可能卡死到冷
  重启。先在非生产机测试，并留恢复路径（GRUB 里旧内核、live USB，或能从
  initramfs 移除 shim）。
- commit 前先跑 dry-run（`mode=1`）和资源测试（`mode=2`）。
- 写死的假设：每卡 64 GiB BAR1 + 32 MiB BAR3、64 GiB 起点的固定 384 GiB 顶层窗口
  （根窗口必须含它）、CMP 170HX（TU102）的 PCFG 偏移。别的硬件需改。
- 无 DKMS，内核升级后要重编。实测用 `initramfs-tools`。

## 仓库结构

```text
cmp_rebar_shim.c       模块源码
Makefile               kbuild 封装
scripts/install.sh     安装模块 + modprobe + initramfs
scripts/uninstall.sh   卸载以上
scripts/check_boot_logs.sh  只读启动日志诊断
scripts/verify.sh      BAR 检查 + CUDA P2P 传输测试
tools/p2p_test.py      CUDA P2P 测试（ctypes，无需 nvcc）
docs/                  安装、顺序、原理、验证、排错
etc/                   示例配置
```

## 贡献

本项目由 AI 协助。若在你机器上跑不通，把本仓库连同你的日志（`dmesg`、`lspci -tv`、
`/proc/iomem`、`mode=1` dry-run 输出）交给 AI，让它按你的拓扑改窗口大小、寄存器偏移
和顶层窗口位置，再把改动和前后日志开 PR 提回来。详见
[`CONTRIBUTING.md`](CONTRIBUTING.md)、[`CHANGELOG.md`](CHANGELOG.md)。

## 许可

GPL-2.0，见 [`LICENSE`](LICENSE)。NVIDIA、CMP 及相关标识为 NVIDIA 公司商标；
本项目与 NVIDIA 无关联、亦未获其背书。
