# KVStore Engine Agent Guide

本文件是仓库内所有 Agent 的强制协作规范。目标是以 C++20 实现面向 Attention KVCache 的分布式、多存储引擎服务器，并让每一次实现都可追踪、可审计、可复现。

## 1. 开始工作前

每个实现请求必须依次执行：

1. 阅读本文件和 `todolist/todolist.md`。
2. 定位与请求对应的清单项；没有对应项时，先新增清单项、依赖和验收条件。
3. 检查相关代码、测试、配置和未提交改动，不得假设模块已经存在。
4. 将目标项标记为 `[~]`，填写当前执行者、实现范围和依赖。
5. 由 Agent A 实现并自测，由独立上下文的 Agent B 审计。
6. 根据审计意见修复并重复审计，直到结论为 `pass` 或 `pass-with-risk`。
7. 只有满足清单中的验收条件后，才能将条目标记为 `[x]`。

状态符号：

- `[ ]`：未开始
- `[~]`：进行中或等待审计
- `[x]`：完成且审计通过
- `[!]`：阻塞，必须注明原因和解除条件

禁止在未更新清单的情况下开始实现。纯调查任务也应在相关项的工作记录中留下结论，但无需把未实现项标记为完成。

## 2. 双 Agent 收敛流程

### Agent A：实现者

Agent A 负责：

- 明确输入、输出、并发模型、所有权和失败语义。
- 采用最小正确改动，沿用现有接口和目录边界。
- 同步实现代码、测试、配置示例和必要文档。
- 运行受影响测试；涉及共享接口、持久化格式或并发行为时运行完整测试。
- 在清单项下记录变更文件、执行命令、测试结果和已知风险。
- 不得以降低、删除或跳过测试来获得通过结果。

### Agent B：审计者

Agent B 必须以独立审计视角检查实际 diff 和测试结果，不得只复述 Agent A 的说明。重点检查：

- 正确性：协议边界、错误路径、部分读写、空值和溢出。
- 并发性：数据竞争、锁顺序、生命周期、背压、取消和停机。
- 一致性：AOF、快照、复制顺序、幂等性及崩溃恢复。
- 兼容性：RESP 客户端行为、配置校验、持久化格式版本。
- 性能：热路径分配、复制、锁竞争、系统调用数量和复杂度退化。
- 安全性：长度上限、资源配额、路径处理、输入验证和敏感日志。
- 测试：失败注入、边界条件、确定性和回归覆盖。

审计结论仅允许：

- `fail`：存在必须修复的问题；列出严重级别、文件和复现方式。
- `pass-with-risk`：功能和测试通过，但存在不阻塞当前里程碑的明确风险；必须登记后续清单项。
- `pass`：验收项全部满足且无已知未登记风险。

审计记录使用以下格式，追加到对应清单项的“工作记录”中：

```text
Audit round: <N>
Auditor: Agent B
Verdict: fail | pass-with-risk | pass
Findings: <按 critical/high/medium/low 排序，注明 file:line>
Commands: <实际执行的构建、测试、sanitizer 或 benchmark 命令>
Residual risks: <无则写 none>
```

若结论为 `fail`，Agent A 修复后必须请求新一轮审计。实现者不能自行把审计结论改为通过。`pass-with-risk` 中的每项风险必须有负责人、后续清单项或接受理由。

## 3. 架构边界

以下目录是核心边界；实现时必须保留其职责隔离。只有在清单中记录裁剪理由并经 Agent B 审计通过，才能合并目录或将多个目录合并为 target：

```text
CMakeLists.txt
cmake/                 # 编译选项和第三方依赖
config/config.json     # 唯一启动配置入口
include/kvstore/       # 对外公共头文件
src/common/            # 状态码、buffer、CRC、日志、配置
src/engine/            # Array/RBTree/Hash/SkipList 及统一 Engine API
src/command/           # 命令模型、校验和分发
src/protocol/          # Text、KV、RESP、Batch 增量解析/编码
src/net/               # epoll/io_uring/ntyco 后端和连接生命周期
src/persistence/       # 写事件、AOF、snapshot、恢复
src/replication/       # 握手、backlog、全量/增量同步、LiveSync
src/kvcache/           # Attention KV 元数据、匹配、分级和准入/淘汰
src/integration/       # vLLM/SGLang adapter 或 sidecar API
src/server/            # 启动、装配、信号和优雅停机
tests/unit/
tests/integration/
tests/e2e/
benchmarks/
tools/
docs/
```

依赖方向应为：网络/协议 -> 命令分发 -> 引擎与 KVCache 服务；持久化和复制消费统一写事件。存储引擎不得依赖网络协议，协议解析器不得直接修改存储。

## 4. 核心契约

### 命令语义

- 基础命令为 `SET/GET/DEL/MOD/EXIST/SAVE/LOAD`。
- `SET` 仅创建不存在的 key；`MOD` 仅更新已存在的 key。原生协议必须保留两者差异。
- Redis RESP 层采用 Redis 覆盖语义：收到 `SET` 时由兼容层根据统一原子 upsert 能力完成覆盖，不能用非原子的 `EXIST` 后接 `SET/MOD`。
- `INCR/DECR` 必须是原子操作，并校验整数格式和溢出。
- `MGET` 保持输入顺序，不存在的 key 返回 RESP Null。
- key 和 value 以二进制安全字节串处理，不能依赖 NUL 结尾。
- 每个变更命令必须产生带单调序列号的统一写事件；读取和管理命令不得进入 AOF/复制流。

### 存储引擎

- Array、RBTree、Hash、SkipList 实现同一个线程安全接口和一致的状态码。
- 引擎返回值不得暴露锁保护下对象的悬空引用；优先显式所有权或调用方提供 buffer。
- 引擎选择由 `config/config.json` 决定，进程启动后不可静默切换。
- 引擎差异只影响实现和性能，不得改变命令可观察语义。

### 持久化与复制

- 所有写来源必须标记为 `client`、`aof_replay`、`full_sync` 或 `incremental_sync`。
- 回放和同步写入更新存储，但不得错误地再次写回同一传播链路。
- AOF flush 同时受 batch 记录数、字节数和时间阈值控制；sync 策略独立配置。
- snapshot 包含 magic、格式版本、引擎/数据元信息、记录数和 CRC32；损坏或不兼容文件必须拒绝加载。
- 发布持久化格式后只允许显式版本升级，不得无版本地更改二进制布局。
- 每个成功提交的写事件获得唯一递增 offset；AOF 与复制保持该顺序。
- 复制 backlog 固定为 1024 个槽位时，槽位应保存序列号和完整事件边界，并能检测覆盖导致的断档。
- 同一监听端口通过握手识别普通客户端和复制节点；握手完成前不得执行普通写命令。
- LiveSync 使用来源节点 ID 和事件 ID 抑制回环，同时保留合法的多跳传播。

### KVCache 数据模型

- 普通 KV 与 Attention KV 使用统一底层字节存储，但 KVCache 元数据必须显式建模：模型 ID、模型/adapter 版本、tenant、token/prefix hash、layer 范围、dtype、shape、device、创建/访问时间和校验值。
- cache key 必须覆盖会改变 KV 张量含义的全部维度，禁止跨模型、跨租户或不兼容 dtype 命中。
- 匹配至少支持 exact match 和最长前缀 match；结果包含命中 token 数及缺失范围。
- 内存/磁盘迁移使用状态机，至少区分 resident、loading、evicting、disk-only、failed；同一对象的并发 load 应合并。
- 请求调度必须考虑内存预算、对象大小、复用频率/最近访问、加载代价、重算代价、队列深度和设备拓扑。
- 热路径不得因统计或持久化进行无界阻塞；必须有背压、超时和降级到重算的策略。

## 5. 配置规范

`config/config.json` 是唯一启动参数入口。命令行只允许指定配置文件路径以及 `--help/--version/--check-config`，不得创建覆盖 JSON 字段的第二套参数体系。

配置至少覆盖：

- server：监听地址、端口、线程数、连接/请求限制和优雅停机超时。
- protocol：启用协议、最大 key/value/frame/batch 大小和解析超时。
- engine：类型、容量和引擎专属参数。
- network：后端类型及 epoll/io_uring/ntyco 参数。
- persistence：目录、AOF 阈值、sync 策略、snapshot 和恢复选项。
- replication：角色、node ID、上游、backlog、超时及后端。
- kvcache：内存/磁盘预算、水位线、分块、匹配、准入和淘汰策略。
- observability：日志级别、metrics 和 tracing。

启动时必须一次性校验配置，未知枚举、范围错误、字段冲突或缺少必填项应输出可定位错误并以非零状态退出。测试不得依赖开发者机器上的绝对路径。

## 6. C++20 工程规范

- 使用 RAII 管理 fd、mmap、ring、线程、锁和内存；禁止裸 `new/delete` 表达所有权。
- 公共 API 使用明确的 `Status/Result` 错误模型，不用异常穿越网络事件循环或 C ABI。
- 使用 `std::span`、`std::string_view` 时必须证明底层生命周期覆盖使用期。
- 跨线程共享状态采用清晰的所有权、锁或原子协议；不得用 `volatile` 实现同步。
- 解析网络和磁盘数据时做整数溢出检查，再分配内存或执行偏移运算。
- 日志不得输出 KVCache 原始张量、凭据或完整租户 key。
- 第三方库通过 CMake `FetchContent` 或系统包引入，固定版本，并记录许可证与引入理由。
- 默认构建开启严格警告；CI 中警告视为错误。生产构建不得依赖 sanitizer。

## 7. 构建与验证

构建骨架完成后，以下命令作为统一入口；新增功能应接入这些命令，而不是创建孤立脚本：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_SANITIZERS=address,undefined
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

涉及并发的变更还需运行 TSAN；涉及协议的变更需有分片输入、粘包、多命令和畸形输入测试；涉及持久化/复制的变更需有崩溃、截断、重复事件和断线重连测试。

性能变更必须保存环境、数据集、命令、至少一次 warmup 和多轮结果，报告 p50/p95/p99、吞吐、CPU、内存及回归比例。不得用 Debug 或 sanitizer 构建宣称生产性能。

## 8. 完成定义

清单项只有同时满足以下条件才算完成：

- 行为与验收条件一致，错误路径已实现。
- 单元、集成或端到端测试与风险匹配，并实际通过。
- 配置、指标和文档随行为同步更新。
- 没有引入未界定的线程、fd、mmap 或内存生命周期。
- Agent B 最终结论为 `pass` 或 `pass-with-risk`。
- 所有残余风险已在 `todolist/todolist.md` 中登记。
- 工作记录包含变更文件、验证命令和审计轮次。

## 9. 变更边界

- 不修改与当前清单项无关的用户改动。
- 不为假设中的兼容需求添加双实现或隐式 fallback。
- 不在未提供正确性基线前优化热路径。
- eBPF/sockmap 与 RDMA 在核心复制契约稳定前只保留接口和设计文档，不提供伪实现。
- vLLM/SGLang 集成优先使用版本化 adapter/sidecar 协议，框架私有 ABI 变化不得渗透到核心引擎。
