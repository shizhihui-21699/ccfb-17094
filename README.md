# 安全唤醒拉取参考实现（C11）

车辆远程终端被伪造格式短信注入指令的根因是：**短信既承担唤醒，又被当作指令通道，而且发送号码可伪造**。

本项目给出一个可移植的固件改造参考：

1. 短信只允许表达“醒来拉取”，不包含、不解析、更不执行任何车辆指令。
2. 终端醒来后在**硬截止时间和电量预算**内建立双向认证加密通道。
3. 终端主动拉取平台待执行指令，校验时间、签名、类型白名单和车辆安全联锁后执行。
4. 用单调序号、持久化游标和 ACK 处理弱网、丢响应、重复投递和断电重启。
5. 用预连接抖动、Full Jitter 退避、平台 `Retry-After`、短信分批和网关限流避免“群醒”压垮平台。

代码是平台无关的状态机和策略库；socket/TLS、modem AT、Flash/KV、CAN/车身控制通过 HAL 接入。

## 目录

```text
include/wake_sms.h       短信边界：只分类唤醒 token，没有指令字段
include/wake_policy.h    截止时间、jitter、指数退避、Retry-After
include/wake_command.h   类型化指令、ACK、结构和有效期校验
include/wake_hal.h       时钟、睡眠、随机数、KV、执行器、电源锁等移植接口
include/wake_channel.h   mTLS/加密拉取通道接口和协议结构
include/wake_agent.h     唤醒主状态机
src/                     C11 实现
tests/                   无外部依赖的主机单元测试
examples/host_demo.c     带模拟平台的完整流程演示
examples/herd_sim.c      2 万设备群醒/错峰/过载退避模拟
```

## 构建和测试

```sh
make test     # ASan/UBSan 可通过下面的 CFLAGS 覆盖
make demo     # 模拟一次 503 后重试、拉取并执行开空调
make herd     # 群醒峰流和错峰效果模拟
make all
```

已在 macOS 上用 clang/cc 编译，并通过 `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`。

消毒器构建示例：

```sh
make clean test \
  CFLAGS='-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror -Iinclude' \
  LDFLAGS='-fsanitize=address,undefined'
```

## 新唤醒流程

```text
modem 收到短信
      │
      ▼
wake_sms_classify(sender, body)
      │  只接受 WAKE1 <opaque-token>
      │  sender 完全不作为信任依据；额外 CMD/参数直接拒绝
      ▼
唤醒任务获取 power wakelock
      │
      ├─ 读取持久化 cursor / 去重 token
      ├─ 低电量检查
      ├─ 0..pre_connect_jitter 错峰
      ├─ 附着蜂窝数据
      └─ POST /v1/devices/{id}/pull  (mTLS + 证书/密钥固定)
                 │
                 ├─ 503/429: Retry-After + jitter，截止时间内退避
                 ├─ 网络超时: Full Jitter 指数退避
                 └─ 200: 校验指令签名/有效期/白名单/安全联锁
                            │
                            ├─ 执行幂等车身操作
                            ├─ 先持久化 cursor/结果
                            └─ 再发 ACK；ACK 丢失由下次 pull 的 cursor 兜底
      │
      ▼
安排 RTC 下次唤醒并释放 wakelock/radio
```

## 短信格式

新短信只允许：

```text
WAKE1 <1..39 位 [A-Za-z0-9_-]>
```

例如：

```text
WAKE1 8f1c2a7d9b4e4f01
```

以下内容都会被 `wake_sms_classify()` 拒绝：

```text
WAKE1 8f1c;CMD=DOOR_UNLOCK
WAKE1 8f1c CMD=CLIMATE_ON
CMD=DOOR_UNLOCK SIGN=forged
```

注意：**格式正确的伪造短信仍可能触发一次唤醒**。这是“短信只作为唤醒信号”的固有残余 DoS：攻击者只能让终端进行一次受预算约束的拉取，不能让它执行短信中的动作。应额外采取：

- 唤醒 token 去重和短窗口合并；
- 持久化唤醒频率预算（默认每 10 分钟 6 次非重复唤醒，可按车型调整），防止伪造短信洪水持续拉起 modem；
- 短信频率限制、运营商侧垃圾短信过滤；
- 异常唤醒计数和平台风控；
- 可选的签名唤醒 nonce/计数器，密钥放安全单元，用于提高伪造唤醒成本；
- 硬唤醒窗口和退避，避免连续短信耗尽电瓶。

不能再依赖主叫号码白名单。

## 平台拉取协议建议

请求：

```http
POST /v1/devices/VIN123/pull HTTP/1.1
Host: pull.example.com
Authorization: Bearer <device-token-or-mTLS-identity>
Content-Type: application/json

{
  "cursor": 1842,
  "wake_token": "8f1c2a7d9b4e4f01",
  "battery_pct": 87,
  "rssi_dbm": -81
}
```

成功响应：

```json
{
  "server_time_ms": 1764158400000,
  "next_pull_after_ms": 3600000,
  "commands": [
    {
      "seq": 1843,
      "id": "cmd-uuid",
      "type": "CLIMATE_ON",
      "value": 22,
      "issued_at_ms": 1764158390000,
      "expires_at_ms": 1764158450000,
      "signature": "<Ed25519/HMAC via secure element>"
    }
  ]
}
```

过载响应：

```http
HTTP/1.1 503 Service Unavailable
Retry-After: 2
Content-Type: application/json

{ "error": "busy", "retry_after_ms": 2000 }
```

终端会把 `Retry-After` 作为下限，再增加最多 50% 随机抖动，避免所有设备在同一秒重试。

## 弱网、电量和截止时间

默认配置是 45 秒硬窗口，可按车型电源策略调整：

- `hard_window_ms`：本次唤醒绝对上限；
- `pre_connect_jitter_ms`：群醒后连接前随机错峰；
- `radio_attach_timeout_ms` / `pull_timeout_ms` / `ack_timeout_ms`：单次操作上限；
- `min_operation_ms`：剩余时间不足时不再启动新尝试；
- `initial_backoff_ms` / `max_backoff_ms`：Full Jitter 指数退避；
- `max_attempts`：可选次数上限，`0` 表示只受截止时间约束；
- `routine_checkin_ms`：无任务时的低频自检。

关键原则：

- **退避睡眠如果不足以保留一次最小网络操作，就不在当前窗口里睡**，而是安排 RTC 稍后重试并立即释放电源锁。
- 网络和平台错误必须区分为可重试、鉴权失败、协议错误和持久化错误，不能对永久性错误忙等。
- 完成后尽快关 radio、释放 wakelock。
- TLS 会话恢复、压缩 CBOR/protobuf、短请求可减少弱网传输和耗电。

## 可靠指令执行

平台和终端之间采用 at-least-once 投递：

- 平台指令带单调递增 `seq`、唯一 `id` 和过期时间；
- 终端 Flash/KV 保存 `cursor`，表示该序号之前的结果已持久化；
- 执行车控后先保存 `cursor/结果/pending_ack`，再发 ACK；
- ACK 成功后清除 `pending_ack`；如果 ACK 失败或随后断电，下次唤醒/拉取前会先重放这条详细结果；
- 即使详细结果 ACK 尚未送达，下一次 pull 请求中的 `cursor` 本身也是累积确认；
- 平台重复投递旧序号时，终端不得再次执行；
- 如果执行后、持久化前断电，指令可能再次执行，因此空调开启、落锁/解锁等处理必须设计为幂等状态设置，而不是“翻转”动作。

安全上还应：

- 强制 TLS 1.2+，优先 TLS 1.3；
- 使用设备证书/mTLS 或安全单元中的设备密钥；
- 校验证书链、域名和平台 CA/SPKI pin；
- 增加应用层指令签名，即使 TLS 端点被误配或内部网关被滥用，也不能伪造车控指令；
- 只允许 `wake_command_type_t` 中的强类型指令，禁止 shell、脚本、动态表达式和短信透传；
- 解锁、后备箱等动作执行前检查车速、档位、点火、碰撞状态等安全联锁；
- 密钥和单调计数器放在安全单元/TEE/eFuse 或带重放保护的安全存储中。

## 平台如何扛住大量车辆同时唤醒

`make herd` 的模拟结果示例：

```text
Simulated fleet: 20000 devices, gateway capacity 1000 pulls/s, wake window 45s, network failure 8%

synchronized SMS blast       peak_due=20000/s overload=67889 expired=23
paced SMS + jitter           peak_due=  410/s overload=    0 expired=0
```

平台侧需要配套：

1. **不要瞬时群发全部唤醒短信**：按地区、基站、机型、VIN 分片，根据网关容量控制发送速率。运营商短信投递本身会错峰，但平台仍应主动 pacing。
2. **终端预连接抖动**：收到短信后随机延迟 `0..pre_connect_jitter_ms`，把短信到达抖动进一步摊平。
3. **Full Jitter 指数退避**：防止弱网恢复后大量终端同步重连。
4. **网关自适应过载保护**：超过容量时快速返回 `503 + Retry-After`，而不是让 TLS 握手和请求队列堆积到超时。
5. **区域接入和水平扩展**：多地域 pull/MQTT 网关、DNS/Anycast/LB 分片、按 VIN/设备 ID 一致性哈希到后端分片。
6. **TLS 握手成本治理**：网关弹性扩容、会话票据/会话恢复、合适的证书链长度；私钥操作可卸载到 HSM/加速卡。
7. **指令队列和反压**：接入层与指令下发服务之间使用 Kafka/Pulsar 等队列，按 VIN 分区，消费速率受控；旧指令 TTL 到期自动失效。
8. **优先级和隔离**：用户正在等待的解锁/空调可优先于日志、升级、批量诊断；避免 OTA 占满连接和电量窗口。
9. **容量隔离**：按车型/地区/租户配置限流配额，防止一个批量任务制造 noisy neighbor。
10. **可观测性**：监控短信下发速率、短信到 pull 的延迟、TLS 握手成功率、503/429、`Retry-After`、cursor 延迟、唤醒窗口超时、电瓶电压和地区/运营商维度失败率。

对于高延迟/高丢包地区，平台还可以根据终端上报的 `rssi/network_type` 返回更长的 `Retry-After` 或下次检查时间，避免终端在无效网络中反复耗电。

## 移植到真实固件

需要实现两个接口集：

- `wake_hal_t`：`now_ms`、`sleep_ms`、真随机数、电量、modem attach、原子 KV、签名验证、安全联锁、CAN/车控执行、电源锁、RTC 调度。
- `wake_secure_channel_t`：基于 mbedTLS/OpenSSL/厂商 TLS 栈的 mTLS pull 和 ACK。

短信回调中只做分类和任务通知：

```c
void on_sms_received(const char *sender, const char *body)
{
    wake_sms_signal_t signal = wake_sms_classify(sender, body);

    if (signal.classification == WAKE_SMS_WAKE_SIGNAL) {
        wake_event_t event = {0};
        strncpy(event.token, signal.token, sizeof(event.token) - 1);
        os_queue_send(wake_queue, &event, 0);  /* ISR 中只 notify */
    }
    /* 其他短信忽略；绝不能把 body 传给 command executor。 */
}

void wake_task_main(void)
{
    wake_event_t event;

    while (os_queue_receive(wake_queue, &event) == OS_OK) {
        wake_report_t report = wake_run(&config, &hal, &channel, event.token);
        metrics_report_wake(&report);
    }
}
```

Flash/KV 写入必须原子且带 CRC/版本号；生产环境不要直接使用示例中的内存 store。

## 边界说明

这里提供的是可测试的 C11 参考状态机和主机模拟，不包含特定车机/Modem SDK、真实 mTLS 证书集成或整车 CAN 协议。接入真实固件时，应替换所有 HAL/channel 实现，并结合实际电瓶容量、驻车间隔、当地网络 RTT/丢包率重新标定 `hard_window_ms`、超时和退避参数。
