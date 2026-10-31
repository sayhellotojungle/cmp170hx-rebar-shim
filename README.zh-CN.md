# cmp-rebar-shim（中文说明）

一个很小的 Linux 树外内核模块：在 NVIDIA 专有驱动 probe 之前，重新排布
CMP 170HX 前面的 PCIe 预取窗口，给每张卡 **64 GiB BAR1 + 32 MiB BAR3**。
不需要 patch 内核，不需要自制内核镜像——initramfs 里的一个模块就够了。

> 研究/折腾项目。会改 PCI 桥窗口和厂商专用 GPU 寄存器，请先看安全说明。
> 与 NVIDIA 无关。GPL-2.0。

> **本项目由 AI 协助编写。** 代码与文档由 AI 辅助生成，并在真实硬件上端到端
> 实测通过（见 [`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md)）。

> **关于仓库里的具体数值。** README 和 `docs/` 中出现的总线号、地址、窗口
> 大小、寄存器偏移、安装参数，全部来自一台实测机器（`docs/TESTED-CONFIG.md`），
> 只是**示例和日志样本，不是标准**。本项目固定不变的目标只有一个：在
> `nvidia.ko` probe 之前，每张 CMP 170HX 拿到 64 GiB BAR1 + 32 MiB BAR3。
> 其余都取决于你的固件、交换机拓扑和发行版。务必先跑 dry-run，并以**你自己的**
> `dmesg` 输出为准。

## 解决什么问题

**核心问题。** GPU 挂在 PCIe 交换机下面的机器（实测树是 PEX88096），枚举
注册时每张 GPU 只分到 64 MiB 的 BAR1，而 Resizable BAR 救不了：放大 BAR
必须同时放大 GPU 前面每一级桥窗口，原内核没有能力重算这棵窗口树——

- 桥窗口尺寸计算没有正确考虑子 BAR 的对齐（64 GiB 大小且 64 GiB 对齐的
  BAR 需要比内核预留的更大的窗口）；
- 子窗口变大后，顶层预取窗口永远不会被重算。

于是 ReBAR 放大失败。而有些驱动**必须**要有这个大 BAR——
[cmpunlocker](docs/CMPUNLOCKER.md) 的 `--p2p` `nvidia.ko` 需要 64 GiB 静态
BAR1 才能映射解锁后的 64 GB 显存并做 BAR1-P2P——失败就会级联成 probe 卡死：

| 现象 | 原因 |
|---|---|
| `NVRM: ... failed to execute Booter: status 0x65`、`RmInitAdapter failed!`，`nvidia-smi` 卡死成 D 状态 | `--p2p` 驱动以 64 MiB BAR1 probe，GSP 初始化失败 |
| `cuDeviceCanAccessPeer()` 返回 0，`nvidia-smi topo -p2p` 显示 `GNS` | 驱动保留 64 MiB 动态 BAR1，P2P 不可用 |

**为什么用 shim 而不是 patch 内核。** 直接的修法是给内核 PCI 核心打补丁，
让它会重算预取窗口树。但那个方案太重：要为每个内核更新重编自维护内核、要
一套编译环境；更关键的是，在飞牛OS（fnOS）这类只发预编译裁剪内核、根本
不提供内核开发路径的系统上，这条路**完全走不通**。

本 shim 从外部达成同样的最终状态：一个小模块，在任何驱动 probe 之前从
initramfs 加载，只使用**原内核**导出的资源 API。不需要内核源码、不打补丁、
不做镜像——在能自编内核的发行版上同样适用。装好后：

- 每张卡 BAR1 = 64 GiB、BAR3 = 32 MiB，交换机桥窗口按新布局重排；
- 驱动打出 `NVRM: CMP BAR1: final size = 65536 MB`，GSP 正常初始化；
- P2P 用真实 64 MiB `cuMemcpyPeer` 全方向拷贝验证通过
  （[`tools/p2p_test.py`](tools/p2p_test.py)）。

本 shim **不做**的事：

- 不解锁 64 GB 显存（cmpunlocker 的职责）；
- 不实现 BAR1-P2P（cmpunlocker `--p2p` 的职责）；
- 不是通用 ReBAR 工具：窗口大小和 PCFG 寄存器偏移只针对 CMP 170HX 和测试树。

## 使用方法

### 与带 `--p2p` 的 cmpunlocker 一起用（已验证路径）

安装顺序和启动顺序都很重要。cmpunlocker 安装器会重建 initramfs，所以本 shim
要**在它之后**装。启动时 shim 必须从 initramfs 先于 NVIDIA 驱动加载；安装
脚本会强制检查这一点（若 `/etc/initramfs-tools/modules` 里 `nvidia` 排在
shim 前面会直接拒绝安装）。

```bash
# 1. 先装带 --p2p 的 cmpunlocker（第三方，见 docs/CMPUNLOCKER.md）
#    下面的参数只是实测机器用过的示例，请以你的 cmpunlocker 版本文档为准
cd /path/to/cmpunlocker
sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-gen2-service --no-iommu

# 2. 再装本 shim
cd cmp-rebar-shim
make
sudo ./scripts/install.sh

# 3. 冷重启（断电再上电，不是普通重启）

# 4. 验证
sudo ./scripts/verify.sh        # shim 已加载 + BAR 大小 + 真实 CUDA P2P 拷贝
```

第 2 步会（每一步的手动等价命令、dracut 写法见
[`docs/INSTALL.md`](docs/INSTALL.md)）：

- 把 `cmp_rebar_shim.ko` 装到 `/lib/modules/$(uname -r)/extra/`；
- 写 `/etc/modprobe.d/cmp-rebar-shim.conf`：`options cmp_rebar_shim mode=1 commit=1`；
- 把 `cmp_rebar_shim` 追加到 `/etc/initramfs-tools/modules`；
- 执行 `depmod` 和 `update-initramfs -u -k $(uname -r)`。

注意：

- 如果你先装了 shim、后装 cmpunlocker，请在 cmpunlocker 之后重新执行
  `sudo ./scripts/install.sh`，再冷重启。
- 如果 NVIDIA 已经以 64 MiB BAR1 加载，再 insmod shim 救不回本次启动；
  必须冷重启让 shim 先跑。
- 完整顺序原理、参数、坑：[`docs/CMPUNLOCKER.md`](docs/CMPUNLOCKER.md)。

### 安装前：先看计划（不写任何东西）

```bash
make

# probe：只读列出 GPU、BAR、ReBAR 能力、桥窗口
sudo insmod ./cmp_rebar_shim.ko mode=0
dmesg | tail -80
sudo rmmod cmp_rebar_shim

# 资源测试：只看 384 GiB 窗口放不放得下（纯内存操作）
sudo insmod ./cmp_rebar_shim.ko mode=2
dmesg | tail -40
sudo rmmod cmp_rebar_shim

# dry-run：打印真实布局的每座桥的计划，并验证固定顶层窗口能放进根预取窗口；
# 不写任何硬件
sudo insmod ./cmp_rebar_shim.ko mode=1
dmesg | tail -80
sudo rmmod cmp_rebar_shim
```

dry-run 日志重点检查：`root window`、`top needs`（按卡数合计，测试树 3 卡
是 384 GiB）、`top allocation test: ret=0`、每卡的 `-> 64G/32M`。计划不符合
预期就不要装，见 [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)。

手动 live 测试（仅当 NVIDIA 未加载时）：

```bash
sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia 2>/dev/null || true
sudo insmod ./cmp_rebar_shim.ko mode=1 commit=1
dmesg | tail -60          # 必须看到 "REBAR ctrl now ..." 和 64G BAR
sudo modprobe nvidia
sudo modprobe nvidia-uvm
```

shim 失败时不要加载带 P2P dwords 的 NVIDIA 驱动（会以 64 MiB BAR1 初始化
GSP 并失败），冷重启即可。

## 验证

只读日志（不调用 `nvidia-smi`）：

```bash
sudo ./scripts/check_boot_logs.sh
```

健康冷启动应看到（地址是实测机器的示例，你机器上会不同；以**大小**和
shim 先于 NVIDIA 的顺序为准）：

```text
cmp_rebar_shim: 0000:05:00.0: REBAR cap locked (0x400), unlocking
cmp_rebar_shim: 0000:05:00.0: REBAR ctrl now 0x1001 (size=16)
cmp_rebar_shim: verify 0000:05:00.0 BAR1 0x1000000000-0x1fffffffff len=0x1000000000 ...
NVRM: CMP BAR1: final size = 65536 MB
```

`NVRM: CMP BAR1: keeping stock size ...` 在 `EnableResizableBar: 1` 时也可能
出现，因为 `RMForceStaticBar1=1` 会让驱动保留 shim 设好的静态窗口；以紧接着的
`final size = 65536 MB` 为准。

真实 P2P 传输：

```bash
python3 tools/p2p_test.py      # 或由 sudo ./scripts/verify.sh 调用
```

## 已验证环境

详见 [`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md)：

- 3x NVIDIA CMP 170HX (`10de:20c2`)，64 GB 显存解锁
- Broadcom PEX88096 PCIe switch
- fnOS 内核 `6.18.18.c1107-trim`，gcc 12.2.0
- cmpunlocker（`--p2p`）的 `nvidia.ko` 610.57.04
- `amd_iommu=on iommu=pt pcie_aspm=off`
- 冷启动后 shim 解锁 BAR1 -> NVIDIA GSP 正常初始化 -> CUDA P2P 六向
  64 MiB `cuMemcpyPeer` 全部通过

## 工作原理（简述）

shim 从 initramfs 加载，发生在 root 挂载和 udev 加载 `nvidia.ko` 之前。
`mode=1 commit=1` 时：找到 CMP 卡并走到交换机顶部桥；计算每座桥需要的
预取窗口并验证固定 384 GiB 顶层窗口（起始 64 GiB）能放进根预取窗口；对每张
卡强制 D0、通过 BAR0+PCFG MMIO 解锁 ReBAR（`CYA=0x30`、
`BAR1_CONFIG=0x8000000a`、ReBAR size=16 即 64 GiB），把 BAR1/BAR3 资源改成
64 GiB/32 MiB；再释放旧子窗口、写入新桥尺寸、插入顶层窗口、
`pci_bus_assign_resources()`、写桥寄存器、恢复 memory decode 并校验。

不修改内核镜像、发行版预编译模块和 NVIDIA 驱动；只用导出的 PCI 资源 API 和
BAR0 MMIO。细节见 [`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md)。

## 安全与限制

- 会改 PCI 桥窗口和 GPU BAR；拓扑不符、顶层固定窗口被占、PCFG 偏移不对都
  可能卡死，需要冷重启恢复。
- shim 只重排**预取**窗口、只动 CMP 设备；测试树里交换机下的 NVMe
  （`08:10.0 -> 0a:00.0`）不受影响。但设 `commit=1` 前仍要先核对你的拓扑。
- 先跑 dry-run（`mode=1` 不加 `commit=1`）和 `mode=2` 资源测试。
- PCFG 偏移、64 GiB/32 MiB、固定 384 GiB 顶层窗口（64 GiB 起）是按
  CMP 170HX + PEX88096 测试树写的；其他拓扑需要改。
- 不负责显存解锁，也不实现 P2P（都归 cmpunlocker）。
- 无 DKMS，内核升级后要重编（`make clean && make && sudo ./scripts/install.sh`）。
- 无保修，GPL-2.0。

## 贡献（AI 协助）

本项目由 AI 协助编写，并在真实环境验证过。但环境千变万化，如果你机器上跑不通，
推荐的做法是：把本仓库连同你的日志（`dmesg`、`lspci -tv`、`/proc/iomem`、
`mode=1` dry-run 输出）一起交给 AI，让它直接读代码、按你的拓扑调整窗口大小、
寄存器偏移和顶层窗口位置，再把改动和前后的启动日志开一个 PR 提回来一起完善。
非常欢迎。详见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。

完整英文文档见 [`README.md`](README.md)。
