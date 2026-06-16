# eth2can.ko — Linux 侧 E2CF 驱动,第一阶段(EtherType 截留)

把 MCXE31B 网关的 6 路 CANFD 暴露为 6 个标准 SocketCAN 设备
`eth2can0..5`(candump/cansend/cangen、ip link 配置波特率,客户零改动)。

## 第一阶段传输方案(本目录)

按"**只截某个 EtherType,其余流量照走协议栈**"的要求:

- RX:`dev_add_pack(0x88B5)` 在指定网口上注册 packet_type 处理器 ——
  内核仅把 E2CF 帧分流给本驱动,**其余报文(SSH/NFS/...)走原协议栈,
  网卡保持原驱动**(标准 fsl-enetc4 即可,不需要 enetc_ecat 独占口)。
- TX:`dev_queue_xmit()` 直发构造好的 L2 帧(Linux→MCU 方向 T_agg=0
  直发,符合协议 §6.1 默认)。
- 路径经过常规 NAPI/softirq,典型时延几十 µs 量级 —— 功能联调够用。

**第二阶段**(见 `eth2can_design/02_Linux侧详细设计.md`):仅替换传输层为
`enetc4_ecat_fast_xmit/recv` 的 `_k` 内核变体 + SCHED_FIFO RT 线程 +
TX 聚合,协议逻辑(本文件的窗口/TXC/EVT/CFG 部分)原样保留。

## 实现要点

- 6× `alloc_candev`,IFF_ECHO,echo_skb_max=16 = 协议窗口;
  `echo_id` 即 DATA 记录 `tag`,TXC 回来 `can_get_echo_skb` 释放槽位并
  唤醒队列 —— **TXC 是唯一流控**(协议 §6.2),窗口满即 stop_queue。
- bittiming 走标准 netlink(`ip link set ... type can bitrate ...`):
  bittiming_const 按 FlexCAN@80MHz(nominal tseg1≤96/tseg2≤32,
  data tseg1≤39/tseg2≤8);`ndo_open` 序列 = STOP→SET_BITRATE→START,
  CFG 带 token,10ms 超时 ×3 重试,MCU 幂等。
- EVT → `can_change_state`/`can_bus_off` + 错误帧(含 RX overflow);
  HB 双向 100ms,500ms 超时 → 全部 carrier off,恢复即 carrier on。
- **网关重启自恢复**(协议 §4.8/§7"MCU 复位"):HB `uptime_s` 回绕(主,
  可抓 <500ms 的快速复位)或 HB 中断后恢复(副)即判定 MCU 重启,
  workqueue(rtnl 串行化)对每个 running 通道:停队列 → 冲洗 echo 窗口
  → 重发 STOP/SET_BITRATE/START → 成功才 carrier on + 唤醒队列。
  重烧/重启 MCU 后**不再需要手动 down/up**(仅 MCU MAC 变更仍需
  rmmod/insmod 重学 peer)。
- **TXC 槽超时**(协议 §7):窗口槽 1s 无 TXC 即本地回收(计 tx_errors、
  唤醒队列)——丢一个以太帧不再永久泄漏窗口;迟到 TXC 被幂等忽略。
- 兼容 5.15(本机编译验证)与 6.9+(`can.fd.data_bittiming` 布局,
  已用版本宏隔离;RTE 6.18 内核交叉编译见下)。

## scripts/ 目录说明(每个脚本干什么、在哪跑、什么时候需要)

| 脚本 | 在哪跑 | 作用 | 什么时候需要 |
|---|---|---|---|
| `env.sh` | 主机(被 source) | 集中所有路径/工具链/板子变量(KSRC、KOUT、TOOLCHAIN_DIR、BOARD_IP...),全部可环境变量覆盖 | 不直接执行;其余主机脚本的公共配置 |
| `build_driver.sh` | 主机 | 交叉编译 `eth2can.ko`(产物落在 `linux/`,已入库);`host` 参数 = 对宿主 5.15 内核编译做 API/语法检查 | **每次改驱动源码后**(记得连同新 .ko 一起提交) |
| `build_kernel.sh` | 主机 | RTE 6.18 内核:`prepare`(defconfig+CAN 全 built-in+modules_prepare)/ 完整构建 / `menuconfig` | 首次搭环境、改内核配置、升内核 |
| `board_setup.sh` | **板上** | **日常唯一入口**:找 .ko(脚本同目录或 `../`,兼容仓库布局)→ vermagic 校验 → insmod → 等第一个网关 HB(peer 锁定)→ 6 通道 1M/8M FD 拉起 | 每次加载/重载驱动 |
| `install_kernel.sh` | 板上 | 从 bundle 解压目录把 Image(+dtb)装进 U-Boot 实际引导的 boot 分区(自动定位 `<bootdisk>p1`,首次自动留 `.orig` 备份可回滚) | 只在**更新内核**时 |
| `make_bundle.sh` | 主机 | 打包 Image+ko+脚本为一个 tgz(`full` 加 dtbs+modules) | 可选:给没有 git/网络的板子做 U 盘部署包;板子能 git clone 时**用不到** |
| `deploy.sh` | 主机 | scp 推 ko/Image 到板 + 远程执行(sshpass) | 可选:手动 scp/git 部署时**用不到** |

> 现在板上直接 `git clone` 仓库即可:`sh linux/scripts/board_setup.sh eth1`
> 会自动在 `../eth2can.ko` 找到入库的模块。`deploy.sh`/`make_bundle.sh`
> 仅作为备用通道保留。

## 构建(主机)

所有路径/工具链集中在 `scripts/env.sh`(KSRC=本仓库 real-time-edge-linux 6.18,
KOUT=KSRC/build_imx95 独立输出目录,工具链 /opt/arm-gnu-toolchain-14.3 aarch64,
板子 IP/用户/目录均可环境变量覆盖):

```sh
cd linux/scripts

./build_kernel.sh prepare      # 首次:imx_v8_defconfig + CAN 全家 built-in(=y,只有
                               # eth2can.ko 本身是模块)+ modules_prepare,几分钟
./build_driver.sh              # 交叉编译 eth2can.ko(arm64;内核未 prepare 会自动先 prepare)
./build_kernel.sh              # 完整内核:Image + dtbs + modules(64 核 ~10 分钟)
./build_kernel.sh menuconfig   # 改内核配置
./build_driver.sh host         # 对宿主内核编译,快速 API/语法检查

BOARD_IP=192.168.x.x ./deploy.sh drv eth1   # scp .ko → rmmod/insmod → 6 通道配 1M/8M 拉起
BOARD_IP=192.168.x.x ./deploy.sh kernel     # scp Image+imx95 dtb 到板上 boot 分区
```

注:仅 `prepare` 时无 Module.symvers,build_driver.sh 自动降级
`KBUILD_MODPOST_WARN=1`(.ko 照常可 insmod);跑过一次完整内核构建后即走正规 modpost。

手工方式仍可用:`make KDIR=... ARCH=arm64 CROSS_COMPILE=...`(见 Makefile)。

```sh
insmod eth2can.ko ifname=eth0            # vid=100 启用 802.1Q;peer=xx:.. 静态对端
ip link set eth2can0 type can bitrate 1000000 dbitrate 8000000 fd on
ip link set eth2can0 up
candump eth2can0 &
cansend eth2can0 123##300DEADBEEF
```

模块参数:`ifname`(下层网口,默认 eth0)、`vid`(-1=不打 VLAN tag,
bring-up 默认;部署按协议置 100)、`peer`(默认从网关 HB 学习对端 MAC)。

## 与固件联调顺序建议

1. MCU 上电(autostart+loopback)看串口统计 → 单板自检通过
2. insmod 后 `ip -d link show eth2can0`:HB 学到对端 → carrier on
3. `cansend` → MCU 串口看 dn/TXC 计数;loopback 模式下 candump 应回环收到
4. 关 loopback、接 TJA1463 扩展板,真总线对打(测试计划 M1/M2/T1-T9)
