# 跟着源码学习的建议顺序

**第一阶段：线程安全与有界队列**

从 `include/sf/pipeline.hpp` 的 `BoundedQueue<T>` 开始：
1. 为什么`try_push`只会成功或拒绝，不能无限等待？
2. `cv_.wait(lk,predicate)`为什么在睡眠中释放互斥锁？
3. 为什么`stop()`必须先唤醒所有等待中的线程？
4. 什么情况下消费者仍可在`stop()`后取完剩余任务？

使用 `tests/test_core.cpp` 里的测试验证理解。

**第二阶段：UDP收发**

读 `apps/sf_source.cpp` 的 `sendto`，和 `apps/sf_engine.cpp` 的 `recvfrom`、`epoll_wait`。先不启用批量接收；运行两个前台终端。

**第三阶段：协议和帧重组**

读 `src/protocol.cpp` 与 `src/pipeline.cpp`。重点比较`seq`（数据报编号）、`frame_id`（完整帧编号）、`chunk_index`（帧内分块编号），弄清楚为什么乱序不等于丢包。

**第四阶段：高负载与故障记录**

读 `src/engine.cpp` 与 `src/recorder.cpp`。先在没有模型的情况下模拟`--work-us`让消费者变慢，观察队列积压、降级和拒绝任务，再理解为什么黑匣子需要事件快照和异步落盘。

**第五阶段：模型与性能工程**

运行自动化测试、重复基准、理解指标定义。最后用测试报告反推系统瓶颈与性能调优。

每个阶段都建议完成一次“画出数据流程图 → 跑通对应命令 → 修改一个参数 → 解释观测结果”的小练习。
