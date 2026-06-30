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
  data tseg1≤32/tseg2≤16);`ndo_open` 序列 = STOP→SET_BITRATE→START,
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

## Customer-facing scripts

`linux/scripts/` is intentionally small. For customer source delivery it should
contain only:

| Script | Purpose |
|---|---|
| `install_driver.sh` | Build `eth2can.ko` for the running kernel, load it for the current boot, and optionally configure `eth2can0..5`. |

The i.MX95/RTE kernel build, deploy, bundle, and boot-partition maintenance
scripts are platform-specific internal tools and live under `tools/imx95/`.
They are not required for normal customer installation.

Manual driver commands are still useful for debugging:

```sh
insmod eth2can.ko ifname=eth0            # vid=100 enables 802.1Q; peer=xx:.. pins peer MAC
ip link set eth2can0 type can bitrate 1000000 dbitrate 5000000 fd on
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

## Portable one-shot install on common Linux targets

For bring-up on Raspberry Pi, RK, i.MX vendor Linux, and PC Ubuntu/Debian,
use the portable installer from the repository root:

```sh
sh linux/scripts/install_driver.sh
sh linux/scripts/install_driver.sh --ifname eth1 --vid 100
sh linux/scripts/install_driver.sh --dry-run --ifname eth0
```

The installer builds `eth2can.ko` against the running kernel, loads it for the
current boot, brings the Ethernet interface up, and configures `eth2can0..5`
as CAN FD channels at 1M/8M by default. It does not install DKMS, write systemd
units, or make the module persistent across reboot. The build/load step does
not require the MCXE31B gateway to be connected. If no gateway heartbeat is
observed, the script leaves the module loaded and skips SocketCAN channel
configuration because `ip link set eth2canN ... up` would only time out.

Normally no kernel rebuild is needed. The target kernel must already provide
SocketCAN support:

- `CONFIG_CAN=y/m`
- `CONFIG_CAN_RAW=y/m`
- `CONFIG_CAN_DEV=y/m`

The target also needs a kernel build tree matching `uname -r`, normally at
`/lib/modules/$(uname -r)/build`. If a vendor BSP does not expose that path,
point the script to the prepared kernel tree:

```sh
KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname eth0
```

Platform notes:

- Ubuntu/Debian/PC: the script can install `make`, `gcc`, `kmod`, `iproute2`,
  `can-utils`, and `linux-headers-$(uname -r)` when a package manager is
  available.
- Raspberry Pi OS: if standard headers are missing, use
  `raspberrypi-kernel-headers`. Some Raspberry Pi releases install headers
  under `/usr/src/linux-headers-$(uname -r)` without creating
  `/lib/modules/$(uname -r)/build`; the installer auto-detects that path.
  The header directory must still match `uname -r` exactly. For example,
  a running `6.1.21-v8+` kernel cannot use `linux-headers-6.1.21-v7l+`.
  If only `6.1.21+`, `6.1.21-v7+`, and `6.1.21-v7l+` exist, boot a matching
  32-bit kernel variant or install/provide the exact `-v8+` kernel build tree.
- RK/i.MX vendor Debian: many BSP kernels require the vendor kernel source or
  an SDK-prepared build directory; use `KDIR=...` when headers are not packaged.
- Fedora/RHEL/Rocky, Arch, and openSUSE are handled on a best-effort basis via
  their standard kernel header/devel packages.

Useful options:

```sh
sh linux/scripts/install_driver.sh --ifname end0 --channels 0-5
sh linux/scripts/install_driver.sh --ifname eth0 --bitrate 1000000 --dbitrate 8000000
sh linux/scripts/install_driver.sh --no-deps --dry-run
sh linux/scripts/install_driver.sh --no-load
sh linux/scripts/install_driver.sh --require-gateway
```

Common failures:

- Missing `/lib/modules/$(uname -r)/build`: install matching kernel headers or
  pass `KDIR=/path/to/kernel/build`.
- Missing SocketCAN kernel config: install or boot a kernel with SocketCAN
  enabled; this is the case that may require a kernel rebuild.
- `__aeabi_uldivmod` during modpost on 32-bit ARM: the driver contains a raw
  64-bit divide. Use the current driver source; time divisions must go through
  kernel helpers such as `div_u64()`.
- `vermagic` mismatch: rebuild `eth2can.ko` on the target or against the exact
  running kernel build tree.
- `gateway heartbeat was not observed`: the module loaded, but Linux did not
  receive the MCXE31B E2CF heartbeat on the selected Ethernet interface. This
  is not an installation failure unless `--require-gateway` is used. The script
  skips channel configuration because retrying it would only time out:

  ```sh
  ip -br link show eth0
  sudo dmesg | tail -80
  sudo tcpdump -eni eth0 'ether proto 0x88b5 or (vlan and ether proto 0x88b5)'
  ```

  Expected traffic is EtherType `0x88b5` heartbeat frames every 100 ms. If
  `tcpdump` sees nothing, check MCU power, cable, selected interface,
  switch/VLAN path, and that the raw-Ethernet MCXE31B firmware is running.
  If `tcpdump` sees frames but the driver does not print `eth2can: gateway
  alive`, capture a short packet log and compare the E2CF header/heartbeat
  format with `src/e2cf_proto.h`.
