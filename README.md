# SignalFlight Systems Edition V1.8.0

**纯 C++17 的 Linux/Windows 实时 IQ 流处理与故障诊断项目。** 无需 AI、Agent、ONNX、OpenCV、Conda 或 GPU。保留网络、并发、DSP、故障注入、录制/回放和量化性能实验。测试自动化仅使用 Python 标准库。

> 这是可运行、可研究、可复现实验的**求职工程项目**，不是经过现场射频设备验证的商业生产软件。V1.8 当前源码已在 Windows 11 本机复验；Linux 当前源码和真实网络环境仍需实测。不要虚构未实测的速度、可用性或生产部署经历。

## 30 秒看懂它做什么

`sf_source` 发送带序号和CRC的IQ数据（QPSK/BPSK/QAM或SigMF原始IQ）→ `sf_engine` 通过Linux `epoll` / Windows Winsock `select` 接收、识别丢包和乱序、按帧重组、送入有界队列 → C++工作线程完成轻量DSP或完整FFT分析 → `FlightRecorder` 保存异常输入，`sf_replay` 离线回放，`sf_bench`/`sf_crc_bench` 形成性能证据。

### 已实测的数据

Windows 11 / i9-12900H，本机 UDP 回环：四个发送进程、共八条逻辑流；每轮发送和接收 25,600 包，处理 3,200 帧，六轮未记录发送失败、最终缺失、超时或队列丢弃。接收端处理该帧首个分片到分析完成的 p99 为 237–446 μs；该口径不含网络及发送端时间，且不包含丢弃帧。接收进程单核归一化 CPU 采样为 0.47%–15.57%，不含发送端 CPU。每档只有两轮，不能据此证明吞吐极限、稳定性或真实网络丢包率。原始汇总和条件见 [`docs/evidence/windows-localhost-20261113`](docs/evidence/windows-localhost-20261113/README.md)。

### 为什么比普通 WebServer 项目更有辨识度？

- **网络损伤可解释**：乱序、重复、CRC损坏、序号缺失、帧超时分开统计，避免笼统的“网络丢包”结论。
- **资源可控**：固定容量任务队列、状态机降级、明确的主动丢弃计数；过载时内存不因队列无限膨胀。
- **故障可追溯**：SFR0002二进制录制具有每包CRC与文件尾部摘要；异常记录先写临时目录，完成后才公开；输入回放能用于回归测试。
- **优化有证据**：CRC查表优化和FFT对照DFT；Linux批量系统调用`recvmmsg`与Windows有界`recvfrom`循环明确区分；报告实际p50/p95/p99与队列水位。
- **验证有边界**：Linux和Windows都能从源码构建，支持单元测试、UDP集成、中文路径、重复启停、异常停止及CPU过载，自动输出JSON报告。

## Windows：无需Linux，直接在 PowerShell 运行

推荐将本压缩包解压到新目录，避免覆盖旧版；项目根目录是包含`CMakeLists.txt`和`scripts`的目录。

```powershell
cd 'E:\c++\项目\SignalFlight'
$env:Path = 'E:\msys64\ucrt64\bin;' + $env:Path
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode test
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode benchmark -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode fault -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode overload -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode qualify -SkipBuild
```

`-Mode qualify`是新增资格测试，会重复运行流水线并进行多次UDP服务启停及处理中停止验证。全部是**前台命令**，无`nohup`或后台守护进程。`build_windows.ps1`保留对中文路径的编码规避处理，纯C++可执行程序无需Python第三方包。

## Linux / Ubuntu

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 scripts/verify_all.py --full --qualify
python3 scripts/run_demo.py --fault
```

若没有 Ninja，可删去`-G Ninja`。前台手动启动服务器：`./build/sf_engine --config configs/local.ini`；另一终端 `./build/sf_source --frames 200 --rate 250`。

## 测试和性能指标

- `test_core`：协议、CRC、序号窗口、帧重组、有界队列、状态机、DSP、录制与回放。
- `test_hardening`：边界数据、FFT参考对照、随机报文变异、并发生产消费、**并发多次关闭录制器**、文件损坏/截断拒绝。
- `tests/test_stop_restart.py`：多次启动和停止UDP接收，额外在持续传输时主动停止，核对任务去向与录制落盘。
- `scripts/qualify.py`：默认8轮、每轮4000帧的流水线检查，加4轮服务启停和1轮处理中停止；报告`benchmarks/qualification_report.json`。
- `scripts/verify_all.py --full --qualify`：统一测试、3轮真实本机UDP基准、5轮CRC/FFT基准、资格测试，生成`benchmarks/verification_report.json`。

**延迟口径**：接收端处理该帧首个分片到分析完成的单调时钟差（详见`docs/testing.md`），p50/p95/p99仅覆盖最近最多16384个样本；`p99_latency_upper_us`是历史分桶上界。**吞吐口径**：`sf_bench`为本机无真实Socket的流水线；`scripts/benchmark.py`为127.0.0.1 UDP，两者不得混为一谈。

## 核心源文件

| 目录 | 作用 |
|---|---|
| `src/protocol.cpp` | SFPv1，56字节包头，大小端转换，CRC32 |
| `src/pipeline.cpp` | 序号重排、去重、帧重组、队列、精确延迟分位数 |
| `src/engine.cpp` | 工作线程、过载调度、异常事件、结果输出与有序关闭 |
| `src/recorder.cpp` | 有界故障录制，SFR0002校验，异步写盘与并发关闭 |
| `src/analysis.cpp` | 无AI依赖的均方功率、64点FFT/参考DFT |
| `apps/` | 发送器、接收器、回放程序、CRC/流水线基准 |
| `scripts/` | Windows脚本、UDP场景、性能和资格测试 |
| `docs/` | 架构、协议、测试、面试答辩、真实验收证据 |

## 适合简历的描述（完成个人实测和代码掌握后使用）

> SignalFlight：Linux/Windows C++17 实时数据流处理与故障诊断系统。设计自定义UDP协议及CRC校验、非阻塞事件接收、序号去重和有界帧重组；实现工作线程池、有界任务队列和过载降级策略，完成任务守恒与关闭回收测试。设计包含文件级完整性校验的故障录制与输入回放工具；使用参考算法对照验证CRC查表与FFT优化，并通过自动化弱网、并发、压力和回放测试。**真实加速数字只填写本人机器的实测报告**。

建议至少能讲清：队列为何有界、发生过载怎样计数、哪种损失不可观测、SFR0002为什么要尾部摘要、为何输入回放不等于线程确定性回放、为什么CRC核加速不等于全系统吞吐提升。详见`docs/INTERVIEW_GUIDE.md`。

## 诚实边界

不保证UDP零丢失；`missing_final`不能区分网络丢包、源端未发、内核溢出和CRC损坏；本系统是输入级逻辑回放，不承诺还原操作系统线程交错；本地环回性能不代表物理链路；故障录制会受到容量/次数上限约束（单独计数）；FFT分析只对每帧前64个采样计算频谱中心，不是全带宽频谱监测或RF硬件产品。此版无AI/模型推理功能。

更多文档：[架构](docs/architecture.md) · [协议](docs/protocol.md) · [测试](docs/testing.md) · [Windows指南](docs/WINDOWS_POWERSHELL.md) · [求职答辩](docs/INTERVIEW_GUIDE.md) · [验收记录](docs/validation_v18.md)。
