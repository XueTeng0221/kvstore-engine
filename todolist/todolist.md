# Distributed KVCache Server Implementation Checklist

本清单是实现状态的唯一事实来源。执行规则、角色职责和通过标准见 `AGENTS.md`。所有任务默认由 Agent A 实现、Agent B 审计；最终审计结论必须为 `pass` 或 `pass-with-risk`。

## 使用规则

- 状态：`[ ]` 未开始，`[~]` 进行中/待审计，`[x]` 已通过，`[!]` 阻塞。
- `### [状态] Pn.m` 标题是带稳定 ID 的可审计任务；标题下的 checkbox 是该任务的实现与验收条件，不是独立任务。
- 开始任务时修改标题状态并填写一份以 `Pn.m` 为 ID 的工作记录；只有内部 checkbox 全部完成且小节“验收”满足，任务才可通过审计。
- 每个内部 checkbox 必须对应实际代码/文档产物或自动化测试证据；结果记录在该任务的 Changed files、Commands 和 Test result 中。
- 开始实现前，在目标项下填写负责人、依赖和本轮范围。
- 完成实现后填写测试命令与结果，再交由 Agent B 审计。
- 审计失败不得勾选；风险接受必须转化为有编号的后续项。
- 每项的“验收”均为最低标准，不能用人工观察替代自动化测试。

## D0：规划文档初始化

- [x] D0.1 创建 `AGENTS.md` 和完整实现清单

依赖：无。

验收：双 Agent 流程、架构边界、全部用户需求、阶段依赖、自动化验收口径和工作记录模板齐全；Agent B 最终结论为 `pass` 或 `pass-with-risk`。

工作记录：

```text
Task IDs: D0.1
Owner: Agent A
Dependencies: none
Scope: 创建仓库协作规范和分阶段实现清单
Changed files: AGENTS.md, todolist/todolist.md
Commands: read/glob/grep 文档与仓库结构
Test result: 文档关键词和路径一致性检查通过
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: 缺少统一逐项记录/依赖；首版分布式拓扑决策晚于复制实现；核心目录边界措辞不够强
Residual risks: 已在 round 2 前修复并复审

Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: checkbox 与任务 ID/验收映射不稳定；依赖引用粒度模糊；primary/replica 术语未统一
Commands: read AGENTS.md, todolist/todolist.md, config/config.json; glob repository
Residual risks: 已在 round 3 前改为小节级稳定任务、展开依赖并统一术语

Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: 未及时追加 round 2 审计历史；逐项证据字段仍不明确
Commands: read AGENTS.md, todolist/todolist.md
Residual risks: 已在 round 4 前补齐审计历史和逐项证据字段

Audit round: 4
Auditor: Agent B
Verdict: pass
Findings: none
Commands: read AGENTS.md, todolist/todolist.md
Residual risks: none
```

## 全局里程碑

- [x] M0 工程骨架、配置和公共基础设施可构建
- [x] M1 四种单机引擎通过统一一致性测试
- [x] M2 三类协议与命令分发可通过 epoll 对外服务
- [x] M3 RESP 兼容常用 Redis 客户端和扩展命令
- [x] M4 AOF、快照及崩溃恢复可验证
- [ ] M5 primary/replica 全量与增量复制可验证
- [ ] M6 KVCache 分级、匹配和请求合并可验证
- [ ] M7 vLLM/SGLang 至少各完成一个可运行集成路径
- [ ] M8 多节点部署、压测、故障注入和发布门禁完成

## 依赖矩阵

未满足硬依赖时任务状态应为 `[!]`，不得提前实现。括号内依赖适用于对应小节全部 checkbox。

- P0.1（无）；P0.2（无）；P0.3（P0.2）；P0.4（P0.2、P0.3）
- P1.1（P0.1、P0.2、P0.3）；P1.2（P1.1）；P1.3（P1.1）；P1.4（P1.1）；P1.5（P1.1）；P1.6（P1.2、P1.3、P1.4、P1.5）
- P2.1（P0.1、P1.1）；P2.2（P2.1）；P2.3（P2.1）；P2.4（P0.2、P0.3、P0.4）；P2.5（P1.6、P2.1、P2.2、P2.3、P2.4）
- P3.1（P2.1、P2.4）；P3.2（P1.1、P3.1）；P3.3（P2.5、P3.2）
- P4.1（P0.3、P1.1）；P4.2（P0.4、P4.1）；P4.3（P0.4、P4.1）；P4.4（P4.2、P4.3）
- P5.1（P0.1、P2.4、P4.1）；P5.2（P0.1、P2.4、P4.1）；P5.3（P4.3、P4.4、P5.1、P5.2）；P5.4（P5.3）；P5.5（P5.1、P5.2、P5.3、P5.4）
- P6.1（P0.4、P2.4、P2.5）；P6.2（P0.4、P2.4、P2.5）
- P7.1（P0.1、P1.1）；P7.2（P7.1）；P7.3（P4.3、P7.1）；P7.4（P7.2、P7.3）；P7.5（P7.2、P7.3、P7.4）
- P8.1（P0.1、P7.1、P7.2、P7.3、P7.4、P7.5）；P8.2（P8.1）；P8.3（P8.1）；P8.4（P8.2、P8.3）
- P9.1（P0.1、P5.3、P7.4）；P9.2（P2.5、P3.3、P4.4、P5.5、P6.1、P6.2、P7.5、P8.4、P9.1）；P9.3（P2.5、P4.4、P5.5、P7.5、P8.4）
- P10.1（P1.6、P2.5、P3.3、P4.4、P5.5、P6.1、P6.2、P7.5、P8.4、P9.1、P9.2、P9.3）；P10.2（P10.1）；P10.3（P10.1、P10.2）

---

## P0：需求冻结与工程骨架

### [x] P0.1 行为与非功能基线

- [x] 冻结首版拓扑为单 primary + 多 replica；首版不分片，分片作为 P9 可选扩展
- [x] 定义 primary 写入、replica 只读/拒绝写、故障切换和一致性边界
- [x] 定义 Text + KV、Batch 和 RESP 的 wire format、错误码、长度限制与示例
- [x] 定义 `SET/GET/DEL/MOD/EXIST/SAVE/LOAD` 的精确语义和响应
- [x] 定义二进制安全 key/value、最大尺寸、TTL 是否支持及越界行为
- [x] 定义单 key 原子性、并发可见性、持久化确认点和复制一致性目标
- [x] 定义首版容量、吞吐、延迟、恢复时间和复制延迟目标
- [x] 创建 `docs/architecture.md`，记录模块依赖、线程模型和数据流

验收：协议文档无歧义；每个错误路径有稳定错误码；性能目标包含硬件与负载前提。

### [x] P0.2 CMake 与目录

- [x] 创建 C++20 顶层 `CMakeLists.txt` 和标准目录结构
- [x] 增加 Debug/Release、严格警告、ASAN/UBSAN/TSAN 和测试开关
- [x] 接入测试框架并固定 FetchContent 版本
- [x] 增加 `kvstore_server`、单元测试、集成测试和 benchmark targets
- [x] 生成版本信息并实现 `--help/--version/--check-config`
- [x] 增加格式化和静态检查配置

验收：干净环境可 configure/build/test；Debug 严格警告为零；sanitizer 构建可运行 smoke test。

### [x] P0.3 公共基础设施

- [x] 实现 `Status/Result<T>`、稳定错误码和错误到协议响应的映射
- [x] 实现 fd、mmap、线程和 buffer 的 RAII 封装
- [x] 实现二进制安全 `ByteBuffer`/只读 view 与边界检查
- [x] 实现结构化日志，包含 request ID、connection ID、node ID 和 event offset
- [x] 实现 CRC32，并用公开测试向量验证
- [x] 实现可替换 clock/随机源，支持确定性测试

验收：公共组件单测通过；错误路径无泄漏；ASAN/UBSAN 无报告。

### [x] P0.4 统一 JSON 配置

- [x] 设计并填写 `config/config.json` 完整默认配置
- [x] 使用成熟 JSON 库解析，禁止手写字符串解析
- [x] 实现 server/protocol/engine/network/persistence/replication/kvcache/observability 配置模型
- [x] 校验必填字段、枚举、范围、跨字段约束和路径可访问性
- [x] 拒绝未知关键字段，并输出 JSON path 级错误
- [x] 实现配置脱敏打印和 `--check-config`
- [x] 为合法、缺失、类型错误、边界值和冲突配置增加测试

验收：进程只从 JSON 获取运行配置；无隐式环境差异；错误配置启动失败且诊断明确。

工作记录：

```text
Task ID: P0.1
Owner: Agent A
Dependencies: none
Scope: 冻结首版协议、拓扑、一致性和性能基线
Condition evidence: docs/architecture.md 与 docs/protocol.md 覆盖拓扑、协议、命令、一致性及带硬件前提的性能目标
Task ID: P0.2
Owner: Agent A
Dependencies: none
Scope: CMake、目录、CLI、测试和工具链骨架
Condition evidence: CMakeLists.txt、cmake/version.hpp.in、严格告警/sanitizer、GoogleTest、CLI 与 benchmark targets 构建通过
Task ID: P0.3
Owner: Agent A
Dependencies: P0.2
Scope: Status/Result、RAII、buffer、日志、CRC32、clock/random
Condition evidence: include/kvstore/common、src/common 及 CommonTest 7 项通过
Task ID: P0.4
Owner: Agent A
Dependencies: P0.2, P0.3
Scope: 完整 JSON 配置模型、校验、脱敏输出和测试
Condition evidence: config/config.json、Config 模型、严格 schema/范围/冲突/路径校验及 ConfigTest 6 项通过
Changed files: CMakeLists.txt, .clang-format, .clang-tidy, .gitignore, cmake/, config/, docs/, include/kvstore/common/, include/kvstore/config/, src/common/, src/config/, src/server/, tests/
Commands: cmake Debug/Release/ASAN+UBSAN/TSAN configure；cmake --build；ctest；kvstore_server --check-config
Test result: round 1 后 Debug 28/28 pass；ASAN+UBSAN 28/28 pass；TSAN 28/28 pass（WSL2 使用 setarch x86_64 -R 启动）；format-check pass；核心源逐文件 GCC analyzer pass
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: config frame size 无符号回绕；启用项超时/地址约束不全；Result Release 错误访问契约不安全；失败路径测试和静态检查证据不足
Commands: Debug/ASAN+UBSAN/TSAN 全构建与 25 tests；Release 108-case benchmark；git diff --check
Residual risks: 等待 Agent A 修复和 round 2 审计

Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: 缺少最大 uint64 frame 和 enabled interval/address 自动化回归；Import 失败未比较完整快照；benchmark 报告未统一十轮 raw 数据和实测时间
Commands: Debug/ASAN+UBSAN/TSAN 28/28；format-check；逐文件 GCC analyzer；临时配置边界验证；360-case benchmark 核对
Residual risks: 已补充回归断言、完整 Import 快照、十轮 raw benchmark 与最终时间，等待 round 3

Audit round: 3-5
Auditor: Agent B
Verdict: pass
Findings: none
Commands: Debug 31/31；ASAN/UBSAN 31/31；TSAN 31/31；format-check；逐文件 GCC analyzer；十轮 360-case benchmark 及 raw 重算
Residual risks: none

Audit round: 3
Auditor: Agent B
Verdict: pending
Findings: pending
Commands: pending
Implementation update: Agent A fixed async test ownership by capturing copied LookupRequest values per thread, exposed RequestPath scheduler metrics, and added bounded parameterized RequestPath tests for one-shot Submit, Pop, and Complete failures. Production defaults remain unchanged; injected completion failure now releases its active reservation so a retry cannot inherit stale inflight state.
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|TieredStore|Policy)'; cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=thread; cmake --build build-tsan -j2; setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure -R 'KvCache(RequestPath|Policy)'
Test result: Debug RequestPath 10/10 passed; TSAN RequestPath 10/10 passed with setarch; no TSAN race reports. Each failure test used 2-second future waits, asserted the expected internal error, verified scheduler pending/inflight bytes returned to zero, and verified a later retry succeeded.
Residual risks: failure hooks are one-shot mutable configuration and are intended only for deterministic tests; no independent Agent B audit has been performed, and synchronous filesystem shutdown remains bounded only by the configured operation deadline.
Residual risks: pending

Audit round: 14
Auditor: Agent B
Verdict: fail
Findings: medium tests/unit/kvcache_request_path_test.cpp:208-210 used unbounded polling waits; medium tests/unit/kvcache_request_path_test.cpp:328-329 and 359-360 used timing-based polling without deterministic worker coordination; unused first_promise remained; failure assertions could leave worker/gate state unreleased
Commands: pending remediation validation
Residual risks: P7.5 remains [~]; Agent A must validate bounded waits, unconditional cleanup, and Debug/ASAN/TSAN focused plus repeated failure/coalescing tests; no audit pass is claimed

Round 14 remediation update: Agent A replaced all request-path polling sites with a bounded two-second WaitUntil helper, used worker pause/resume for distinct queued operations, added unconditional resume/gate-release guards, removed first_promise, and preserved production code.
Changed files: tests/unit/kvcache_request_path_test.cpp, todolist/todolist.md
Commands: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON && cmake --build build -j2 && ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=address,undefined && cmake --build build-asan -j2 && ctest --test-dir build-asan --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=thread && cmake --build build-tsan -j2 && setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; `setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure --repeat until-fail:5 -R 'KvCacheRequestPathTest\\.(CoalescesDiskReadAndRecordsMetrics|PopFailureDrainsDistinctPendingOperationsAndRetrySucceeds|CompleteFailureIsSharedByCoalescedWaitersAndRetrySucceeds)|OneShotFailures/KvCacheRequestPathSchedulerFailureTest'`
Test result: Debug focused 50/50 passed in 7.41s; ASAN/UBSAN focused 50/50 passed in 18.01s with no sanitizer reports; TSAN focused 50/50 passed in 28.86s with no race reports; repeated failure/coalescing filter 6/6 test cases passed five times (30 executions) in 1.68s with no TSAN reports. P7.5 remains [~]; independent Agent B audit is still required and no audit pass is claimed.

Audit round: 15
Auditor: Agent B
Verdict: fail
Findings: medium tests/unit/kvcache_request_path_test.cpp still used WaitUntil wall-clock polling for pending/coalescing state; medium explicit ReadGate::Release calls conflicted with RAII ownership; bounded completion waits and unconditional cleanup required remediation.
Commands: pending remediation validation
Residual risks: P7.5 remains [~]; Agent A must complete sanitizer and repeated scheduler/coalescing validation. No audit pass is claimed.

Round 15 remediation update: Agent A replaced WaitUntil with RequestPathTestPeer condition-variable acknowledgments observing pending/coalesced transitions under RequestPath::mutex_, removed the unused helper, made resume and gate cleanup idempotent through RAII guards, and retained bounded future waits. The RequestPath state condition variable is notified at pending insertion and coalescing transitions.
Changed files: include/kvstore/kvcache/request_path.hpp, src/kvcache/request_path.cpp, tests/unit/kvcache_request_path_test.cpp, todolist/todolist.md
Commands: `cmake --build build -j2 && ctest --test-dir build --output-on-failure -R 'KvCacheRequestPathTest\\.(CoalescesDiskReadAndRecordsMetrics|PopFailureDrainsDistinctPendingOperationsAndRetrySucceeds|CompleteFailureIsSharedByCoalescedWaitersAndRetrySucceeds)|OneShotFailures/KvCacheRequestPathSchedulerFailureTest'`
Test result: Debug focused remediation subset 6/6 passed in 0.21s. ASAN/UBSAN, TSAN, and repeated scheduler/coalescing runs remain pending; P7.5 stays [~] and no audit pass is claimed.

Audit round: 16
Auditor: Agent B
Verdict: fail
Findings: medium src/kvcache/request_path.cpp:246-256 used a 1ms future.wait_for polling loop; medium completion paths directly called promise.set_value and could race during owner/worker/shutdown/failure handling, with no completion notification contract
Commands: pending remediation validation
Residual risks: P7.5 remains [~]; Agent A must validate one-shot completion, cancellation/deadline wakeups, and Debug/ASAN/TSAN focused plus repeated failure/coalescing runs. No audit pass is claimed.

Round 16 remediation update: Agent A added Pending completion state and condition_variable, centralized one-shot promise fulfillment in Complete, and changed Lookup to wait on completion notification with a waiter-local stop_callback and deadline. RequestPath mutex is not held while fulfilling futures; shutdown, worker failure, cancellation, and normal completion notify_all. Agent B round 17 requested; P7.5 remains [~].
Changed files: include/kvstore/kvcache/request_path.hpp, src/kvcache/request_path.cpp, todolist/todolist.md
Commands: `cmake --build build -j2 && ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=address,undefined && cmake --build build-asan -j2 && ctest --test-dir build-asan --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=thread && cmake --build build-tsan -j2 && setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; setarch TSAN filtered failure/coalescing suite `--repeat until-fail:5`
Test result: Debug focused 52/52 passed; ASAN/UBSAN focused 52/52 passed with no reports; setarch TSAN focused 52/52 passed with no race reports; repeated failure/coalescing suite passed 5/5 for each of 8 filtered cases (40 executions) with no TSAN reports. Agent B round 17 is requested; P7.5 remains [~] and no audit pass is claimed.

Audit round: 13
Auditor: Agent B
Verdict: fail
Findings: medium tests still used timing/polling and a filesystem gate for coalescing determinism; medium LookupResult mixed source-internal loaded ranges with recompute ranges; requested full Debug/ASAN/TSAN validation evidence was pending
Commands: pending remediation validation
Residual risks: Agent A must run the requested Debug, ASAN focused, setarch TSAN focused, and five-repeat scheduler-failure validations; independent audit remains required and no audit pass is claimed
Validation update: Agent A changed CoalescesDiskReadAndRecordsMetrics to pause/resume the RequestPath worker, assert one pending scheduler operation, and release the gate through an unconditional guard; source-internal loaded ranges are now exposed only by TensorRangeView and recompute_ranges contains absent suffixes only.
Commands: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON && cmake --build build -j2 && ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=address,undefined && cmake --build build-asan -j2 && ctest --test-dir build-asan --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=thread && cmake --build build-tsan -j2 && setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'`; repeated setarch TSAN failure/coalescing filter with `--repeat until-fail:5`.
Test result: Debug focused 50/50 passed (7.06s); ASAN/UBSAN focused 50/50 passed (18.40s); TSAN focused 50/50 passed (28.51s), no race reports; repeated TSAN scheduler/coalescing filter 6/6 test cases passed, each repeated five times (1.64s), no race reports. P7.5 remains [~]; independent Agent B audit is still required and no audit pass is claimed.
```

---

## P1：统一存储 API 与四种引擎

### [x] P1.1 Engine API

- [x] 定义线程安全 `IEngine` 接口及 owned value 返回模型
- [x] 定义 create/get/delete/modify/exists/upsert/increment/scan/export/import 原语
- [x] 明确 key 不存在、重复 key、整数错误、容量不足和取消状态
- [x] 提供原子 upsert，支撑 Redis `SET` 覆盖语义
- [x] 提供原子 increment，支撑 `INCR/DECR` 与溢出检测
- [x] 提供一致性测试套件，可参数化运行在所有引擎上

验收：四种引擎只能通过同一接口被上层访问；接口文档注明并发和生命周期。

### [x] P1.2 Array Engine

- [x] 实现容量管理、查找、插入、修改和删除
- [x] 定义有序/无序布局并测试删除后的索引正确性
- [x] 增加并发读写、容量耗尽和大 value 测试

### [x] P1.3 RBTree Engine

- [x] 基于可靠实现或充分测试的红黑树实现 CRUD
- [x] 验证根黑、无红红边、路径黑高一致和排序性质
- [x] 增加顺序插入、逆序插入、随机删除和并发测试

### [x] P1.4 Hash Engine

- [x] 实现哈希桶、扩容策略、冲突处理和容量限制
- [x] 防止扩容期间可见性错误和退化输入导致无界阻塞
- [x] 增加高冲突、rehash、并发读写和二进制 key 测试

### [x] P1.5 SkipList Engine

- [x] 实现层高生成、查找、插入、修改和删除
- [x] 固定测试随机种子并验证层级不变量
- [x] 增加顺序/随机负载及并发测试

### [x] P1.6 引擎对比与门禁

- [x] 对所有引擎运行模型测试/差分测试，参考模型为标准容器加互斥锁
- [x] 对随机命令序列运行至少 100 万步属性测试
- [x] 建立 key/value 尺寸和读写比例矩阵 benchmark
- [x] 报告吞吐、p50/p95/p99、内存放大和扩容/尾延迟
- [x] TSAN 覆盖四种引擎的并发测试

验收：语义差分为零；sanitizer 通过；性能报告能支持默认引擎选择。

工作记录：

```text
Task ID: P1.1
Owner: Agent A
Dependencies: P0.1, P0.2, P0.3
Scope: 统一线程安全 API、owned value、原子 upsert/increment 和参数化契约测试
Condition evidence: IEngine、Engine factory、统一状态/owned value、原子 upsert/increment、百万步参数化差分测试
Task ID: P1.2
Owner: Agent A
Dependencies: P1.1
Scope: 无序紧凑 Array 引擎与容量/并发测试
Condition evidence: ArrayEngine 无序紧凑布局、swap-delete、容量/1 MiB value/并发测试
Task ID: P1.3
Owner: Agent A
Dependencies: P1.1
Scope: RBTree 引擎、不变量检查与并发测试
Condition evidence: 自实现 RBTree、ValidateInvariants、升序/降序/随机删除和并发测试
Task ID: P1.4
Owner: Agent A
Dependencies: P1.1
Scope: Hash 引擎、冲突/rehash/并发测试
Condition evidence: std::unordered_map HashEngine、初始单桶 rehash 5000 keys、二进制 key 和并发测试
Task ID: P1.5
Owner: Agent A
Dependencies: P1.1
Scope: 固定种子 SkipList 引擎、不变量与并发测试
Condition evidence: 固定种子 SkipList、ValidateInvariants、随机差分和并发测试
Task ID: P1.6
Owner: Agent A
Dependencies: P1.2, P1.3, P1.4, P1.5
Scope: 统一差分、百万步属性、TSAN 和 Release benchmark
Condition evidence: EngineDifferentialTest 1,000,000 步；ASAN/UBSAN/TSAN；十轮隔离 Release 360-case 矩阵、raw benchmarks/p1-current.txt 及 docs/p1-benchmark.md
Changed files: include/kvstore/engine/, src/engine/, tests/unit/engine_*.cpp, benchmarks/engine_benchmark.cpp, docs/p1-benchmark.md
Commands: ctest Debug/ASAN+UBSAN/TSAN；build-release/engine_benchmark；/usr/bin/time -v benchmark
Test result: round 1 后三种构建各 28/28 pass；四引擎百万步差分及混合不变量为零；Release 十轮 360-case 原始结果已保存，默认 Hash
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: Import/二进制 value/Export/Exists/取消失败路径证据不足；结构混合测试不足；benchmark 与生产目标口径不一致、无回归比例和原始结果
Commands: Debug/ASAN+UBSAN/TSAN 全构建与 25 tests；Release 108-case benchmark
Residual risks: 等待 Agent A 修复和 round 2 审计

Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: 同 round 2 P0 记录；Import 和最终 benchmark 证据仍不足
Commands: Debug/ASAN+UBSAN/TSAN 28/28；Release 360-case benchmark
Residual risks: 已修复，等待 round 3

Audit round: 3
Auditor: Agent B
Verdict: pending
Findings: pending
Commands: pending
Residual risks: pending

Audit round: 4-5
Auditor: Agent B
Verdict: pass
Findings: none
Commands: Debug 31/31；ASAN/UBSAN 31/31；TSAN 31/31；format-check；逐文件 GCC analyzer；十轮 360-case benchmark 及最终报告/raw 一致性复核
Residual risks: none
```

---

## P2：命令编排、协议与 epoll 服务

### [x] P2.1 统一命令模型与分发

- [x] 定义 typed command/request context/response，协议层不直接访问引擎
- [x] 实现命令注册与分发，避免协议间复制业务逻辑
- [x] 实现 `SET/GET/DEL/MOD/EXIST/SAVE/LOAD`
- [x] 将引擎错误稳定映射到各协议响应
- [x] 加入请求 deadline、取消、最大在途请求和背压
- [x] 为每个命令增加协议无关单元测试

验收：同一命令通过不同协议得到等价结果；未知命令和参数错误不会关闭健康连接。

### [x] P2.2 Text + KV 协议

- [x] 冻结 framing，支持二进制 value，不能依赖换行承载任意 payload
- [x] 实现增量 parser，处理分片、粘包和单连接多命令
- [x] 实现 encoder、错误响应和协议版本
- [x] 增加畸形长度、超限、提前 EOF 和 fuzz 测试

### [x] P2.3 Batch 协议

- [x] 定义 batch header、record count、总长度、每项状态和最大限制
- [x] 明确 batch 是逐项原子还是整体原子，并实现对应行为
- [x] 支持混合读写及保持响应顺序
- [x] 对超大 batch、部分失败和背压增加测试

### [x] P2.4 epoll 网络后端

- [x] 定义 `INetworkBackend` 与 connection state 生命周期
- [x] 实现 nonblocking accept/read/write、边沿触发 drain 和 partial write
- [x] 实现 per-connection input/output 高水位与读暂停
- [x] 实现连接空闲、握手、请求和关闭超时
- [x] 实现 signal 驱动的优雅停机和在途请求排空
- [x] 测试慢客户端、断连、半关闭、fd 复用和连接风暴

验收：epoll 后端可在同一端口识别已启用协议并稳定服务；无 busy loop、无未界定 buffer 增长。

### [x] P2.5 Server 装配

- [x] 从 `config/config.json` 创建引擎、协议、网络、持久化和复制组件
- [x] 启动阶段失败执行逆序清理并返回非零状态
- [x] 增加 readiness/liveness 内部状态
- [x] 实现端到端 smoke test：启动、CRUD、停机、重启

工作记录：
```text
Task IDs: P2.1, P2.2, P2.3, P2.4, P2.5
Owner: Agent A
Dependencies: P1.1, P1.6, P0.4
Scope: 本轮修复 typed command dispatcher 事务边界、多协议语义、Batch frame 身份、epoll 连接生命周期/背压/超时/协议门控，以及 server readiness 与同步 SAVE/LOAD 装配
Implementation decisions: Batch 逐项原子并保持响应顺序；epoll 增加配置化 input/output 水位、空闲/握手/优雅停机超时；协议探测支持分片且严格遵循 enabled；Redis CLIENT 状态保存在连接会话
Current scope after audit convergence: 补齐 RequestContext/deadline/cancel、INetworkBackend、未知命令健康连接、Native/Batch 边界测试、真实 epoll socket 生命周期及进程级 restart smoke
Progress evidence: RequestContext/connection IDs、INetworkBackend、server lifecycle state、typed unknown command errors、Native/Batch boundary tests and real TCP fixture added; process restart smoke and fuzz remain pending
Condition evidence: command dispatcher; RESP/native/Batch incremental parsers; epoll edge-triggered accept/read/write and live RESP SET/GET smoke
Changed files: include/kvstore/command/, include/kvstore/protocol/, include/kvstore/net/, src/command/, src/protocol/, src/net/, src/server/main.cpp, tests/unit/protocol_test.cpp
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure; cmake --build build --target format-check; real TCP ServerFixture tests; redis-py/Node redis/go-redis smoke clients
Test result: Debug 55/55 pass; ASAN/UBSAN/TSAN setarch matrix previously 45/45; ServerFixture 4/4; redis-py 6.4.0, Node redis 6.2.1, go-redis 9.7.0 smoke pass; redis-cli unavailable via local package runtime
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: epoll half-close/backpressure/config protocol gating missing; batch frame handling and several Redis semantics previously incomplete; fixed subset now under round 2 review
Residual risks: P2.3/P2.4/P2.5 full failure injection and socket tests pending

Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: Dispatcher/AOF 事务与并发、offset/event_id 恢复、AOF partial write/size/metadata、snapshot 完整 header/crash、epoll backpressure/timeout/half-close/protocol gating、Batch frame identity、Redis 多 key/CLIENT 语义仍有阻塞问题；已完成 Dispatcher 串行化、AOF metadata CRC/连续校验、snapshot header CRC、部分 RESP/Batch 修复
Commands: Debug/ASAN/UBSAN/TSAN 40/40；format-check；P2-P4 focused 9/9；live epoll RESP smoke
Residual risks: P2-P4 继续保持 [~]，需独立修复并重新审计

Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: P2-P4 首轮审计发现写事件失败回滚、offset/event_id 恢复、Redis 语义、AOF 格式/同步/replay、snapshot durability、epoll half-close/backpressure/timeout、Batch frame boundary 和配置启用行为问题；已修复部分 dispatcher/AOF/RESP/Batch 边界，仍待复审
Commands: Debug 40/40；ASAN/UBSAN 40/40；focused protocol/persistence 9/9；live epoll RESP SET/GET smoke
Residual risks: 等待后续修复与 Agent B round 3

Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: Dispatcher/AOF failure原子性与恢复 event_id；epoll output backpressure、协议探测/配置 gating、timeout/drain；Batch frame 对应；Redis 多 key/CLIENT 语义仍不满足
Commands: Debug/ASAN+UBSAN/TSAN 40/40；format-check；focused 9/9
Residual risks: P2 全部任务保持 [~]，不得标记完成

Audit round: 21
Auditor: Agent B
Verdict: pass-with-risk
Findings: P2.1/P2.3/P2.5 实现验收通过；P2.2 缺实际 libFuzzer 运行；P2.4 缺慢客户端、fd 复用和 shutdown 后零 dispatch 专项回归
Commands: Debug 73/73；ASAN/UBSAN 73/73；TSAN setarch 73/73；format-check；git diff --check；真实 TCP fixture
Residual risks: P2.2/P3.1/P10.1 fuzz（Owner: Agent A）；P2.4/P10.1 socket 压力与生命周期专项测试（Owner: Agent A）

Audit round: 22
Auditor: Agent B
Verdict: pass-with-risk
Findings: P2.1-P2.5 全部验收通过；无 critical/high；慢读测试未确定性触发内核 EAGAIN/partial-write，转 P10.1
Commands: Debug 77/77；ASAN/UBSAN 77/77；TSAN setarch 77/77；Clang 18 libFuzzer 10,000 runs；socket 专项 repeat 20；format-check；git diff --check
Residual risks: P2.4/P10.1，Owner: Agent A；增加受控小 SO_SNDBUF、确定性 EAGAIN/partial-write 回归
```

---

## P3：Redis RESP 兼容层

### [x] P3.1 RESP parser/encoder

- [x] 支持 RESP2 Simple String、Error、Integer、Bulk String、Array 和 Null
- [x] 评估 RESP3；首版不实现时明确拒绝/协商行为
- [x] 实现增量、多命令 pipeline、嵌套深度和 frame 上限
- [x] 保持 key/value 二进制安全
- [x] 增加 redis-protocol corpus、fuzz 和恶意长度测试

### [x] P3.2 Redis 命令

- [x] `SET` 使用原子 upsert，已存在 key 不通过竞态的 EXIST+MOD 实现
- [x] 实现 `GET/DEL/EXISTS/MGET`
- [x] 实现原子 `INCR/DECR`、非整数和溢出错误
- [x] 实现 `PING [message]`、`ECHO message`
- [x] 实现最小 `CLIENT SETINFO/SETNAME/GETNAME` 探测兼容
- [x] 实现最小 `INFO` sections，未支持命令返回标准错误
- [x] 决定并记录 `SAVE/LOAD` 在 RESP 层的暴露方式
- [x] 增加 pipeline、并发覆盖、MGET Null 和客户端探测测试

### [x] P3.3 客户端兼容验证

- [x] 使用 `redis-cli` 完成 CRUD、pipeline 和 INFO smoke test
- [x] 使用至少 Python redis-py、Node redis、Go go-redis 验证连接探测
- [x] 记录不兼容命令和版本边界

验收：常见客户端无需关闭健康检查即可连接；覆盖写和计数命令在线性化测试中正确。

工作记录：
```text
Task IDs: P3.1, P3.2, P3.3
Owner: Agent A
Dependencies: P2.1, P2.4, P2.5, P1.1
Scope: RESP2 incremental parser/encoder and Redis command compatibility
Current scope after audit convergence: 修正 Redis arity/INFO/CLIENT/HELLO/SAVE 暴露，补多 key/MGET/计数/探测测试并运行可用客户端矩阵
Progress evidence: INCRBY/DECRBY, Redis arity, INFO sections, typed unknown/HELLO errors, native management commands, binary/limit tests, real TCP session tests, fixed-version client smoke scripts and docs/redis-compatibility.md added
Condition evidence: RESP2 bulk/array/null/error, pipeline/fragmentation, atomic Redis SET overwrite, MGET Null, INCR/DECR, PING/ECHO/INFO
Changed files: include/kvstore/protocol/resp.hpp, src/protocol/resp.cpp, src/command/dispatcher.cpp, tests/unit/protocol_test.cpp
Commands: ctest --test-dir build --output-on-failure -R ProtocolTest
Test result: ProtocolTest 14/14 pass; client smoke matrix pass for redis-py 6.4.0, Node redis 6.2.1, go-redis 9.7.0; local redis-cli unavailable
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: initial RESP semantic/type issues and absent client compatibility matrix; Redis overwrite/null/type fixes added, client matrix pending
Residual risks: P3.3 external client compatibility pending

Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: Redis DEL/EXISTS 多 key、CLIENT 子命令和外部客户端兼容仍未完成
Commands: ProtocolTest focused pass；未运行外部 client matrix
Residual risks: P3.3 remains open

Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: DEL/EXISTS 多 key、CLIENT 子命令状态和外部客户端矩阵未完成
Commands: ProtocolTest focused suite pass
Residual risks: P3 全部任务保持 [~]

Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: initial RESP SET/GET/DEL/type defects fixed and regression tests added; redis-cli/client-library matrix and malformed-prefix behavior remain pending
Commands: Debug focused protocol tests pass
Residual risks: P3.3 external client compatibility pending

Audit round: 21
Auditor: Agent B
Verdict: pass-with-risk
Findings: P3.2/P3.3 实现验收通过；P3.1 仅因 Clang/libFuzzer 在当前环境不可用保持未完成
Commands: ProtocolTest；ServerFixture；redis-cli 6.0.16、redis-py 6.4.0、Node redis 6.2.1、go-redis 9.7.0 实际 smoke
Residual risks: P3.1/P10.1 fuzz target 待具备 Clang/libFuzzer 的环境执行（Owner: Agent A）

Audit round: 22
Auditor: Agent B
Verdict: pass-with-risk
Findings: P3.1-P3.3 全部验收通过；无 critical/high
Commands: Clang 18/compiler-rt protocol_fuzz 10,000 runs；redis-cli 6.0.16、redis-py 6.4.0、Node redis 6.2.1、go-redis 9.7.0；Debug/ASAN/TSAN 77/77
Residual risks: P10.1 继续扩大协议 fuzz corpus 与运行时长（Owner: Agent A）
```

---

## P4：统一写事件、AOF 与快照

### [x] P4.1 写事件中心

- [x] 定义 `WriteEvent`：offset、event ID、origin node/source、command、key/value、时间和 checksum
- [x] 区分 `client/aof_replay/full_sync/incremental_sync` 来源
- [x] 明确事件提交点、存储可见点、AOF 确认点和复制发布点
- [x] 实现有界队列、背压和停机 drain
- [x] 实现传播策略，阻止回放重入 AOF 和同步回环
- [x] 测试并发生产顺序、重复事件、消费者失败和队列满

验收：所有变更命令恰好形成一个逻辑事件；offset 严格递增且恢复后不回退。

### [x] P4.2 AOF 格式与批处理

- [x] 冻结版本化 AOF record/frame 格式和 CRC
- [x] 按记录数、累计字节、时间任一阈值触发 flush
- [x] 独立实现 `always/everysec/no` sync 策略及错误上报
- [x] 处理 partial write、EINTR、ENOSPC、损坏尾记录和目录 fsync
- [x] 实现 replay，来源标记为 `aof_replay`
- [x] 实现 rewrite/compaction 或登记明确容量边界
- [x] 增加 kill -9、截断、bit flip、重复 replay 和磁盘满测试

### [x] P4.3 Snapshot

- [x] 定义 header：magic、version、flags、count、payload length、last offset、CRC32
- [x] 定义确定性 record 编码，不直接 dump C++ struct 内存布局
- [x] 使用临时文件、fsync 和原子 rename 发布快照
- [x] 实现 mmap 加载，并校验文件长度、边界、count 和 checksum
- [x] 实现 io_uring snapshot writer，保留同步 fallback 的明确选择配置
- [x] `SAVE/LOAD` 与后台任务并发时有清晰互斥/快照点语义
- [x] 测试空库、大对象、损坏 header、版本不兼容和中断发布

### [x] P4.4 恢复流程

- [x] 启动时加载最新合法 snapshot 后回放更高 offset 的 AOF
- [x] 拒绝 offset 回退、重复或不连续记录，或按文档化策略处理
- [x] 暴露恢复阶段、进度、耗时和失败原因
- [x] 恢复期间 readiness 为 false，不接受普通写请求
- [x] 建立不同崩溃点的恢复矩阵测试

验收：确认成功的写在配置承诺范围内可恢复；损坏数据不会静默加载；恢复结果与参考模型一致。

工作记录：
```text
Task IDs: P4.1, P4.2, P4.3, P4.4
Owner: Agent A
Dependencies: P1.1, P0.3, P0.4
Scope: 本轮重做统一写事件提交/失败回滚、二进制版本化 AOF frame、显式 snapshot header/mmap load，以及严格 offset/event_id 恢复
Implementation decisions: AOF 使用二进制 AOF1 frame；snapshot header 包含 flags/count/payload length/last offset/last event ID/CRC；io_uring writer 配置启用时优先使用，能力不足时按配置允许同步 fallback；损坏尾记录、回退、重复和断档一律拒绝
Current scope after audit convergence: 补 WriteEvent checksum/origin、有界事件消费与 drain、AOF sync/failure matrix、snapshot 边界/发布测试、恢复状态和 restart reference-model matrix
Condition evidence: WriteEvent offset/source, AOF AOF1+CRC+fdatasync/replay, snapshot magic/version/count/CRC/atomic rename, startup restore
Changed files: include/kvstore/persistence/, src/persistence/, src/server/main.cpp, tests/unit/persistence_test.cpp
Commands: ctest --test-dir build --output-on-failure -R PersistenceTest; live restart smoke pending rerun
Test result: snapshot/AOF unit tests pass; crash/ENOSPC/full recovery matrix pending
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: initial AOF format/sync/replay offset and snapshot metadata issues; AOF CRC/monotonic replay/fdatasync and recovered offset fixes added, snapshot full metadata/dir fsync pending
Residual risks: P4.3/P4.4 crash and full recovery matrix pending

Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: AOF metadata CRC, monotonic sequence, fdatasync, replay offset filtering and INCR delta fixes added; timer sync, directory/file fsync, snapshot metadata and crash matrix remain pending
Commands: PersistenceTest focused suite pass
Residual risks: P4.3/P4.4 durability and recovery matrix pending

Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: event sink failure side effects、完整 event_id 恢复、snapshot header/count durability、AOF partial write/size/crash matrix 未完成
Commands: PersistenceTest focused pass；ASAN/TSAN 40/40
Residual risks: P4.1-P4.4 remains open

Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: sink failure事务边界、event_id replay、AOF partial write rollback/size limit、snapshot完整header与crash矩阵仍未完成
Commands: PersistenceTest focused suite pass；ASAN/UBSAN/TSAN 40/40
Residual risks: P4 全部任务保持 [~]

Audit round: 4
Auditor: Agent B
Verdict: fail
Findings: high dispatcher 多 key DEL 部分提交；high AOF fdatasync 失败残留与恶意长度分配；high epoll 超时/背压/停机 drain；medium snapshot-AOF gap、CLIENT 状态和配置约束；缺少故障注入与 socket 回归
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure (40/40); git diff --check
Residual risks: 已修复大部分代码路径，批量事件事务与自动化故障/socket 测试待 round 5

Audit round: 5
Auditor: Agent B
Verdict: fail
Findings: high src/command/dispatcher.cpp:108 多 key DEL 在第 N 个事件 sink 成功后后续 sink 失败时，已发布事件无法回滚；high src/net/epoll_server.cpp:103 停机 drain 仍可能接收并执行新请求；high src/net/epoll_server.cpp:215 单次 pipeline 在水位检查前可执行大量命令；medium partial frame parse timeout 状态不准确；medium CLIENT 子命令状态注入大小写；medium src/persistence/aof.cpp:108 ftruncate 失败处理缺少 durability 证明；缺少对应自动化回归
Commands: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON; cmake --build build -j2; ctest --test-dir build --output-on-failure (40/40); focused ProtocolTest/PersistenceTest (9/9); git diff --check
Residual risks: P2-P4 保持 [~]；下一轮扩展 EventSink 批量提交接口并增加故障注入、真实 socket、TSAN/ASAN 回归

Audit round: 6
Auditor: Agent B
Verdict: fail
Findings: high epoll 在 max_inflight_requests 截断后静默丢弃已解析命令；high Batch 粘连 frame 被单帧 max_frame 限制；high 多 key DEL 引擎中途失败不回滚；high AOF partial write 回滚结果未验证；medium AOF Append 未验证跨调用连续性；medium partial frame parse timeout 状态错误；medium input buffer 限制未覆盖 parser/pipeline；medium listen address 格式未在配置阶段校验
Commands: Debug/ASAN/UBSAN/TSAN build and ctest; focused protocol/persistence tests; git diff --check
Residual risks: 已修复主要代码路径；需 round 7 复核 pending queue、Batch 累积上限、AOF rollback/durability 和 socket 故障注入

Audit round: 7
Auditor: Agent B
Verdict: fail
Findings: critical AOF sync/rollback failure仍可能留下已提交 frame；high malformed RESP/native 访问失败 Result value 导致事件循环异常；high pending queue 在仅 EPOLLOUT 时无 drain；high graceful shutdown 不排空 pending；high Batch 处理仍可能超过输出上限后丢响应；high 多 key DEL rollback 与外部 batch sink 原子性未证明；medium Batch 及 parser 输入上限/事件 ID 连续性；format-check 未通过
Commands: Debug/ASAN/UBSAN/TSAN build；ctest 40/40；focused protocol/persistence；git diff --check；format-check failed
Residual risks: P2/P3/P4 继续保持 [~]；需要独立修复 AOF durability、epoll pending drain/shutdown、Batch 输出事务和故障注入回归

Audit round: 8
Auditor: Agent B
Verdict: fail
Findings: critical AOF write/sync/truncate 失败后不能证明残留 frame 不可恢复；high Batch 单 frame 超过 max_inflight_requests 时丢余下命令；high pending queue 无连接级总量上限；high Batch 累积与输出编码可绕过内存/输出水位；high 多 key DEL fallback sink 仍非原子；high AOF 总记录大小检查下溢；medium 输入上限减法下溢、snapshot 边界 event_id 对应关系、metrics/upstream 地址校验、超大 timeout；format-check 失败；缺少真实 socket/故障注入回归
Commands: Debug build+ctest 40/40; ASAN/UBSAN build+ctest 40/40; TSAN build pass, ctest discovery blocked by environment mapping; focused Protocol/Persistence/Cli 11/11; format-check failed; git diff --check
Residual risks: P2/P3/P4 保持 [~]；需要先闭环 AOF 提交协议、pending/Batch 有界调度和故障注入测试，再请求下一轮审计

Audit round: 9
Auditor: Agent B
Verdict: fail
Findings: critical AOF sync_policy=no 下 Append/Flush 可能留下 flags=0 frame 而 replay 强制 flags=1；high Batch sink 无事务 prepare/commit/rollback 契约；high Batch/RESP 达到 max_inflight 后仍存在消费后丢弃；high shutdown 只做一次 pending drain；high AOF commit sync 失败仍依赖 truncate；high snapshot 与 AOF 长度边界校验不足；medium writer 重启未初始化末尾 offset/event_id；medium snapshot 资源上限、metrics/upstream 地址和 timeout 范围不足；缺少故障/socket 测试
Commands: Debug build+ctest 40/40; ASAN/UBSAN build+ctest 40/40; TSAN discovery/binary blocked by environment mapping; focused tests 11/11; format-check passed; git diff --check passed
Residual risks: P2/P3/P4 保持 [~]；下一轮必须先完成 AOF committed/no-sync 语义、严格有界 pending/Batch 调度、shutdown 循环 drain 和恢复边界矩阵

Audit round: 11
Auditor: Agent B
Verdict: fail
Findings: critical AOF marker/crash and batch atomicity; critical no/everysec sync semantics; high AOF rollback durability; high pending queue byte bounds and output budget; high BatchEventSink no transaction contract; medium persistence/network flags silently ignored; medium snapshot/AOF boundary; medium malformed connection lifecycle; format-check failure in aof.hpp
Commands: Debug/ASAN/UBSAN/TSAN build and ctest; TSAN setarch ctest 40/40; focused 20/20; format-check failed; git diff --check passed
Residual risks: P2/P3/P4 remain [~]; next scope is transactional AOF envelope, bounded output/pending, explicit unsupported configuration rejection, malformed socket closure and failure-injection coverage

Audit round: 12
Auditor: Agent B
Verdict: fail
Findings: critical epoll probe temporary string_view UAF；critical AOF 逐 marker 无 batch crash atomicity；high marker/rollback durability；high everysec 无 idle timer；high output budget检查晚；high pending queue按命令而非字节且高水位时反向保留 EPOLLIN；high malformed parser连接未进入terminal；high online LOAD复活旧AOF状态；medium多项配置开关静默忽略
Commands: Debug/ASAN/UBSAN 40/40; TSAN environment mapping failure with setarch direct pass; format-check pass; git diff --check pass
Residual risks: 已修复 UAF、错误连接 terminal 标记和 LOAD 拒绝；其余风险进入 round 13

Audit round: 13
Auditor: Agent B
Verdict: fail
Findings: critical AOF batch/crash atomicity；critical AOF rollback durability；high everysec 无后台定时 flush且阈值失效；high Batch 输出预算仍可导致执行后丢响应；high高水位且 pending 时仍启用 EPOLLIN；high multi-key DEL sink无事务契约；medium配置开关/恢复边界/真实 socket 与故障注入覆盖不足
Commands: Debug/ASAN/UBSAN 40/40; TSAN discovery blocked by environment and setarch ctest 40/40; format-check pass; git diff --check pass
Residual risks: P2/P3/P4 保持 [~]；下一轮必须完成事务 envelope、durability failure state、字节级 admission/backpressure、完整配置拒绝策略与真实 socket/故障矩阵

Audit round: 14-16
Auditor: Agent B
Verdict: fail
Findings: 事务 AOF 未提交尾恢复、uncertain commit、snapshot event boundary、无 sink 序列推进、截断 header ASAN 越界，以及 pending admission 先入队后拒绝；均在 round 17 前修复
Commands: Debug/ASAN/UBSAN/TSAN 逐轮 build+ctest；format-check；git diff --check
Residual risks: round 17 前仅剩真实 socket admission/terminal-close 自动化覆盖

Audit round: 17
Auditor: Agent B
Verdict: pass-with-risk
Findings: medium tests/unit/protocol_test.cpp:96 缺少真实 epoll socket 回归，尚未自动验证 pending 参数字节超限时不入队、不 dispatch、返回 BUSY 后 EOF；静态审查未发现 critical/high
Commands: Debug 45/45；ASAN/UBSAN 45/45；TSAN setarch 45/45；format-check pass；git diff --check pass
Residual risks: P2.4/P10.1，Owner: Agent A；补充 RESP/Native/Batch 真实 socket admission/terminal-close 测试，断言超限请求 dispatch 次数为零

Audit round: 18-20
Auditor: Agent B
Verdict: fail
Findings: response budget 误拒绝、AOF 尾事务恢复、flush 阈值、queue deadline、跨协议限额、Native missing 状态、input 减法下溢及 multi-key DEL sink 原子性；均在 round 21 前修复
Commands: 每轮 Debug/ASAN/UBSAN/TSAN build+ctest；format-check；git diff --check
Residual risks: 修复后进入 round 21

Audit round: 21
Auditor: Agent B
Verdict: pass-with-risk
Findings: P4.1/P4.2/P4.4 实现验收通过；P4.3 同步 snapshot 完成，io_uring writer 尚未实现，仅提供显式同步 fallback
Commands: Debug 73/73；ASAN/UBSAN 73/73；TSAN setarch 73/73；format-check pass；git diff --check pass
Residual risks: P4.3，Owner: Agent A；实现并验证 io_uring snapshot writer；P2.4/P10.1 补专项 socket 压力矩阵；P2.2/P3.1/P10.1 在 Clang 环境运行 fuzz

Audit round: 22
Auditor: Agent B
Verdict: pass-with-risk
Findings: P4.1-P4.4 全部验收通过；无 critical/high；io_uring direct 与 fallback 能力测试通过，fallback 诊断措辞为非阻塞 low
Commands: Debug 77/77；ASAN/UBSAN 77/77；TSAN setarch 77/77；io_uring capability/fallback repeat 10；format-check；git diff --check
Residual risks: P9.3，Owner: Agent A；仅在实际 capability probe 失败并发生 fallback 时输出 unavailable 诊断
```

---

## P5：复制与可替换异步后端

### [ ] P5.1 同端口握手与连接状态机

- [ ] 定义版本化 replication handshake、认证占位、node ID、role 和能力协商
- [ ] 在同一端口区分 client 和 peer，设置握手大小及超时
- [ ] 按 fd 管理状态：accepted/handshake/full-sync/catch-up/online/closing
- [ ] 正确处理 fd 复用、partial frame、重连和优雅关闭
- [ ] 增加伪造握手、版本冲突、慢握手和断连测试

### [ ] P5.2 1024 槽增量 backlog

- [ ] 实现固定 1024 槽 ring，每槽保存 offset、边界和完整事件
- [ ] 检测 wrap/覆盖/请求 offset 过旧并触发全量同步
- [ ] 明确大于单槽事件的拒绝或外部 payload 策略
- [ ] 实现并发 producer/consumer 可见性与慢副本处理
- [ ] 对边界 offset、wrap、多副本和断档增加测试

### [ ] P5.3 Primary/replica 全量与增量同步

- [ ] primary 在一致 snapshot offset 上生成/选择快照
- [ ] snapshot 传输期间保留后续增量并在完成后追平
- [ ] replica 原子安装全量数据，来源标记为 `full_sync`
- [ ] replica 从确认 offset 接收增量，来源标记为 `incremental_sync`
- [ ] 实现 ACK、heartbeat、超时、重传/重连和状态指标
- [ ] 防止 stale primary、重复事件和 offset 分叉静默覆盖
- [ ] 测试同步期间持续写、断线、primary 重启和 backlog 覆盖

### [ ] P5.4 LiveSync 回环抑制

- [ ] 使用 origin node ID + event ID 识别已见事件
- [ ] 定义去重窗口容量、过期和重启后的行为
- [ ] 支持合法多跳传播并抑制 A->B->A 回环
- [ ] 使用 2/3 节点拓扑验证无无限传播、无漏写

### [ ] P5.5 复制执行后端

- [ ] 定义 backend-neutral replication transport/task 接口
- [ ] 实现 pthread 后端作为正确性基线
- [ ] 实现 reactor 后端并接入现有事件循环
- [ ] 实现 proactor/io_uring 后端
- [ ] 实现 ntyco 协程后端
- [ ] 对四后端运行同一状态机、断线重连和吞吐测试
- [ ] 创建 `docs/ebpf-sockmap.md`，仅定义接入点、约束和安全模型
- [ ] 创建 `docs/rdma.md`，仅定义内存注册、传输和 fallback 接口

验收：后端切换不改变复制语义；全量后持续写入最终收敛；TSAN 和故障注入通过。

工作记录：待开始时填写。

---

## P6：io_uring 与 ntyco 网络后端

### [ ] P6.1 io_uring 服务后端

- [ ] 探测内核能力并在配置要求不满足时明确失败
- [ ] 实现 accept/recv/send/cancel completion 生命周期
- [ ] 处理 multishot、buffer ownership、CQ overflow 和 shutdown race
- [ ] 实现与 epoll 等价的背压、限额和超时
- [ ] 运行同一网络/协议一致性测试及性能对比

### [ ] P6.2 ntyco 协程服务后端

- [ ] 固定 ntyco 依赖版本并审计许可证/维护状态
- [ ] 明确 coroutine 栈、连接对象和 engine task 生命周期
- [ ] 实现取消、超时、背压和优雅停机
- [ ] 运行同一网络/协议一致性测试及性能对比

验收：三种服务后端均通过相同端到端测试；配置选择失败不静默 fallback，除非配置明确允许。

工作记录：待开始时填写。

---

## P7：Attention KVCache 模型与分级存储

### [x] P7.1 KVCache 数据与 key 规范

- [x] 定义 tensor manifest：model/adapter/tenant/token hash/layer/dtype/shape/layout/device
- [x] 定义 chunk 大小、对齐、压缩可选项和每 chunk checksum
- [x] 定义模型升级、adapter 变化和 tokenizer 变化的失效规则
- [x] 实现 canonical cache key，防止跨模型/租户错误命中
- [x] 实现 metadata 与 chunk 生命周期的原子关联
- [x] 增加 key 碰撞、元数据不兼容和损坏 chunk 测试

验收：任意命中都能证明张量兼容；不完整对象不可见；删除可回收所有关联 chunk。

### [x] P7.2 Match 与索引

- [x] 实现 exact match
- [x] 实现 token/prefix hash 的最长前缀 match
- [x] 返回命中 token 数、命中层/块和缺失范围
- [x] 处理 hash 碰撞，可用 token 摘要/二次校验确认
- [x] 为索引更新、驱逐和并发查询定义一致性
- [x] benchmark 不同 prefix 长度、并发度和对象规模

### [x] P7.3 内存/磁盘分级状态机

- [x] 定义 resident/loading/evicting/disk-only/failed 状态与合法转换
- [x] 实现内存 slab/pool、预算、碎片统计和高低水位
- [x] 实现磁盘 chunk store、空间配额、回收和校验
- [x] 实现 memory->disk 降级和 disk->memory 提升
- [x] 合并同一对象并发 load，等待者可超时/取消
- [x] 迁移期间 pin 活跃对象，防止 use-after-free 或重复驱逐
- [x] 对磁盘满、短读、checksum 错误、取消和进程重启增加测试

### [x] P7.4 决策与调度

- [x] 收集 recency、frequency、size、load cost、recompute cost 和 reuse distance
- [x] 建立可解释准入分数，首版基线可采用 cost-aware LRU/GDSF
- [x] 区分 prefill 热对象、decode 活跃对象和低复用对象
- [x] 调度 load/match/evict 队列，设置并发度、优先级和 I/O 配额
- [x] 在 deadline 前预计无法加载时快速 miss 并允许推理端重算
- [x] 防止大对象扫描、cache pollution 和 tenant 饥饿
- [x] 支持策略参数配置和运行指标，不在线上热路径同步训练策略

验收：策略决策可由指标解释；在基准 trace 上优于纯 LRU 基线，且尾延迟无不可接受回归。

### [x] P7.5 请求到达时的内存快取路径

- [x] 实现单次 lookup 返回 resident handle，避免额外 value copy
- [x] 对命中 prefix 只调度缺失 token/layer 的计算或加载
- [x] 合并相同 prefix 的并发 miss，防止重复磁盘 I/O/重算
- [x] 支持 request priority、deadline 和取消传播
- [x] 记录 exact/prefix/memory/disk/miss/coalesced 命中分类

验收：resident hit 不触发磁盘 I/O；并发相同请求只产生一次 load；handle 生命周期覆盖推理消费。

工作记录：

```text
Task ID: P7.5
Owner: Agent A
Dependencies: P7.2, P7.3, P7.4
Current repair scope: Agent A addressing P7.5 failure injection and async request ownership: copied per-thread LookupRequest values, deterministic one-shot scheduler Submit/Pop/Complete failures, exactly-once future completion and retry/counter regression tests. Independent follow-up audit remains required; keep [~].
Current flaky-test repair: Agent A; dependencies P7.2, P7.3, P7.4 and existing RequestPath/SchedulerState APIs. Add a private test-only worker gate so two distinct operations are queued before the single fail_pop is consumed; preserve production Pop-error draining. Acceptance: both queued requests return kInternal, retries on the same RequestPath succeed, pending/inflight bytes are zero after failure and retries, and the filtered Debug/TSAN test runs at least five times where feasible. Status remains [~]; independent audit pending, no audit pass claimed.
Changed files: include/kvstore/kvcache/request_path.hpp, src/kvcache/request_path.cpp, tests/unit/kvcache_request_path_test.cpp, todolist/todolist.md
Commands: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON; cmake --build build -j2; filtered ctest x5; cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=thread; cmake --build build-tsan -j2; setarch "$(uname -m)" -R filtered ctest x5
Test result: Debug 5/5 and TSAN 5/5 passed; no TSAN race reports. The test now barriers on SchedulerState().pending == 2 before releasing the worker, asserts both single-Pop-failure results are kInternal, verifies both retries succeed, and checks pending/inflight bytes are zero after each phase.
Audit round: 11
Auditor: Agent B
Verdict: fail
Findings: high `src/kvcache/request_path.cpp:26-28` started `worker_` during member initialization, allowing the worker to access `worker_paused_for_test_` and synchronization state before construction completed; confirmed RequestPath constructor race.
Commands: pending remediation verification; required Debug full relevant tests and repeated setarch TSAN RequestPath failure suite
Residual risks: Agent A moved worker startup into the constructor body after all members are initialized; P7.5 remains [~] and no audit pass is claimed. Independent follow-up audit required.
Remediation update: Agent A moved thread startup from the initializer list to the constructor body and explicitly initializes `worker_paused_for_test_` before starting the worker. API and tests are preserved.
Remediation evidence: `cmake --build build -j2` passed; the relevant Debug ctest invocation reached 41/50 tests before the command timeout in the existing long-running coalesced disk-read case, so full relevant Debug completion remains pending. `cmake --build build-tsan -j2` passed; `setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure --repeat until-fail:5 -R 'KvCacheRequestPath(SchedulerFailureTest|RequestPathTest\\.(PopFailureDrainsDistinctPendingOperationsAndRetrySucceeds|CompleteFailureIsSharedByCoalescedWaitersAndRetrySucceeds))'` passed all three failure variants for five repetitions (15/15), with no TSAN reports.
Scope: request-path lookup handle、prefix missing ranges、相同请求合并、priority/deadline/cancel 和分类指标；接入 MatchIndex/TieredStore/Scheduler
Implementation status: in progress; Agent A remediation implementation complete, pending independent Agent B audit
Changed files: CMakeLists.txt, include/kvstore/kvcache/policy.hpp, src/kvcache/policy.cpp, include/kvstore/kvcache/request_path.hpp, src/kvcache/request_path.cpp, tests/unit/kvcache_request_path_test.cpp, todolist/todolist.md
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'
Test result: build passed; focused KVCache tests 38/38 passed. Comprehensive P7.5 behavioral tests and independent audit remain pending.
Round 9 update: Added bounded shutdown race, distinct-operation Pop-drain, and coalesced Complete-failure tests; P7.5 remains [~] pending Agent B audit.
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: request error path accessed Result value unsafely; coalesced waiters were not cancellation/deadline aware; priority and scheduler were bypassed; metrics semantics and tests were incomplete
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R KvCacheRequestPathTest
Residual risks: entered remediation and round 2

Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: scheduler completion is not exception/cancellation safe; prefix missing ranges are ignored; scheduler estimates and concurrent priority behavior are superficial; coalescing key and overlapping metric semantics remain unsafe; behavioral tests are absent
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R 'KvCacheRequestPathTest|KvCachePolicySchedulerTest'
Residual risks: P7.5 remains incomplete; Agent A must implement true range loading, safe scheduler lifecycle, precise coalescing/classification semantics, and comprehensive tests
Decisions: user accepted StatusCode::kDeadlineExceeded and TieredStore range-load extension; metrics use separate match/storage dimensions; cancellation is waiter-local and shared operation cancels only after its last waiter leaves
Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: range handle metadata is invalid for chunk-intersecting partial tensors; coalescing ignores ranges and waiter attributes; scheduler is synchronous and lacks safe task ownership/completion; cancellation/last-waiter propagation is incomplete; behavioral request-path tests are absent
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'
Residual risks: redesign required before round 4: preserve source addressing for range handles or introduce an explicit range view, use range-aware operation keys and waiter registry, add RAII scheduler task ownership, and add behavioral tests
Audit round: 5
Auditor: Agent B
Verdict: fail
Findings: prefix worker loads the matched source object and does not invoke LoadRanges; shutdown can block on active max-deadline load; active last-waiter cancellation and scheduler completion failure recovery are unproven; coalesced priority policy and core concurrent behavior lack tests
Commands: cmake --build build -j; ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|Policy|TieredStore)'
Residual risks: semantic clarification required: missing prefix suffix is recompute work and cannot be loaded from the shorter source object; LoadRanges applies only to ranges within the matched source object. Then add bounded operation deadline/shutdown, completion recovery, priority policy, and concurrency tests
Audit round: 8
Auditor: Agent B
Verdict: pass-with-risk
Findings: medium shared operation bound may retain short-deadline work; synchronous filesystem calls cannot be interrupted and Shutdown joins; scheduler failure injection remains untested; timing-based concurrency tests may vary; range view requires callers to interpret physical indices explicitly
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|Match|TieredStore|Policy)'
Residual risks: Agent A to add scheduler Submit/Pop/Complete failure tests, document blocked filesystem shutdown acceptance, run ASAN/UBSAN/TSAN focused tests, and reconcile P7.5 checkboxes/evidence; P9.2/P10.1 risks remain registered
Audit round: 17
Auditor: Agent B
Verdict: pass-with-risk
Findings: none
Commands: Debug focused 52/52; ASAN/UBSAN focused 52/52; TSAN focused 52/52; repeated TSAN failure/coalescing 45 executions passed; git diff --check
Residual risks: synchronous filesystem calls remain non-interruptible, so Shutdown latency can exceed shutdown_bound_; owner Agent A, tracked for future I/O backend work
Validation update: ASAN/UBSAN focused 44/44 passed; TSAN RequestPath 6/7 passed and LastWaiterCancelsAndShutdownIsRepeatable exposed a test-side race from reassigning a shared stop_token while Lookup reads it (tests/unit/kvcache_request_path_test.cpp:222); P7.5 remains [~] pending race fix and re-audit

Implementation update: Agent A implemented `TieredStore::LoadRanges` with manifest/range validation and physical chunk-aligned selection; added focused range-loading regression coverage. Pending Agent B audit.
Changed files: src/kvcache/tiered_store.cpp, tests/unit/kvcache_tiered_store_test.cpp

Remediation update: Agent A preserved the source KVD1 manifest for range handles, added `TensorRangeView` with requested ranges and physical chunk indices, and made request coalescing keys range-aware.
Changed files: include/kvstore/kvcache/tiered_store.hpp, src/kvcache/tiered_store.cpp, src/kvcache/request_path.cpp, tests/unit/kvcache_tiered_store_test.cpp
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R 'KvCache(TieredStore|RequestPath)'
Test result: build passed; focused KVCache tests 23/23 passed
Audit round: 4
Auditor: Agent B
Verdict: pending
Findings: pending
Commands: pending
Residual risks: pending

Implementation update: Agent A added source-contained LoadRanges selection, operation deadlines with bounded shutdown cancellation, and max-priority/earliest-deadline waiter aggregation. Existing focused tests pass; requested concurrent/failure tests remain outstanding.
Changed files: include/kvstore/kvcache/policy.hpp, include/kvstore/kvcache/request_path.hpp, src/kvcache/request_path.cpp, todolist/todolist.md
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R 'KvCache(RequestPath|TieredStore|Policy)'
Test result: build passed; focused KVCache tests 32/32 passed
Audit round: 5 follow-up
Auditor: Agent B
Verdict: pending
Findings: pending independent audit; substantive concurrency, shutdown, scheduler failure and coalescing tests still required
Commands: pending
Residual risks: underlying TieredStore calls remain synchronous; scheduler priority updates are retained in operation state but cannot mutate an already queued scheduler task

P7.5 remediation update: Agent A made coalesced operation priority/deadline immutable after submission. Scheduler requests receive copied owner values; waiter attributes do not aggregate or reprioritize queued work. The shared operation uses an independent bounded load deadline; waiter deadlines remain local. Shutdown requests stop and join the owned worker. Underlying synchronous filesystem calls cannot be interrupted by a stop token, so no hard shutdown completion bound is claimed.

Repair update: Agent A fixed LoadRanges physical offset calculation for each planar K/V plane and updated the range test to expect both intersecting physical chunks.
Changed files: src/kvcache/tiered_store.cpp, tests/unit/kvcache_tiered_store_test.cpp, todolist/todolist.md
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure -R KvCacheRequestPathTest
Test result: Debug focused RequestPathTest 7/7 pass; tests cover source-only prefix/recompute plan, disk metrics, coalescing, survivor deadline, last-waiter cancellation, and repeated shutdown.
Audit round: 6
Auditor: Agent B
Verdict: pending
Findings: pending
Commands: pending
Residual risks: pending
```

```text
Task ID: P7.1
Owner: Agent A
Dependencies: P0.1, P1.1
Scope: 定义版本化 tensor manifest 与 canonical key，建立独立 resident chunk pool，并实现 metadata/chunk 原子发布和回收
Implementation decisions: KVCacheService 独立于 IEngine 的 owned-copy API；canonical 编码使用版本化长度前缀二进制格式和 SHA-256，chunk 使用 CRC32；首版磁盘层采用目录分片的不可变内容寻址文件；压缩首版为 none 并保留版本化枚举
Downstream decisions: P7.4 实现 LRU 基线与 GDSF 默认策略、priority/deadline 队列及 tenant DRR；P8.1 使用 Protobuf+UDS 和 pinned CPU staging；vLLM 0.29.0 使用 KVConnectorBase_V1，SGLang 0.5.19 使用 dynamic HiCacheStorage interface_v1；Qwen2.5-0.5B 固定 revision 作为共同基准
Condition evidence: TensorManifest 覆盖 model/adapter/tenant/token/layer/dtype/shape/axis/stride/layout/packing/cache ABI/device/topology/timestamp/checksum；KVC1 canonical 编码和 SHA-256 key；64-byte 对齐、CRC32 chunk、整对象 SHA-256；有硬计数/逻辑尺寸限额及保守内存准入估算的 reserve/put/commit/abort/delete 与 shared resident handle；19 项 golden/collision/quota/layout/不兼容/损坏/并发生命周期测试
Changed files: CMakeLists.txt, include/kvstore/kvcache/model.hpp, include/kvstore/kvcache/chunk_registry.hpp, src/kvcache/model.cpp, src/kvcache/chunk_registry.cpp, tests/unit/kvcache_model_test.cpp, docs/kvcache-format.md, todolist/todolist.md
Commands: cmake Debug configure/build；ctest Debug 94/94；ASAN+UBSAN full 87/87 及 focused 19/19；TSAN setarch full 87/87 及 focused 19/19；format-check；git diff --check
Test result: Debug 94/94 pass；ASAN+UBSAN focused 19/19 pass；TSAN focused 19/19 pass；此前 sanitizer full 87/87 pass；format-check 和 git diff --check pass
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: high Lookup 仅凭 CacheKey 无法二次确认 manifest；high reservation/object 缺少 chunk/byte/count 硬上限；high layout 缺轴顺序/stride/KV packing/format ABI；medium handle 空值/越界错误模型不安全；medium 并发竞态、lookup collision 与 quota 测试不足；medium 缺完整 wire 顺序/枚举和 golden vector
Commands: Debug/ASAN+UBSAN/TSAN focused 各 10/10；format-check；git diff --check；实际 diff/API/docs 审查
Residual risks: none；阻塞项进入 Agent A 修复并请求 round 2
Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: high max_objects 可由多个 pending 后依次 commit 绕过；high 部分分配/KeyFunction 异常可穿透 Status API；medium layout/packing 与轴位置语义未约束；medium tracked budget 未包含 metadata/bookkeeping；medium 缺 commit/abort、quota 并发和失败路径测试
Commands: Debug/ASAN+UBSAN/TSAN focused 各 15/15；并发测试 repeat 100；format-check；git diff --check；独立 canonical encoder 校验 golden key
Residual risks: none；阻塞项进入 Agent A 修复并请求 round 3
Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: high 新增 P7 文件未 stage，审计以 clean git archive 无法复现（仓库流程未要求实现阶段修改 staging，四审按工作区产物复核）；medium tracked estimate 未充分计入动态 manifest/allocator 开销；medium 声称 block-major 满块被拒绝；low 缺 block-major 和 allocation rollback 回归
Commands: Debug full 95/95；focused Debug/ASAN+UBSAN/TSAN 各 18/18；并发 repeat 100；format-check；git diff --check；clean committed-tree archive configure
Residual risks: tracked estimate 已扩大且文档明确物理预算归 P7.3；block-major 满块原逻辑实际接受并新增 planar/interleaved 自动化回归；未获用户要求不修改 staging area
Audit round: 4
Auditor: Agent B
Verdict: pass-with-risk
Findings: low 缺 allocator partial-reservation/publication 确定性失败注入；当前 RAII 和显式 rollback 审查正确
Commands: fresh cached-dependency configure configure/build + full 96/96；Debug/ASAN+UBSAN/TSAN focused 各 19/19；并发生命周期 repeat 100；format-check；git diff --check
Residual risks: P10.1 增加 allocator fault-injection，Owner: Agent A；P7.3 实现物理 64-byte 对齐、allocator 硬预算和过期 reservation cleanup；P8.2-P8.4 真实 GPU 验收前由用户启动带 NVIDIA 支持的 Docker

Task ID: P7.2
Owner: Agent A
Dependencies: P7.1
Scope: 实现 exact/longest-prefix 索引、完整命中证明、命中层/块/缺失范围、并发更新一致性和 prefix benchmark
Implementation decisions: 索引按不含 token identity 的完整 tensor compatibility 分区；query 传入临时 token ID span 和严格递增 reusable lengths，由索引单遍计算 cumulative SHA-256 并验证完整 query digest；候选命中后二次比较完整 compatibility bytes 与 digest，更新/驱逐和查询由 shared mutex 线性化
Condition evidence: 有界 MatchIndex exact/longest-prefix；请求 token IDs 单遍重算 cumulative SHA-256 与完整 compatibility 二次校验；MatchResult 返回 hit token/layer/chunks/missing range；canonical-key-specific erase 和 shared_mutex 线性化 insert/erase/query；Release 54-case benchmark 覆盖 128/1024/4096 tokens、2/16/64 objects、1/4/16 readers、partial-hit/miss
Changed files: CMakeLists.txt, include/kvstore/kvcache/match_index.hpp, src/kvcache/match_index.cpp, tests/unit/kvcache_match_test.cpp, benchmarks/kvcache_match_benchmark.cpp, docs/kvcache-match.md, todolist/todolist.md
Commands: cmake Debug build；ctest Debug 101/101；ASAN+UBSAN focused 5/5；TSAN focused 5/5；Release kvcache_match_benchmark；format-check；git diff --check
Test result: 初版 Debug 101/101 pass；二审修复后 Debug/ASAN+UBSAN/TSAN focused 各 8/8 pass；Release 54-case matrix 0.022-0.502 M lookup/s single reader、0.052-1.86 M lookup/s at 16 readers；format/diff checks pass
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: high Entry identity 未包含 canonical key，物理 chunk 变体可误判重复且 Erase 删除错误对象；high lookup 可按超大 chunk_count 在锁内无界分配且 index/prefix 无上限；medium collision/layout/digest 测试被残留字段变化遮蔽；medium 并发生命周期覆盖不足；medium benchmark 三维耦合且仅 exact hit
Commands: Debug full 101/101；Debug/ASAN+UBSAN/TSAN focused 5/5；并发 repeat 100；Release benchmark 三次；format-check；git diff --check
Residual risks: none；阻塞项进入 Agent A 修复并请求 round 2
Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: high prefix digest 可由调用方伪造而未绑定 query token 序列；medium duplicate 判定晚于容量检查；medium variant-limit 与并发重叠证据不足；low benchmark 无 warmup/barrier/repeat
Commands: Debug full 104/104；修复前后 focused Debug/ASAN+UBSAN/TSAN 各 8/8；TSAN repeat 100；Release 54-case benchmark；format-check；git diff --check
Residual risks: none；阻塞项进入 Agent A 修复并请求 round 3
Audit round: 3
Auditor: Agent B
Verdict: fail
Findings: medium entry-limit 自动化证据仍与 variant-limit 耦合，删除 max_entries enforcement 后测试仍会通过；无 critical/high 实现缺陷
Commands: Debug full 104/104；Debug/ASAN+UBSAN/TSAN focused 各 8/8；并发 repeat 100；Release 54-case matrix；format-check；git diff --check
Residual risks: none；补不同 token length 的独立 max_entries 回归后请求 round 4
Audit round: 4
Auditor: Agent B
Verdict: pass
Findings: none
Commands: KvCacheMatchTest 8/8；format-check；git diff --check；审查当前 P7.2 实现、测试、benchmark、文档和 CMake
Residual risks: none

Task ID: P7.3
Owner: Agent A
Dependencies: P4.3, P7.1
Scope: 实现 resident/loading/evicting/disk-only/failed 状态机、64-byte aligned resident pool、不可变磁盘 chunk store、双向迁移、并发 load 合并与 pin 生命周期
Implementation decisions: resident pool 使用按 chunk size 分级的 64-byte aligned allocator 和硬预算；磁盘按 object key 分目录、chunk SHA-256 内容寻址，metadata 最后原子发布；同 key loading 使用共享状态/condition variable 合并，deadline/cancel waiter 不取消其他消费者需要的 I/O
Condition evidence: TierState 合法转换验证；ResidentPool 64-byte aligned 硬预算及 usage/fragmentation/peak/high-low watermark 统计；KVD1 metadata-last 原子发布、chunk CRC32/SHA-256 与整对象 SHA-256 校验、启动重开和未发布数据清理；resident/disk 双向迁移、同对象 load condition-variable 合并、waiter deadline/stop_token 独立取消、shared pin 跨 eviction/delete 生命周期；ENOSPC-equivalent write/metadata rename 故障注入与 quota/state rollback
Changed files: CMakeLists.txt, include/kvstore/kvcache/tiered_store.hpp, src/kvcache/tiered_store.cpp, tests/unit/kvcache_tiered_store_test.cpp, docs/kvcache-tiering.md, todolist/todolist.md
Commands: cmake --build build -j2；ctest --test-dir build --output-on-failure；cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=address,undefined；cmake --build build-asan -j2；ctest --test-dir build-asan --output-on-failure -R 'KvCache(TierState|ResidentPool|TieredStore)Test'；cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=thread；cmake --build build-tsan -j2；setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure -R 'KvCache(TierState|ResidentPool|TieredStore)Test'；clang-format --dry-run --Werror 新增 3 文件；git diff --check
Test result: Debug full 116/116 pass；首审修复后 Debug/ASAN+UBSAN/TSAN focused 各 14/14 pass；format-check pass；git diff --check pass
Known risks: synchronous promotion/publication I/O intentionally runs in caller thread；P7.4 负责基于本轮 high/low watermark 统计接入淘汰策略；未新增 allocator fault-injection（登记 P10.1，Owner: Agent A）
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: high Put 插入 loading/reserve 后异常可泄漏状态与 quota；high Delete 进入 evicting 后路径异常可永久卡住；high Delete 和新 shard 发布目录 fsync 不完整；medium 仅测逻辑 quota、缺 ENOSPC 写失败注入；medium 默认空 handle 可解引用 null；low metadata 尾长度减法可能下溢
Commands: KvCacheTieredStoreTest 10/10；静态审查 tiered store/state/pool/disk format
Residual risks: none；阻塞项进入 Agent A 修复并请求 round 2
Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: high delete fsync fault hook 抛异常可遗留 evicting；high restart 未验证完整 shard 路径且忽略重复 canonical key；medium 路径组件/symlink 与 O_NOFOLLOW 防护不足；medium promotion handle 使用 caller expected manifest 丢失 persisted checksum/timestamps；medium 缺 after-progress partial write/read 注入；low moved-from handle 有意保持有效但文档未说明
Commands: Debug/ASAN+UBSAN/TSAN focused 各 16/16；format-check；git diff --check；静态审查
Residual risks: none；阻塞项进入 Agent A 修复并请求 round 3
Audit round: 3
Auditor: Agent B
Verdict: pending
Findings: pending
Residual risks: none

Audit round: 4-7
Auditor: Agent B
Verdict: pass-with-risk
Findings: none blocking
Commands: P7.3 Debug/ASAN+UBSAN/TSAN focused 23/23 each; format-check; git diff --check
Residual risks: P10.1 allocator fault injection and mixed-size stress; P9.2 concurrent filesystem replacement protection

Task ID: P7.4
Owner: Agent A
Dependencies: P7.2, P7.3
Scope: 实现可解释的 LRU/GDSF 准入和驱逐评分、有界 priority/deadline load 队列、tenant 公平调度、I/O 并发与字节配额、快速 miss 和策略指标
Implementation decisions: ShouldAdmit 采用先计算后提交，拒绝仅更新 bounded rejection metric，不改变 clock/frequency/residency/tracked records；score 使用 long double 中间值并将正溢出 clamp 到 double max，NaN/负值/未知 enum 返回 Status；policy/scheduler 内部 mutex 线性化且 metrics 按值快照；Pop 清理 expired、跳过 I/O blocked request，并在 eligible 集合内按 priority/deadline/id 排序，tenant 采用 deterministic bounded quantum；deadline 使用 unsigned tick distance 避免 signed time_point subtraction overflow；移除 callback hook，data-only fail_activation 在状态变更前失败；Submit 成功插入前不 compact，Pop/Cancel no-throw prune 并修正 cursor；server runtime 装配归 P7.5
Condition evidence: policy.hpp/policy.cpp expose checked Result-based LRU/GDSF explanations, transactional admission, bounded scans/tracking, workload classes, internally synchronized value metrics, and bounded Submit/Pop/Complete/Cancel scheduling；tests independently cover rejected-state preservation, enum/non-finite/overflow, activation failure then Metrics no-deadlock/state leak, min/max deadline feasibility, blocked/expired selection, priority/deadline/fairness, cancellation, tenant metadata cleanup gauge, bounds and TSAN concurrency；74-request simulator computes actual misses/cost/p95 and Release gate requires GDSF cost/p95 improvement
Changed files: CMakeLists.txt, config/config.json, tools/compat/config.json, include/kvstore/config/config.hpp, src/config/config.cpp, include/kvstore/kvcache/policy.hpp, src/kvcache/policy.cpp, src/kvcache/tiered_store.cpp (format only), tests/unit/config_test.cpp, tests/unit/kvcache_policy_test.cpp, benchmarks/kvcache_policy_benchmark.cpp, docs/kvcache-policy.md, docs/p7-policy-benchmark.md, todolist/todolist.md
Commands: cmake Debug configure/build；ctest Debug focused and full；cmake ASAN+UBSAN configure/build + focused ctest；cmake TSAN configure/build + setarch x86_64 -R focused ctest；cmake Release build kvcache_policy_benchmark + execute；cmake --build build --target format-check；git diff --check
Test result: round 2 remediation Debug focused 16/16 pass；ASAN+UBSAN focused 16/16 pass；TSAN focused 16/16 pass including concurrent policy/scheduler test；repository format-check and git diff --check pass；prior full Debug 142/142 and Release deterministic trace remain valid: LRU requests=74 misses=15 cost=1302 p95=100, GDSF requests=74 misses=38 cost=137 p95=1
Known risks: validated KvCacheConfig is not yet constructed by server because P7.5 owns request-path/TieredStore integration；trace latency is deterministic model units and makes no wall-clock performance claim
Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: high ShouldAdmit rejected decisions mutate existing/tracked state；high Pop exception ordering can leak counters/reservations；high non-finite score overflow is not rejected or clamped and enum values are not validated；high policy and scheduler are not internally thread-safe and Metrics exposes mutable shared state by reference；high Pop lets a blocked/expired highest-ranked request hide eligible work；medium deterministic comparison hardcodes costs instead of simulating misses/latency and lacks p95 evidence；medium runtime config is parsed but not wired into server construction；low repository format-check fails in latest P7.3 tiered_store.cpp changes
Commands: Agent B static diff/API/test review and Debug focused execution
Residual risks: none；全部 findings 进入 Agent A round 1 修复，P7.4 保持 [~]
Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: high scheduler invokes arbitrary before_activate callback while mutex is held, permitting deadlock；high deadline subtraction can overflow for extreme steady_clock time_points；high Submit compacts tenant metadata before insertion, so allocation failure can leave cursor/queue state changed；medium Pop/Cancel pruning and cursor handling do not prove bounded tenant metadata；medium missing callback/Metrics deadlock regression, extreme deadline, and cleanup tests
Commands: Agent B static review of round 1 diff/API and focused test evidence
Residual risks: round 2 findings enter Agent A remediation；P7.4 remains [~]

Audit round: 3
Auditor: Agent B
Verdict: pass
Findings: none
Commands: Focused Debug/ASAN+UBSAN/TSAN 15/15 each；full Debug 143/143；Release kvcache_policy_benchmark；format-check；git diff --check
Residual risks: runtime policy/scheduler construction is assigned to P7.5；deterministic trace is not a wall-clock performance claim
```

---

## P8：vLLM 与 SGLang 集成

### [x] P8.1 版本化集成协议

- [x] 选择 adapter/sidecar 边界，定义 capability negotiation 和版本策略
- [x] 定义 lookup/reserve/put/get/release/abort 请求及 tensor descriptor
- [x] 明确 CPU pinned memory、CUDA IPC 或网络传输的首版路径
- [x] 定义超时、取消、部分命中、校验失败和回退重算行为
- [x] 支持 request/model/tenant trace context
- [x] 提供框架无关 mock client 和 contract tests

工作记录：

```text
Task ID: P8.1
Owner: Agent A
Dependencies: P0.1, P7.1, P7.2, P7.3, P7.4, P7.5 (completed headings; P7.5 audit round 17 pass-with-risk)
Scope: Protobuf+UDS versioned integration contract, framework-independent mock client and contract tests; CPU pinned staging ownership and fallback semantics; Docker daemon preparation for P8.2/P8.3.
Acceptance: six checkboxes above backed by schema/documentation and automated contract tests; independent Agent B audit required. Real framework/GPU integration remains P8.2-P8.4.
Changed files: CMakeLists.txt, proto/kvstore_integration_v1.proto, include/kvstore/integration/protocol.hpp, src/integration/protocol.cpp, tests/unit/integration_protocol_test.cpp, docs/p8-integration-protocol.md
Commands: `protoc --version` (unavailable); `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_BUILD_BENCHMARKS=OFF && cmake --build build -j2 && ctest --test-dir build --output-on-failure`; ASAN/UBSAN and TSAN configure/build with focused `Codec|Session` ctest filters
Test result: Full Debug 162/162 passed; ASAN/UBSAN 3/3 integration tests passed; TSAN 3/3 integration tests passed with setarch.
Design limitations: protobuf-generated bindings are unavailable because protoc/runtime are not installed; the checked-in schema and dependency-free codec provide the bounded executable contract. P8.1 defines framing and mock semantics, not a production UDS listener. Prefix matching remains implemented by the existing MatchIndex/RequestPath boundary and is not duplicated in Session.
Residual risks: P7.5 synchronous filesystem shutdown risk remains tracked for future I/O backend work; GPU support not yet verified.

Audit round: 1
Auditor: Agent B
Verdict: fail
Findings: high src/integration/protocol.cpp:84-118 foreign sessions can Put/Commit/Abort reservations and Get foreign tenant manifests; high :101-104 failed Commit loses tracking and leaks; high include/kvstore/integration/protocol.hpp:41 missing destructor cleanup and copy restrictions; high src/integration/protocol.cpp:88-89 tracking allocation lacks rollback; high :46 Feed allocates unbounded input before checking; medium :18 Encode uses 9 instead of 13 header bytes; high :112 Release is a no-op; high proto/kvstore_integration_v1.proto:5-7 lacks full manifest, requests/responses, generated codec/dispatch mock, prefix hit and recompute wire semantics; documentation does not establish precise pinned ownership. Locations refer to the audited first pass.
Commands: findings reproduced by independent auditor and supplied by parent; exact auditor commands not supplied.
Residual risks: all listed findings require remediation and a new independent audit; six premature acceptance checkboxes reset.

Remediation owner: fresh Agent A; P8.1 remains [~] pending independent audit. No audit pass is claimed.
Remediation dependencies: P0.1, P7.1-P7.5; Docker handled by parent, outside this change.
Remediation scope: real generated Protobuf request/response codec and executable mock dispatcher; session ownership, failure rollback, destructor cleanup, bounded framing, lease unpin, MatchIndex prefix and recompute semantics, regression and sanitizer tests.
Implementation: Protobuf 3.21.12 fetched and built using pinned archive SHA256; previous protoc-unavailable/dependency-free-codec limitation is superseded. Full 34-field manifest, capabilities, trace, deadline/cancellation, all operations, IDs, explicit statuses and hit/recompute ranges are generated and serialized. Session constructor binds authorized identity; registry IDs are owner-checked, lease IDs are process-unique and session-owned. Failed commit retains reservation and rolls back staged index; commit prepares lease metadata before publication. Preallocated tracking eliminates acquisition-time allocation; failure hook verifies rollback. Destructor/disconnect abort and unpin; Release never deletes cache data. Registry Abort and MatchIndex rollback are allocation-free cleanup paths.
Changed files: CMakeLists.txt; proto/kvstore_integration_v1.proto; include/kvstore/integration/protocol.hpp; src/integration/protocol.cpp; src/integration/mock_client.cpp; tests/unit/integration_protocol_test.cpp; docs/p8-integration-protocol.md; include/kvstore/kvcache/match_index.hpp; src/kvcache/match_index.cpp; src/kvcache/chunk_registry.cpp; todolist/todolist.md. Existing edits to request_path/tiered_store tests were left unchanged.
Condition evidence: capability/version tests and protocol doc; generated manifest/all-op request/response schema and dispatch; precise adapter-owned page-locked staging/copy/event ownership docs; exact/prefix/miss/deadline/cancel/checksum tests with wire ranges; authorized trace and cross-session/cross-tenant denial tests; executable kvstore_mock_client serialized publish/lookup/get/release workflow plus 21 unit contract tests. Six acceptance boxes intentionally remain unchecked pending independent review.
Commands: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON`; `cmake --build build -j4`; `ctest --test-dir build --output-on-failure`; `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON -DKVSTORE_SANITIZERS=address,undefined`; `cmake --build build-asan -j4`; `ctest --test-dir build-asan --output-on-failure -R 'Integration(Codec|Session|Mock)|KvCache(Model|Match|RequestPath)'`; equivalent build-tsan configure/build with `-DKVSTORE_SANITIZERS=thread`; `setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure -R 'Integration(Codec|Session|Mock)|KvCache(Model|Match|RequestPath)'`; `setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure --repeat until-fail:5 -R 'IntegrationSession'`; scoped clang-format -i and --dry-run --Werror; `cmake --build build --target format-check`; `git diff --check`.
Test result: full Debug 181/181 passed (18.75s); ASAN/UBSAN affected 64/64 passed (3.91s), no sanitizer reports; TSAN affected 64/64 passed (3.44s), no race reports; 16 Session tests repeated five times under TSAN, 80/80 executions passed (2.66s). Initial simultaneous build/test commands hit the tool's 120s total limit; completed with longer windows, without removing/skipping tests. All changed C++ files pass scoped clang-format and diff whitespace checks. Whole-repository format-check fails on pre-existing formatting in include/kvstore/kvcache/{request_path,tiered_store}.hpp and src/kvcache/{request_path,tiered_store}.cpp; these unrelated files were not reformatted.
Residual risks / handoff: independent Agent B round 2 required. P8.2/P8.3 (Owner: Agent A/parent) own real adapters, GPU validation and asynchronous transfer cancellation; this contract uses admission deadlines and between-operation upload cancellation, not preemptive synchronous commit. P9.2 (Owner: Agent A) owns production UDS peer authentication/listener and configurable service-wide quotas; constructor identity is supplied by trusted caller in this mock. P10.1 (Owner: Agent A) owns expanded protobuf fuzzing, upstream dependency sanitizer/upgrade review and pre-existing whole-tree formatting cleanup. Generated message code and project code are sanitizer-instrumented; upstream protobuf runtime/compiler are not, and emit their existing GCC AlignFail noreturn warning. External cache administration must coordinate registry eviction and index erase; no eviction or disk-load API is claimed here. No Docker changes made.
```

```text
Audit round: 2
Auditor: Agent B
Verdict: fail
Findings: high src/integration/protocol.cpp:191-236 and :295 FromProto requires a 32-byte payload digest and full ValidateManifest for LOOKUP as well as RESERVE, preventing real prefill queries whose tensor payload is not yet computed.
Commands: independent audit finding supplied by parent; exact auditor commands not supplied.
Residual risks: remediation and independent re-audit required; P8.1 remains [~] with all six acceptance boxes unchecked.
Round 2 remediation owner: Agent A
Dependencies: P0.1, P7.1-P7.5; Docker remains parent-owned.
Scope: separate query/publication manifest decoding, allow absent/zero query payload digest while rejecting malformed supplied lengths, retain full publication validation, verify serialized exact/prefix/miss responses return stored digest, update documentation and run full Debug plus affected ASAN/UBSAN/TSAN.
Implementation: FromProto takes explicit ManifestUse (publication by default); LOOKUP selects query validation through ValidateManifestIdentity and safely leaves absent payload digest zero-initialized. Nonempty payload digests must be exactly 32 bytes in both modes. RESERVE retains ValidateManifest and rejects absent/all-zero payload digests before acquiring registry state. Hit replies continue to use the resident source manifest and stored digest. Mock client now issues Lookup without a payload digest.
Changed files: include/kvstore/integration/protocol.hpp; src/integration/protocol.cpp; src/integration/mock_client.cpp; tests/unit/integration_protocol_test.cpp; docs/p8-integration-protocol.md; todolist/todolist.md.
Regression evidence: four added serialized tests cover absent/32-zero digests for exact and prefix hits and misses, full stored response manifest/digest and hit/recompute ranges, reservation rejection with zero pending/tracked bytes, malformed digest lengths 1/31/33 for both operations, and unchanged token/geometry/tenant query validation. Existing publication/commit checksum tests remain intact.
Commands: `cmake --build build -j4 && ctest --test-dir build --output-on-failure`; `cmake --build build-asan -j4 && ctest --test-dir build-asan --output-on-failure -R 'Integration(Codec|Session|Mock)|KvCache(Model|Match|RequestPath)'`; `cmake --build build-tsan -j4 && setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure -R 'Integration(Codec|Session|Mock)|KvCache(Model|Match|RequestPath)'`; `clang-format -i` and `clang-format --dry-run --Werror` on the four changed C++ files; `git diff --check`.
Test result: full Debug 185/185 passed (17.67s); affected ASAN/UBSAN 68/68 passed (4.14s), no sanitizer reports; affected TSAN 68/68 passed (2.53s), no race reports. Changed-file formatting and diff whitespace checks passed. Whole-tree pre-existing formatting limitation from round 1 is unchanged.
Handoff: independent Agent B round 3 required; no audit pass claimed. P8.1 remains [~] and all six acceptance boxes remain unchecked. Existing residual risks remain registered above; no Docker or unrelated file changes in this round.
```

```text
Audit round: 3
Auditor: Agent B
Verdict: pass-with-risk
Findings: none blocking; six P8.1 conditions satisfied within documented resident serialized mock scope. Query/publication validation, session ownership, rollback, RAII, bounded framing, generated Protobuf dispatch, prefix/recompute responses and lease release independently verified.
Commands: cmake --build build -j4; ctest --test-dir build --output-on-failure; cmake --build build-asan -j4; ctest --test-dir build-asan --output-on-failure -R 'Integration(Codec|Session|Mock)|KvCache(Model|Match|RequestPath)'; cmake --build build-tsan -j4; setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure -R 'Integration(Codec|Session|Mock)|KvCache(Model|Match|RequestPath)'; clang-format --dry-run --Werror include/kvstore/integration/protocol.hpp src/integration/protocol.cpp src/integration/mock_client.cpp tests/unit/integration_protocol_test.cpp; git diff --check
Test result: independent full Debug 185/185; ASAN/UBSAN 68/68; TSAN 68/68; no sanitizer reports; changed-file format and whitespace checks passed.
Residual risks: P8.2/P8.3 Owner Agent A: real adapters, GPU transfers/cancellation and RequestPath integration; P9.2 Owner Agent A: production UDS listener/authentication and configurable quotas; P10.1 Owner Agent A: protobuf fuzzing, upstream runtime/compiler sanitizer and upgrade review, pre-existing formatting cleanup. Accepted scope: external eviction must coordinate registry/index removal; resident mock does not claim disk loading or production transport.
Completion: P8.1 accepted after independent round 3; historical pending/fail statements above are superseded, not removed.

Docker preparation (Agent A): Windows Docker Desktop started using powershell.exe Start-Process; host Docker CLI verifies Client=29.4.0 Server=29.4.0 OS=linux and registered nvidia runtime. Linux docker info still fails because /var/run/docker.sock is absent; Windows CLI remains usable from WSL at /mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe.
Commands: docker info; systemctl status docker --no-pager; docker desktop start; powershell.exe -NoProfile -Command 'Start-Process "C:\Program Files\Docker\Docker\Docker Desktop.exe"'; host docker.exe info/version; host docker.exe run --rm --gpus all ubuntu:24.04 nvidia-smi
Test result: daemon enabled and host API responsive. GPU smoke could not start: Docker Hub manifest request timed out; no local images available. Registered nvidia runtime alone is not proof of GPU execution.
Follow-up: P8.2/P8.3 Owner Agent A: enable native WSL Docker integration if needed, resolve approved registry/proxy access and rerun GPU container smoke before framework acceptance. Do not claim GPU readiness or M7 completion.
```

### [~] P8.2 vLLM adapter

Runtime implementation work record (2026-09-13):
Owner: Agent A (current implementation context); independent audit: parent, not this agent.
Dependencies: P8.1 accepted round 3; local Qwen2.5-0.5B revision 060db6499f32faf8b98477b0a26969ef7d8b9987; RTX 4060 CUDA.
Scope: isolate vLLM 0.29.0 and SGLang 0.5.19 environments; inspect installed upstream APIs; implement genuine KVConnectorBase_V1/HiCache plugins and a core bridge; exercise real CUDA pinned-buffer/event ownership and sequential model E2E (miss, partial/full external hit, cancellation, timeout, restart). Add reproducible runtime requirements, tests, CMake registration and evidence. Preserve existing uncommitted adapters. P8.2/P8.3 remain [~]; no audit verdict is authored here.
Acceptance: actual framework callbacks must transfer this project's cached tensors; built-in caching or mock tests do not satisfy runtime acceptance. Record exact upstream/hardware failures and incomplete conditions without claiming completion.

Resume inspection (2026-09-13): Owner Agent A (resumed implementation context), dependencies and scope unchanged. No surviving test/server/installer processes; prior uncommitted C++ adapters preserved. `.venv` passes pip check but vLLM metadata is absent; `.venv-vllm` is a partially installed isolated environment. NVIDIA reports RTX 4060 Laptop 8188 MiB, driver 591.86, CUDA 13.1. Real Python plugins and framework E2E are not yet present. Parent independent audit remains required.

- [ ] 固定支持的 vLLM 版本和 KV cache layout
- [ ] 在 prefill 前查询 exact/prefix match
- [ ] 将命中块注入受支持的 block manager/connector 接口
- [ ] prefill 后异步发布可复用 KV，并在请求取消时清理 reservation
- [ ] 集成测试覆盖 miss、partial hit、full hit、超时和 server 重启

### [~] P8.3 SGLang adapter

- [ ] 固定支持的 SGLang 版本和 radix/prefix cache 边界
- [ ] 映射 SGLang prefix/radix 元数据到 canonical cache key
- [ ] 实现读取、发布、释放和失败回退
- [ ] 集成测试覆盖 miss、partial hit、full hit、超时和 server 重启

### [ ] P8.4 端到端计算节省评估

- [ ] 建立固定模型、GPU、prompt 数据集、并发度和输出长度基线
- [ ] 分别测量 cold miss、disk hit、memory prefix hit、memory full hit
- [ ] 记录 TTFT、TPOT、端到端延迟、GPU prefill 时间、磁盘/网络字节和 CPU
- [ ] 记录命中 token 数 `H`、总输入 token 数 `T`，报告 `H/T` token 重算避免率
- [ ] 使用 profiler 记录 baseline 与 cache hit 的 prefill FLOPs/GPU time 差值
- [ ] 报告吞吐提升 `(cached_qps / baseline_qps - 1) * 100%`
- [ ] 报告计算节省 `(baseline_prefill_gpu_ms - cached_prefill_gpu_ms) / baseline_prefill_gpu_ms * 100%`
- [ ] 至少运行 1 次 warmup 和 10 次测量，报告均值、p50/p95/p99 和方差
- [ ] 产出可复现实验报告，不能仅用 token 命中率替代真实性能收益

验收：至少一个 vLLM 和一个 SGLang 支持版本通过端到端测试；收益报告可由脚本在记录环境中复现。

工作记录：
```text
Task ID: P8.2
Owner: Agent A
Dependencies: P8.1
Scope: Qwen2.5-0.5B vLLM 0.29.0 adapter; KVConnectorBase_V1 integration boundary, exact/prefix lookup, publication lifecycle, cancellation and contract tests.
Status: in progress; real GPU/container validation pending Docker image access.
Implementation start: Agent A; P8.1 round 3 accepted. Scope limited to include/kvstore/integration/vllm, src/integration/vllm, dedicated tests/docs and CMake registration. Implement serialized Session transport, fixed single-rank full-block CPU staging contract, validated hit injection and cooperatively advanced asynchronous publication with reservation cleanup. Acceptance for this round: executable mock coverage of miss/partial/full hit, deadlines, cancellation, metadata rejection and restart/persistence boundary; no real vLLM/GPU E2E claim. Keep [~] and acceptance boxes unchecked pending independent Agent B audit.

Resource validation: Qwen2.5-0.5B downloaded through `HF_ENDPOINT=https://hf-mirror.com` into `artifacts/models/Qwen2.5-0.5B`; mirror revision `060db6499f32faf8b98477b0a26969ef7d8b9987`, `model.safetensors` 988097824 bytes. `config.json` reports qwen2, 24 layers, BF16, 14 attention heads, 2 KV heads, vocabulary 151936, matching the adapter's fixed 24-layer BF16 block baseline. This is model-resource validation only and does not prove CUDA execution.
Changed files: src/integration/vllm/adapter.cpp; include/kvstore/integration/vllm/adapter.hpp; tests/integration/vllm/adapter_test.cpp; src/integration/vllm/CMakeLists.txt; tests/integration/vllm/CMakeLists.txt; CMakeLists.txt; artifacts/models/Qwen2.5-0.5B/{config.json,generation_config.json,tokenizer.json,tokenizer_config.json,model.safetensors}
Commands: `HF_ENDPOINT=https://hf-mirror.com hf download Qwen/Qwen2.5-0.5B --repo-type model --local-dir artifacts/models/Qwen2.5-0.5B --include '*.safetensors' --include '*.safetensors.index.json' --max-workers 4`; `cmake --build build --target kvstore_vllm_tests -j2`; `./build/tests/integration/vllm/kvstore_vllm_tests --gtest_color=no`; `sha256sum artifacts/models/Qwen2.5-0.5B/model.safetensors`; `git diff --check`
Test result: Qwen resource download completed; safetensors SHA-256 `88c142557820ccad55bb59756bfcfcf891de9cc6202816bd346445188a0ed342`. vLLM adapter contract suite 16/16 passed. Real vLLM 0.29.0 runtime, CUDA/pinned staging and GPU E2E remain pending; P8.2 stays [~].
Virtualenv validation: workspace `.venv` created and populated from Tsinghua PyPI with `safetensors==0.5.3`, `huggingface-hub==0.27.1`, and `transformers==4.48.3`. Local `AutoConfig`/tokenizer loading succeeded and safetensors metadata reports 290 tensors; embedding and Q/K/V shapes are `(151936,896)`, `(896,896)`, `(128,896)`, `(128,896)`. The tokenizer reports 151643 base vocabulary while the model uses padded 151936 embedding rows; this expected distinction is recorded and the adapter validates the model padded vocabulary boundary. PyTorch is not installed, so CPU forward/generation is not claimed. Commands: `.venv/bin/python ... AutoConfig/AutoTokenizer/safe_open`; `cmake --build build --target kvstore_vllm_tests -j2`; `./build/tests/integration/vllm/kvstore_vllm_tests --gtest_color=no`. Result: local model metadata validation passed; vLLM contract tests 16/16 passed. `.venv/` added to `.gitignore`.
GPU/runtime validation: `.venv` now contains `torch==2.14.0+cu130` from the Tsinghua PyPI mirror; local CUDA smoke reports `cuda_available=True`, one NVIDIA GeForce RTX 4060 Laptop GPU, and Qwen2.5-0.5B CPU/GPU model forward succeeded with logits `(1,4,151936)`, 24 KV layers and first KV shape `(1,2,4,64)`. Docker Hub was unreachable, so `docker.m.daocloud.io/nvidia/cuda:12.4.1-base-ubuntu22.04` was pulled instead (digest `sha256:0f6bfcbf267e65123bcc2287e2153dedfc0f24772fb5ce84afe16ac4b2fada95`); container `nvidia-smi` sees the RTX 4060, driver 591.86, CUDA 13.1. This validates GPU plumbing, not vLLM runtime compatibility or adapter CUDA DMA ownership.
Build remediation: completed missing `RadixMetadata::device` initialization in `tests/unit/sglang_hicache_storage_test.cpp`, removing the `-Werror=missing-field-initializers` blocker. `cmake --build build --target kvstore_unit_tests kvstore_vllm_tests -j2` succeeded; combined SGLang/vLLM acceptance filter passed 18/18. P8.2 remains [~] because actual vLLM 0.29.0 and KVConnectorBase_V1 runtime integration is not installed; P8.3 remains [~] pending independent audit.

Audit round: 1
Auditor: Agent B
Verdict: pass-with-risk
Findings: medium real vLLM 0.29.0 KVConnectorBase_V1/runtime and block-manager callback compatibility is not wired; medium CUDA allocator, page-locked host memory, H2D event lifetime and active DMA cancellation are not exercised; low whole-tree build is blocked by unrelated SGLang test missing-field-initializers warning under -Werror.
Commands: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON`; `cmake --build build --target kvstore_vllm_tests -j2`; `./build/tests/integration/vllm/kvstore_vllm_tests --gtest_color=no`; `git diff --check`
Test result: isolated vLLM target and 16/16 tests passed. Tests cover miss/partial/full hit, exact/prefix lookup, metadata and block safety rejection, deadline/cancellation, remote timeout, lost acknowledgement, persistence rehydration, publication cleanup and reconnect lookup. Whole build/test is not claimed because the unrelated SGLang test fails `-Werror=missing-field-initializers`.
Residual risks: P8.2/P8.4, Owner Agent A: validate actual vLLM 0.29.0 KVConnectorBase_V1, GPU block injection, pinned CPU staging/CUDA event cancellation and performance on Qwen2.5-0.5B. P8.3, Owner Agent A: fix SGLang test warning and obtain independent audit. P10.1 tracks the broader integration matrix. P8.2 remains [~] and acceptance boxes remain unchecked.
```

---

## P9：多节点扩展、运维与安全

### [ ] P9.1 分布式放置与路由

- [ ] 基于 P0.1 已冻结的 primary/replica 契约，评估是否为后续版本启用分片
- [ ] 若评审决定启用分片，先补充架构决策和一致性契约，再实现稳定 hash/slot、节点成员版本和请求重定向
- [ ] 定义副本读策略、staleness 暴露和故障切换边界
- [ ] KVCache 放置考虑 tenant/model、设备拓扑和数据局部性
- [ ] 增加节点加入/离开、重平衡和部分网络故障测试

### [ ] P9.2 资源隔离与安全

- [ ] 配置连接、请求、frame、batch、内存、磁盘和队列硬上限
- [ ] 实现 tenant namespace 和容量/带宽配额
- [ ] 评估认证与 TLS；未实现时限制部署边界并记录风险
- [ ] 防止目录穿越、符号链接替换和非预期 snapshot/AOF 路径
- [ ] fuzz 所有网络/磁盘 parser，并保留 regression corpus
- [ ] 依赖漏洞和许可证扫描进入发布流程

### [ ] P9.3 可观测性

- [ ] 指标：请求量、错误、延迟、连接、队列和各引擎操作
- [ ] 指标：AOF flush/sync、snapshot、恢复 offset 和错误
- [ ] 指标：复制 lag、backlog 使用、full sync、重连和回环丢弃
- [ ] 指标：内存/磁盘容量、水位、迁移、命中分类和加载合并
- [ ] tracing 覆盖 parse/dispatch/engine/match/load/replicate/persist
- [ ] 高基数 key、tenant、request ID 不得成为无界 metrics label
- [ ] 增加健康检查、诊断 INFO 和关键告警建议

验收：故障注入可通过日志、指标和 trace 定位；敏感 payload 不进入日志。

工作记录：
```text
Task ID: P8.3
Owner: Agent A
Dependencies: P8.1
Scope: Qwen2.5-0.5B SGLang 0.5.19 adapter; dynamic HiCacheStorage interface_v1/radix metadata mapping, publication lifecycle, fallback and contract tests.
Status: in progress; real GPU/container validation pending Docker image access.

Implementation round: 1 (Agent A)
Dependencies: P8.1 accepted round 3 pass-with-risk.
Scope: framework-independent SGLang 0.5.19 / Qwen2.5-0.5B dynamic HiCacheStorage interface_v1 contract; root-prefix metadata mapping, serialized Session lookup/read/publish/release/abort and bounded timeout/cancel/checksum fallback; standalone mock and contract tests. Only SGLang adapter paths, build registration, scoped tests/docs and this P8.3 record are in scope; no vLLM edits.
Acceptance: automated miss/partial/full hit, metadata isolation, upload cleanup, timeout, cancellation, checksum and restart contracts. Real Python plugin loading, GPU DMA/radix insertion and SGLang E2E remain explicitly unverified; P8.3 stays [~] pending independent Agent B audit.
Changed files: CMakeLists.txt; include/kvstore/integration/sglang/hicache_storage.hpp; src/integration/sglang/hicache_storage.cpp; tests/unit/sglang_hicache_storage_test.cpp; docs/p8-sglang-adapter.md; todolist/todolist.md
Commands: `cmake --build build -j2` (pass); `ctest --test-dir build --output-on-failure -R 'SglangHiCache|IntegrationMockClient'` (existing build: IntegrationMockClient pass); fresh configure/build blocked by pre-existing/unowned `src/integration/vllm/CMakeLists.txt:6` deferred `add_subdirectory` error. No real SGLang/GPU E2E claimed.
Test result: adapter library compiles in existing build; focused new test could not be registered because fresh configure is blocked by the unrelated vLLM CMake change. Agent B audit required; P8.3 remains [~].
```

---

## P10：系统验证与发布门禁

### [ ] P10.1 自动化测试矩阵

- [ ] 单元测试覆盖公共组件、四引擎、parser、状态机和策略
- [ ] 集成测试覆盖三协议 x 三网络后端 x 四引擎的受支持矩阵
- [ ] 端到端测试覆盖 Redis 客户端、SAVE/LOAD、重启和复制
- [ ] 模型/属性测试覆盖随机命令序列和引擎差分
- [ ] fuzz 覆盖 RESP/Text/Batch、AOF、snapshot 和 replication frame
- [ ] ASAN/UBSAN/TSAN 在 CI 分开运行
- [ ] 失败注入覆盖 ENOSPC、EIO、partial write、kill -9、网络分区和慢节点
- [ ] 长稳测试检查 fd、RSS、磁盘、队列和延迟随时间增长

### [ ] P10.2 性能与容量

- [ ] 建立单引擎、协议、网络、AOF、复制和 KVCache 分层 benchmark
- [ ] 比较 epoll/io_uring/ntyco 及复制 pthread/reactor/proactor/ntyco
- [ ] 测试 1B 到目标 KV chunk 尺寸、不同读写比和连接数
- [ ] 测试内存压力、磁盘冷读、backlog wrap 和 snapshot 干扰
- [ ] 设置可解释的性能回归阈值并保存历史结果
- [ ] 通过容量模型给出每节点 key/chunk、内存、磁盘和网络预算

### [ ] P10.3 发布准备

- [ ] 固化配置 schema、wire protocol、AOF 和 snapshot 版本
- [ ] 编写部署、升级、备份恢复、故障处理和兼容性文档
- [ ] 验证 clean build、Release build、测试和安装包
- [ ] 列出已知限制、未支持 Redis 命令和实验性后端
- [ ] Agent B 执行最终独立审计并给出 `pass` 或 `pass-with-risk`

验收：所有 M0-M8 里程碑勾选；无 critical/high 未解决问题；所有 residual risk 有负责人和跟踪项。

工作记录：待开始时填写。

---

## 审计问题模板

发现问题时按以下格式追加，修复后保留历史记录：

```text
ID: AUD-<phase>-<number>
Severity: critical | high | medium | low
Status: open | fixed | risk-accepted
Location: <file:line>
Problem: <可观察的问题>
Reproduction: <命令或最小步骤>
Required fix: <可验收的修复要求>
Owner: Agent A | name
Audit round: <N>
```

## 当前下一步

- [x] M0/M1 已依据 P0/P1 最终 `pass` 审计补正里程碑状态
- [~] 实现并审计 P7.1；通过后按依赖推进 P7.2/P7.3
- [!] P8.2-P8.4 真实 GPU 验收：Windows Docker daemon 已启动且注册 NVIDIA runtime；等待镜像仓库连通及 GPU 容器 smoke 通过，WSL 原生 socket 尚不可用（Owner: Agent A）

## 工作记录模板

每个小节在首次执行时复制以下模板；审计失败后追加新轮次，不覆盖旧记录：

```text
Task ID: Pn.m
Owner: Agent A / <name>
Dependencies: <task IDs or none>
Scope: <本轮实现范围>
Condition evidence: <按内部 checkbox 顺序列出产物或测试证据；未完成项写 pending>
Changed files: <paths>
Commands: <实际执行命令>
Test result: <通过/失败及摘要>
Audit round: <N>
Auditor: Agent B
Verdict: pending | fail | pass-with-risk | pass
Findings: <file:line；无则 none>
Residual risks: <无则 none；有则对应后续任务 ID、负责人和接受理由>
```
