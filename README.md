# FRDM-MCXE31B 以太网转 CAN FD 网关

这是一个 CAN0-first 的 Ethernet-to-CAN/CAN FD 参考工程。当前目标不是一次性做完整 6 路产品，而是先把 CAN0 的软件架构、配置、状态观测和压测闭环做稳定。

当前范围：

- 目标板：FRDM-MCXE31B
- 当前验收通道：CAN0
- 默认 CAN：CAN FD，仲裁段 1 Mbps，数据段 5 Mbps，BRS on
- 数据端口：UDP `50000`
- 控制端口：UDP `50001`
- 协议：`SCGW` v2，项目内 UDP tunnel 协议

## 架构

```text
main.c
  ethernet_lwip.c       lwIP, DHCP, link/status
  can_service.c         FlexCAN SDK boundary, queues, config, counters
  gateway_router.c      channel validation, route/config/status snapshot
  can_udp_gateway.c     UDP tunnel endpoint + JSON control endpoint

tools/can_gateway_protocol.py     host 侧共享协议 codec
tools/win_can_udp_test.py         Windows 配置、冒烟、压测 CLI
tools/socketcan_udp_bridge.py     Linux vcan/SocketCAN bridge
```

设计取舍：

- `fsl_flexcan` SDK driver 保持原样。
- `can_service.c` 只封装 FlexCAN init/re-init、TX/RX queue、配置和硬件计数。
- `can_udp_gateway.c` 只处理 UDP data tunnel 和 JSON control endpoint。
- Linux 用户侧目标是 SocketCAN/can-utils 体验：`candump`、`cangen`、`canplayer`、`canbusload`。
- `SCGW` v2 是项目内 tunnel 协议，不是行业标准协议；主流接口是 Linux SocketCAN。

## Windows 冒烟测试

先从串口 DHCP 日志确认板子 IP。下面示例使用 `192.168.8.110`。

重新初始化 CAN0：

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.110 --set-can-config --channel 0 --can-enabled --can-fd --bitrate 1000000 --data-bitrate 5000000 --config-brs --timeout 2
```

清计数：

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.110 --reset-stats --timeout 2
```

发送一帧 CAN FD+BRS，同时监听 CAN0 到 Ethernet 的回包：

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.110 --listen --send-id 0x123 --fd --brs --data "11 22 33 44" --timeout 30
```

查询状态：

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.110 --status --json --timeout 2
```

常用判断字段：

```text
tunnel.rx_frames       MCU 已接收的 Ethernet-to-CAN tunnel frame
router.rx              router 已接受的 frame
can[0].tx_start        CAN0 TX start count
can[0].tx_done         CAN0 TX done count
can[0].rx              CAN0 RX count
can[0].rx_fifo_overflow
can[0].tx_err_counter
can[0].rx_err_counter
can[0].watermark
```

PCAN-View 收不到帧时按这个顺序判断：

```text
tunnel.rx_frames 不增加       UDP/protocol 没进 MCU
router.rx 增加但 tx_start 不增 配置、队列或 frame validation 问题
tx_start/tx_done 增加          CAN bus、PCAN 配置或物理层问题
```

## Windows 压测

CAN FD+BRS，64 byte payload，summary-only JSON 输出：

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.110 --pressure --fd --brs --dlc 64 --duration 600 --rate 1000 --json-report
```

推荐 CAN0 压测流程：

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.110 --set-can-config --channel 0 --can-enabled --can-fd --bitrate 1000000 --data-bitrate 5000000 --config-brs --timeout 2
python .\tools\win_can_udp_test.py --board 192.168.8.110 --reset-stats --timeout 2
python .\tools\win_can_udp_test.py --board 192.168.8.110 --pressure --fd --brs --dlc 64 --duration 600 --rate 1000 --json-report
python .\tools\win_can_udp_test.py --board 192.168.8.110 --status --json --timeout 2
```

非过载输入下的初步验收目标：

```text
rx_fifo_overflow = 0
bus-off = 0
rx_drop = 0
tx_timeout = 0
queue watermark 不长期满
```

## Linux SocketCAN

创建 `vcan0`：

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

启动 bridge：

```bash
python3 tools/socketcan_udp_bridge.py --remote-host 192.168.8.110 --can vcan0
```

使用标准 SocketCAN 工具：

```bash
candump vcan0
cangen vcan0 -g 1 -L 64 -f
canplayer vcan0=can0 -I trace.log
```

## 构建

```powershell
& 'C:\Keil_v5\UV4\UV4.exe' -b 'enet2can_e31.uvprojx' -t 'enet2can_e31 debug' -o 'debug\codex_build.log'
```

期望结果：

```text
"debug\enet2can_e31.out" - 0 Error(s), 0 Warning(s).
```
