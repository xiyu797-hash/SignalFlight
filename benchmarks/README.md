# Benchmark与验收报告目录

此目录随发行包只包含文档；所有数字报告需要在实际机器上生成，避免把其他机器或历史版本的结果误当作本机成绩。

- `verification_report.json`：单元测试、UDP集成、文件回放和性能基准的退出码与耗时。
- `qualification_report.json`：重复本机流水线负载、接收器反复启停以及处理中关闭的验收报告。
- `benchmark_results.json/csv`：三组UDP场景的真实帧数、丢失与队列数据。
- `crc_comparison.json`、`dsp_comparison.json`：分别为CPU计算内核基准；**不是系统总吞吐**。

Linux: `python3 scripts/verify_all.py --full --qualify`。Windows: `run_windows.ps1 -Mode benchmark` 再 `-Mode qualify -SkipBuild`。
