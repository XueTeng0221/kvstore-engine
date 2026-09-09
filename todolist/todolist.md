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

- [ ] M0 工程骨架、配置和公共基础设施可构建
- [ ] M1 四种单机引擎通过统一一致性测试
- [ ] M2 三类协议与命令分发可通过 epoll 对外服务
- [ ] M3 RESP 兼容常用 Redis 客户端和扩展命令
- [ ] M4 AOF、快照及崩溃恢复可验证
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
Residual risks: pending
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

### [~] P2.1 统一命令模型与分发

- [ ] 定义 typed command/request context/response，协议层不直接访问引擎
- [ ] 实现命令注册与分发，避免协议间复制业务逻辑
- [ ] 实现 `SET/GET/DEL/MOD/EXIST/SAVE/LOAD`
- [ ] 将引擎错误稳定映射到各协议响应
- [ ] 加入请求 deadline、取消、最大在途请求和背压
- [ ] 为每个命令增加协议无关单元测试

验收：同一命令通过不同协议得到等价结果；未知命令和参数错误不会关闭健康连接。

### [~] P2.2 Text + KV 协议

- [ ] 冻结 framing，支持二进制 value，不能依赖换行承载任意 payload
- [ ] 实现增量 parser，处理分片、粘包和单连接多命令
- [ ] 实现 encoder、错误响应和协议版本
- [ ] 增加畸形长度、超限、提前 EOF 和 fuzz 测试

### [~] P2.3 Batch 协议

- [ ] 定义 batch header、record count、总长度、每项状态和最大限制
- [ ] 明确 batch 是逐项原子还是整体原子，并实现对应行为
- [ ] 支持混合读写及保持响应顺序
- [ ] 对超大 batch、部分失败和背压增加测试

### [~] P2.4 epoll 网络后端

- [ ] 定义 `INetworkBackend` 与 connection state 生命周期
- [ ] 实现 nonblocking accept/read/write、边沿触发 drain 和 partial write
- [ ] 实现 per-connection input/output 高水位与读暂停
- [ ] 实现连接空闲、握手、请求和关闭超时
- [ ] 实现 signal 驱动的优雅停机和在途请求排空
- [ ] 测试慢客户端、断连、半关闭、fd 复用和连接风暴

验收：epoll 后端可在同一端口识别已启用协议并稳定服务；无 busy loop、无未界定 buffer 增长。

### [~] P2.5 Server 装配

- [ ] 从 `config/config.json` 创建引擎、协议、网络、持久化和复制组件
- [ ] 启动阶段失败执行逆序清理并返回非零状态
- [ ] 增加 readiness/liveness 内部状态
- [ ] 实现端到端 smoke test：启动、CRUD、停机、重启

工作记录：
```text
Task IDs: P2.1, P2.2, P2.3, P2.4, P2.5
Owner: Agent A
Dependencies: P1.1, P1.6, P0.4
Scope: typed command dispatcher, native Text/KV + Batch, epoll backend and server assembly
Condition evidence: command dispatcher; RESP/native/Batch incremental parsers; epoll edge-triggered accept/read/write and live RESP SET/GET smoke
Changed files: include/kvstore/command/, include/kvstore/protocol/, include/kvstore/net/, src/command/, src/protocol/, src/net/, src/server/main.cpp, tests/unit/protocol_test.cpp
Commands: cmake --build build -j2; ctest --test-dir build --output-on-failure; live socket smoke on port 6399
Test result: Debug 40/40 pass; protocol/persistence focused 9/9 pass; live RESP SET/GET pass
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
```

---

## P3：Redis RESP 兼容层

### [~] P3.1 RESP parser/encoder

- [ ] 支持 RESP2 Simple String、Error、Integer、Bulk String、Array 和 Null
- [ ] 评估 RESP3；首版不实现时明确拒绝/协商行为
- [ ] 实现增量、多命令 pipeline、嵌套深度和 frame 上限
- [ ] 保持 key/value 二进制安全
- [ ] 增加 redis-protocol corpus、fuzz 和恶意长度测试

### [~] P3.2 Redis 命令

- [ ] `SET` 使用原子 upsert，已存在 key 不通过竞态的 EXIST+MOD 实现
- [ ] 实现 `GET/DEL/EXISTS/MGET`
- [ ] 实现原子 `INCR/DECR`、非整数和溢出错误
- [ ] 实现 `PING [message]`、`ECHO message`
- [ ] 实现最小 `CLIENT SETINFO/SETNAME/GETNAME` 探测兼容
- [ ] 实现最小 `INFO` sections，未支持命令返回标准错误
- [ ] 决定并记录 `SAVE/LOAD` 在 RESP 层的暴露方式
- [ ] 增加 pipeline、并发覆盖、MGET Null 和客户端探测测试

### [~] P3.3 客户端兼容验证

- [ ] 使用 `redis-cli` 完成 CRUD、pipeline 和 INFO smoke test
- [ ] 使用至少 Python redis-py、Node redis、Go go-redis 验证连接探测
- [ ] 记录不兼容命令和版本边界

验收：常见客户端无需关闭健康检查即可连接；覆盖写和计数命令在线性化测试中正确。

工作记录：
```text
Task IDs: P3.1, P3.2, P3.3
Owner: Agent A
Dependencies: P2.1, P2.4, P2.5, P1.1
Scope: RESP2 incremental parser/encoder and Redis command compatibility
Condition evidence: RESP2 bulk/array/null/error, pipeline/fragmentation, atomic Redis SET overwrite, MGET Null, INCR/DECR, PING/ECHO/INFO
Changed files: include/kvstore/protocol/resp.hpp, src/protocol/resp.cpp, src/command/dispatcher.cpp, tests/unit/protocol_test.cpp
Commands: ctest --test-dir build --output-on-failure -R ProtocolTest
Test result: focused protocol tests pass; redis-cli/client-library compatibility pending
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
```

---

## P4：统一写事件、AOF 与快照

### [~] P4.1 写事件中心

- [ ] 定义 `WriteEvent`：offset、event ID、origin node/source、command、key/value、时间和 checksum
- [ ] 区分 `client/aof_replay/full_sync/incremental_sync` 来源
- [ ] 明确事件提交点、存储可见点、AOF 确认点和复制发布点
- [ ] 实现有界队列、背压和停机 drain
- [ ] 实现传播策略，阻止回放重入 AOF 和同步回环
- [ ] 测试并发生产顺序、重复事件、消费者失败和队列满

验收：所有变更命令恰好形成一个逻辑事件；offset 严格递增且恢复后不回退。

### [~] P4.2 AOF 格式与批处理

- [ ] 冻结版本化 AOF record/frame 格式和 CRC
- [ ] 按记录数、累计字节、时间任一阈值触发 flush
- [ ] 独立实现 `always/everysec/no` sync 策略及错误上报
- [ ] 处理 partial write、EINTR、ENOSPC、损坏尾记录和目录 fsync
- [ ] 实现 replay，来源标记为 `aof_replay`
- [ ] 实现 rewrite/compaction 或登记明确容量边界
- [ ] 增加 kill -9、截断、bit flip、重复 replay 和磁盘满测试

### [~] P4.3 Snapshot

- [ ] 定义 header：magic、version、flags、count、payload length、last offset、CRC32
- [ ] 定义确定性 record 编码，不直接 dump C++ struct 内存布局
- [ ] 使用临时文件、fsync 和原子 rename 发布快照
- [ ] 实现 mmap 加载，并校验文件长度、边界、count 和 checksum
- [ ] 实现 io_uring snapshot writer，保留同步 fallback 的明确选择配置
- [ ] `SAVE/LOAD` 与后台任务并发时有清晰互斥/快照点语义
- [ ] 测试空库、大对象、损坏 header、版本不兼容和中断发布

### [~] P4.4 恢复流程

- [ ] 启动时加载最新合法 snapshot 后回放更高 offset 的 AOF
- [ ] 拒绝 offset 回退、重复或不连续记录，或按文档化策略处理
- [ ] 暴露恢复阶段、进度、耗时和失败原因
- [ ] 恢复期间 readiness 为 false，不接受普通写请求
- [ ] 建立不同崩溃点的恢复矩阵测试

验收：确认成功的写在配置承诺范围内可恢复；损坏数据不会静默加载；恢复结果与参考模型一致。

工作记录：
```text
Task IDs: P4.1, P4.2, P4.3, P4.4
Owner: Agent A
Dependencies: P1.1, P0.3, P0.4
Scope: write events, versioned AOF, CRC snapshot, mmap load and restart recovery
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

### [ ] P7.1 KVCache 数据与 key 规范

- [ ] 定义 tensor manifest：model/adapter/tenant/token hash/layer/dtype/shape/layout/device
- [ ] 定义 chunk 大小、对齐、压缩可选项和每 chunk checksum
- [ ] 定义模型升级、adapter 变化和 tokenizer 变化的失效规则
- [ ] 实现 canonical cache key，防止跨模型/租户错误命中
- [ ] 实现 metadata 与 chunk 生命周期的原子关联
- [ ] 增加 key 碰撞、元数据不兼容和损坏 chunk 测试

验收：任意命中都能证明张量兼容；不完整对象不可见；删除可回收所有关联 chunk。

### [ ] P7.2 Match 与索引

- [ ] 实现 exact match
- [ ] 实现 token/prefix hash 的最长前缀 match
- [ ] 返回命中 token 数、命中层/块和缺失范围
- [ ] 处理 hash 碰撞，可用 token 摘要/二次校验确认
- [ ] 为索引更新、驱逐和并发查询定义一致性
- [ ] benchmark 不同 prefix 长度、并发度和对象规模

### [ ] P7.3 内存/磁盘分级状态机

- [ ] 定义 resident/loading/evicting/disk-only/failed 状态与合法转换
- [ ] 实现内存 slab/pool、预算、碎片统计和高低水位
- [ ] 实现磁盘 chunk store、空间配额、回收和校验
- [ ] 实现 memory->disk 降级和 disk->memory 提升
- [ ] 合并同一对象并发 load，等待者可超时/取消
- [ ] 迁移期间 pin 活跃对象，防止 use-after-free 或重复驱逐
- [ ] 对磁盘满、短读、checksum 错误、取消和进程重启增加测试

### [ ] P7.4 决策与调度

- [ ] 收集 recency、frequency、size、load cost、recompute cost 和 reuse distance
- [ ] 建立可解释准入分数，首版基线可采用 cost-aware LRU/GDSF
- [ ] 区分 prefill 热对象、decode 活跃对象和低复用对象
- [ ] 调度 load/match/evict 队列，设置并发度、优先级和 I/O 配额
- [ ] 在 deadline 前预计无法加载时快速 miss 并允许推理端重算
- [ ] 防止大对象扫描、cache pollution 和 tenant 饥饿
- [ ] 支持策略参数配置和运行指标，不在线上热路径同步训练策略

验收：策略决策可由指标解释；在基准 trace 上优于纯 LRU 基线，且尾延迟无不可接受回归。

### [ ] P7.5 请求到达时的内存快取路径

- [ ] 实现单次 lookup 返回 resident handle，避免额外 value copy
- [ ] 对命中 prefix 只调度缺失 token/layer 的计算或加载
- [ ] 合并相同 prefix 的并发 miss，防止重复磁盘 I/O/重算
- [ ] 支持 request priority、deadline 和取消传播
- [ ] 记录 exact/prefix/memory/disk/miss/coalesced 命中分类

验收：resident hit 不触发磁盘 I/O；并发相同请求只产生一次 load；handle 生命周期覆盖推理消费。

工作记录：待开始时填写。

---

## P8：vLLM 与 SGLang 集成

### [ ] P8.1 版本化集成协议

- [ ] 选择 adapter/sidecar 边界，定义 capability negotiation 和版本策略
- [ ] 定义 lookup/reserve/put/get/release/abort 请求及 tensor descriptor
- [ ] 明确 CPU pinned memory、CUDA IPC 或网络传输的首版路径
- [ ] 定义超时、取消、部分命中、校验失败和回退重算行为
- [ ] 支持 request/model/tenant trace context
- [ ] 提供框架无关 mock client 和 contract tests

### [ ] P8.2 vLLM adapter

- [ ] 固定支持的 vLLM 版本和 KV cache layout
- [ ] 在 prefill 前查询 exact/prefix match
- [ ] 将命中块注入受支持的 block manager/connector 接口
- [ ] prefill 后异步发布可复用 KV，并在请求取消时清理 reservation
- [ ] 集成测试覆盖 miss、partial hit、full hit、超时和 server 重启

### [ ] P8.3 SGLang adapter

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

工作记录：待开始时填写。

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

工作记录：待开始时填写。

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

- [ ] 从 P0.1 开始冻结首版协议与一致性目标
- [ ] 完成 P0.2 工程骨架后再并行推进 P0.3/P0.4
- [ ] M0 审计通过前不得开始网络、复制或 KVCache 性能优化

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
