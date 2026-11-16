# 测试与性能解释（Systems Edition 1.8）

## 一键测试

Linux: `python3 scripts/verify_all.py --full --qualify`（须先CMake编译）。Windows：`powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode test`，之后`-Mode benchmark -SkipBuild`及`-Mode qualify -SkipBuild`。全部使用Python标准库，无pip依赖。

验证脚本要求全部列出的检查通过；不采用`SKIPPED`冒充通过。报告中包含每项退出码、运行耗时及日志摘要。`qualify.py`检查生产消费任务守恒和反复启动/停止。

## 指标的真正含义

| 名称 | 含义/限制 |
|---|---|
| `packets_rx` | 应用层已读取的数据报数量，不等于发送方总量或网卡接收总量 |
| `crc_errors` | 收到数据且CRC校验失败，未接收的报文无法被CRC诊断 |
| `duplicates` / `reordered` | 限定重排窗口内的序号重复与非单调到达 |
| `missing_final` | 超过设定等待窗口的序号缺口，不等同可唯一归因的“网络丢包” |
| `frames_ready` | 完整重组且通过协议层检查的帧数量（分析之前） |
| `frames_expired` | 帧超时或关闭时未完整重组数量 |
| `analyzed_full` / `analyzed_light` | 进入完整/降级DSP分析并完成的任务 |
| `queue_dropped` | 队列满/严重过载策略主动舍弃的完整帧 |
| `queue_peak` | 队列最大实时深度，不得超过配置容量 |
| `record_completed` / `record_write_failed` / `record_suppressed` | 落盘完成、写盘失败、受数量上限抑制的故障快照数 |
| `p50_latency_us` / `p95_latency_us` / `p99_latency_us` | 最近至多16384个接收至分析完成的本机处理延迟分位数（微秒，上取整） |
| `p99_latency_upper_us` | 历史粗桶分位上界，不是精确p99 |

指标守恒：仅在所有输入处理完、执行`Engine::close()`后，`frames_ready = analyzed_full + analyzed_light + analyzed_failed + queue_dropped`。途中快照不能强求等式成立。录制中`record_dropped`与队列丢弃、网络丢包无直接关系。

## 性能数字解释

- CRC32参考 vs 查表，只是校验内核微基准；通过相同测试数据和严格校验和比对确认数学等价。
- FFT vs DFT，只是前64采样的DSP特征；基准不代表整体1024点FFT，更不代表整机吞吐加速。
- `sf_bench`无真实网络收发，数据在同一进程中编码后直接送入Engine；`benchmark.py`有真实本机127.0.0.1 UDP链路，两者不能混同。
- CPU型号、Windows/Linux平台、g++优化选项、任务大小、是否有录制线程、负载情况都应写在实验报告中。

## 故障恢复边界

SFR0002录制文件有CRC与整体FNV-1a摘要；摘要用于**意外损坏检测**，不是防恶意篡改认证。异常中止时未完成记录会留在`.partial_*`目录且不会被误认作有效事件。复现实验时必须说明记录是否完整；回放相同输入不等于确定性多线程重演。

Systems Edition no longer emits inference/classification fields. `sf_replay`, `sf_source`, `sf_bench` and `sf_engine` reject the old `--onnx-model` flag rather than silently ignoring it.
# UDP receive buffer diagnostics

`sf_engine` accepts `--recvbuf-bytes` (default 4194304, maximum 67108864) and prints `SOCKET_BUFFER` JSON with the requested value and the value returned by `getsockopt(SO_RCVBUF)`. Linux reports a doubled bookkeeping value; the returned value is not directly comparable to the request without accounting for that behavior. A normal request may be clamped by `net.core.rmem_max`. Linux-only `--force-recvbuf` requests `SO_RCVBUFFORCE` for this socket and requires `CAP_NET_ADMIN`; an error is reported explicitly if unavailable. This option does not change global sysctls. A larger buffer can reduce burst drops but does not prove network delivery or prevent sustained overload; record both the requested and observed values in experiments.
