# SignalFlight Systems V1.8：真实验收报告

本版仅保留纯C++的UDP流处理、DSP分析、并发调度、故障录制与回放，不依赖Agent、ONNX、OpenCV、PyTorch或GPU。

## 2026-11-05 当前可核对的结果（本次Linux运行环境）

| 项目 | 实际证据 | 结果 |
|---|---|---|
| GCC Release / Clang Release C++测试 | 两套构建各2项CTest | PASS |
| GCC ASan+UBSan / ThreadSanitizer | 独立构建各2项CTest | PASS |
| 核心/UDP/扩展/回放/中文路径/启停 | `docs/evidence/linux_core_verification.json` | 8/8 PASS，无可选跳过项 |
| 本机真实UDP场景 | `docs/evidence/linux_udp_benchmark.json` | 三场景×三轮完成 |
| CRC参考与查表算法 | `docs/evidence/linux_crc_comparison.json` | 校验相同，内核中位加速约4.95× |
| 参考DFT与FFT | `docs/evidence/linux_dsp_comparison.json` | 校验相同，内核中位加速约5.59× |
| 重复处理资格测试 | `docs/evidence/linux_qualification.json` | 32,000帧，8轮，无队列丢弃和分析失败 |
| 服务启停+处理中停止 | 同上 | 4轮正常+1轮途中停止，通过 |

**性能口径**：这里的CRC、FFT加速是单独的CPU计算核基准，不能当作整体UDP吞吐加速。上述吞吐也来自本机流水线或127.0.0.1环回，不代表公网/实际网卡性能。运行期间系统负载导致基准具有波动；未经实验不能声称某优化永远加速。

## Linux UDP三场景

- baseline：每轮300完整帧，0应用层队列丢弃；输入为同机UDP环回。
- weak_network：注入丢包/乱序后每轮245完整帧、55不完整；这里的丢包是测试注入结果而非企业网络实测。
- overloaded：每轮550完整帧进入调度，队列按规则舍弃部分任务，任务处理+丢弃计数守恒；尾延迟显著增加，具体见原始JSON。

## 交付范围之外的验证

- **Windows V1.8原生执行：未在本次工具环境运行。** 历史V1.6曾经由用户在Windows 11+MSYS2 UCRT64上运行通过，但新版本要再次在Windows本机完成`test`、`benchmark`、`qualify`。
- 没有完成数周持续值守、真实SDR硬件链路验证、不同CPU架构兼容性证明、恶意对手安全测试。
- SFR0002的FNV摘要只用于意外损坏检测，不是防篡改密码学认证；重放只保证输入/逻辑事件一致，不重演真实线程调度。
- 不宣称商业生产级SLA、百万连接或零丢包。此项目属于可复现的**工程求职作品**，不是商用产品认证。

## 本人电脑复测命令

Windows PowerShell：先完整编译`-Mode test`，再`-Mode benchmark -SkipBuild`和`-Mode qualify -SkipBuild`。结果报告落在本机`benchmarks/`；首次启动必须**不带SkipBuild**，以避免旧版二进制混用。任何失败需留下原始日志，不能简单改成PASS。
