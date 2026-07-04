# Codex Flash Signup 架构分析与优化方案

## 1. 核心注册认证接口

系统有两条注册路径：**Platform 注册**（创建过期账号）和 **OAuth 认证**（激活为活跃账号）。

### 1.1 Platform 注册流程

> 源码：`src/registration/platform_register_provider.c`

| 步骤 | 方法 | 端点 | 请求体/参数 | 作用 |
|------|------|------|-------------|------|
| 1 PLATFORM_LOGIN | GET | `https://platform.openai.com/login` | — | 建立初始 cookie |
| 2 AUTHORIZE | GET | `https://auth.openai.com/api/accounts/authorize` | query: client_id, audience, redirect_uri, device_id, login_hint(email), scope, state, nonce, code_challenge, code_challenge_method=S256 | OAuth 授权入口 |
| 3 PASSWORD_PAGE | GET | `https://auth.openai.com/create-account/password` | — | 密码注册页（从 authorize 的 Location 跳转） |
| 4 USER_REGISTER | POST | `https://auth.openai.com/api/accounts/user/register` | `{"password":"...","username":"email"}` | 提交邮箱密码完成注册 |
| 5 EMAIL_OTP_SEND | GET | `https://auth.openai.com/api/accounts/email-otp/send` | — | 触发验证码邮件发送 |
| 6 EMAIL_VERIFICATION | GET | `https://auth.openai.com/email-verification` | — | 打开邮箱验证页 |
| 7 OTP_VALIDATE | POST | `https://auth.openai.com/api/accounts/email-otp/validate` | `{"code":"123456"}` | 提交 OTP 验证 |
| 8 SESSION_DUMP | GET | `https://auth.openai.com/api/accounts/client_auth_session_dump` | — | 获取 user_id |
| 9 CREATE_ACCOUNT | POST | `https://auth.openai.com/api/accounts/create_account` | `{"name":"Full Name","birthdate":"1990-01-01"}` | 完成账号资料 |

**关键常量：**
- `client_id`: `app_2SKx67EdpoN0G6j64rFvigXD`
- `audience`: `https://api.openai.com/v1`
- `redirect_uri`: `https://platform.openai.com/auth/callback`
- `scope`: `openid profile email offline_access`
- PKCE: S256 code_challenge, 43 字符 code_verifier

**结果：** 注册成功后以 `status=expired` 入库，不写入 access/refresh token。

---

### 1.2 OAuth 激活流程

> 源码：`src/oauth/oauth_provider.c`

| 步骤 | 方法 | 端点 | 请求体/参数 | 作用 |
|------|------|------|-------------|------|
| 1 OAUTH_AUTHORIZE | GET | `https://auth.openai.com/oauth/authorize` | query: response_type=code, client_id, redirect_uri, state, scope, code_challenge, code_challenge_method=S256, prompt=login, originator=codex_vscode | Codex OAuth 入口 |
| 2 OAUTH_AUTH | GET | `https://auth.openai.com/api/oauth/oauth2/auth` | 同上参数 | 内部 OAuth 路由 |
| 3 ACCOUNT_LOGIN | GET | Location 跳转 | — | 登录挑战 |
| 4 LOGIN_PAGE | GET | `https://auth.openai.com/log-in` | — | 登录页，获取 oai-did cookie |
| 5 SENTINEL_AUTHORIZE | POST | `https://sentinel.openai.com/backend-api/sentinel/req` | `{"p":"","id":"device_id","flow":"authorize_continue"}` | 获取 Sentinel Token（反机器人） |
| 6 AUTHORIZE_CONTINUE | POST | `https://auth.openai.com/api/accounts/authorize/continue` | `{"username":{"kind":"email","value":"..."}}` + Header: OpenAI-Sentinel-Token | 提交邮箱 |
| 7 SENTINEL_PASSWORD | POST | `https://sentinel.openai.com/backend-api/sentinel/req` | `{"p":"","id":"device_id","flow":"password_verify"}` | 刷新 Sentinel |
| 8 SEND_OTP | POST | `https://auth.openai.com/api/accounts/passwordless/send-otp` | 空 body | 触发 OTP |
| 9 OTP_VALIDATE | POST | `https://auth.openai.com/api/accounts/email-otp/validate` | `{"code":"123456"}` | 验证 OTP |
| 10 SESSION_DUMP | GET | `https://auth.openai.com/api/accounts/client_auth_session_dump` | — | 获取 workspace_id |
| 11 WORKSPACE_SELECT | POST | `https://auth.openai.com/api/accounts/workspace/select` | `{"workspace_id":"..."}` | 选择 workspace |
| 12-14 REDIRECT | GET | 重定向链（最多 8 跳） | — | 跟随 consent → callback |
| 15 TOKEN_EXCHANGE | POST | `https://auth.openai.com/oauth/token` | form: grant_type=authorization_code, client_id, code, redirect_uri, code_verifier | 换取 access_token + refresh_token |

**关键常量：**
- `client_id`: `app_EMoamEEZ73f0CkXaXp7hrann`（Codex 专用）
- `redirect_uri`: `http://localhost:1455/auth/callback`
- `scope`: `openid profile email offline_access api.connectors.read api.connectors.invoke`
- `originator`: `codex_vscode`
- PKCE: S256, 64 字符 code_verifier

**结果：** 成功后以 `status=active` 入库，写入 access_token 和 refresh_token。

---

### 1.3 两条路径的编排关系

```
registration_tasks 调度器
├── REG_WORKFLOW_REGISTER_ONLY     → Platform 注册 → 入库(expired)
├── REG_WORKFLOW_REGISTER_THEN_OAUTH → Platform 注册 → 等待 → OAuth 激活 → 入库(active)
├── REG_WORKFLOW_OAUTH_ONLY        → 从库中取 expired 账号 → OAuth 激活 → 更新为 active
└── REG_SCHEDULER_FASTLANE         → 快速通道模式
```

---

## 2. 邮箱取件交互

### 2.1 外部服务：Rapid-Inbox

> 源码：`src/mail/rapid_inbox.c`

对接一个自建的邮件收件 API 服务。

**API 端点：**

| 用途 | 方法 | URL 模板 | 认证 |
|------|------|----------|------|
| 获取验证码列表 | GET | `{base_url}/api/v1/public/mailboxes/{email}/verification-codes?limit=20` | `X-API-Key` header |
| 获取邮件列表 | GET | `{base_url}/api/v1/public/mailboxes/{email}/messages?limit=20` | 同上 |
| 获取单封邮件 | GET | `{base_url}/api/v1/public/mailboxes/{email}/messages/{delivery_id}` | 同上 |
| 获取单封验证码 | GET | `{base_url}/api/v1/public/mailboxes/{email}/messages/{delivery_id}/verification-code` | 同上 |

**配置存储：** SQLite `mail_settings` 表
- `rapid_inbox_base_url` — 默认 `http://127.0.0.1:8000`
- `rapid_inbox_api_key` — API 密钥

### 2.2 验证码提取逻辑

```
响应 JSON 结构:
{
  "items": [
    {
      "verification_code": "123456",    // 优先取这个
      "code": "123456",                 // 备选字段
      "subject": "Your code is 123456", // 最后从 subject 正则提取连续6位数字
      "received_at": "2026-01-01T00:00:00Z"
    }
  ]
}
```

提取优先级：
1. `$.items[i].verification_code`
2. `$.items[i].code`
3. 从 `$.items[i].subject` 中匹配第一个连续 6 位数字

### 2.3 轮询与超时策略

**Platform 注册路径：**
```
OTP 发送 → 每 2.5s 轮询
         → 10s 未收到 → 触发 resend
         → resend 后每 2.5s 轮询
         → resend 后 30s 仍未收到 → 放弃
```

**OAuth 路径：**
```
OTP 发送 → 每 2.5s 轮询
         → 60s 总超时 → 放弃（无 resend 机制）
         → 验证失败 → 重新触发 send-otp 重试一次
```

**时间过滤：** 只接受 `received_at >= otp_sent_after_epoch - 10s` 的验证码，避免取到旧邮件。

### 2.4 域名规则管理

支持通配符域名规则，用于邮箱地址生成：
- `example.com` — 精确匹配
- `*.example.com` — 一级通配
- `*.*.example.com` — 多级通配

存储在 `mail_domain_rules` 表，`is_active=1` 的规则参与邮箱生成。

---

## 3. 代理织入逻辑

### 3.1 代理池管理

> 源码：`src/proxy/proxy_pool.c`

**支持协议：** `http`、`socks5`、`socks5h`

**导入格式：** 每行一个
```
http://user:pass@host:port
socks5://host:port
socks5h://user:pass@host:port
host:port              → 自动补 http://
```

**存储：** SQLite `proxy_nodes` 表
```sql
CREATE TABLE proxy_nodes (
  id INTEGER PRIMARY KEY,
  scheme TEXT,
  host TEXT,
  port INTEGER,
  username TEXT,
  password TEXT,
  proxy_url TEXT UNIQUE,
  status TEXT,          -- 'new' | 'active' | 'failed'
  last_test_ok INTEGER,
  last_http_status INTEGER,
  exit_ip TEXT,
  exit_loc TEXT,        -- 国家代码
  exit_colo TEXT,       -- Cloudflare 数据中心
  trace_http TEXT,
  trace_tls TEXT,
  last_error TEXT,
  last_tested_at INTEGER,
  created_at INTEGER,
  updated_at INTEGER
);
```

**测试方法：** 通过代理请求 `https://cloudflare.com/cdn-cgi/trace`，解析：
- `ip=` → 出口 IP
- `loc=` → 国家
- `colo=` → 数据中心
- `http=` → HTTP 版本
- `tls=` → TLS 版本

### 3.2 代理选取策略

```c
// proxy_pool_pick_active_url()
SELECT proxy_url FROM proxy_nodes WHERE status='active'
ORDER BY CASE scheme
  WHEN 'socks5h' THEN 0   -- 最优先：远程 DNS 解析
  WHEN 'http' THEN 1
  WHEN 'socks5' THEN 2    -- 最低：本地 DNS 解析
  ELSE 3
END, random()
LIMIT 1
```

优先级：`socks5h > http > socks5`，同优先级随机。

### 3.3 代理注入点

**curl_multi 路径（flow_engine.c）：**
```c
curl_easy_setopt(easy, CURLOPT_PROXY, slot->flow.proxy_url);
if (proxy_url_has_scheme(slot->flow.proxy_url, "socks5")) {
    curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(easy, CURLOPT_DOH_URL, "https://1.1.1.1/dns-query");
}
```

**curl-impersonate 路径（flow_impersonate.c）：**
```c
add_arg(argv, &argc, "--proxy");
add_arg(argv, &argc, flow->proxy_url);
```

### 3.4 代理生命周期

```
导入(new) → 测试通过(active) → 分配给 flow → 流程结束
                                              ├── 成功 → 代理保持 active
                                              └── 风控拦截 → 标记 environment_retryable
                                                           → 丢弃当前环境
                                                           → 重新分配代理+指纹重试
         → 测试失败(failed)
```

**锚定策略：** 每个 flow 创建时绑定一个代理，整个流程生命周期内不变。

### 3.5 socks5 特殊处理

当使用 socks5 代理时：
- 强制 IPv4 解析（`CURL_IPRESOLVE_V4`）
- 启用 DoH（`https://1.1.1.1/dns-query`）避免 DNS 泄露

---

## 4. 浏览器指纹模拟

> 源码：`src/http_client/browser_profile.c`

### 4.1 指纹维度

| 维度 | 生成方式 |
|------|----------|
| User-Agent | 根据浏览器+OS+版本组合生成 |
| Sec-CH-UA | Chrome 系生成，Firefox/Safari 留空 |
| Sec-CH-UA-Platform | 对应 OS |
| Sec-CH-UA-Mobile | desktop=?0, mobile=?1 |
| Accept-Language | 根据地区生成（en-US, ja-JP, zh-CN 等） |
| Accept-Encoding | `gzip, deflate, br, zstd` |
| Timezone | 对应地区 |

### 4.2 浏览器分布

| 类型 | 权重（desktop hint） | 权重（无 hint） |
|------|---------------------|-----------------|
| Chrome Desktop | 72% | 56% |
| Firefox Desktop | 28% | 20% |
| Chrome Android | — | 15% |
| Safari iOS | — | 9% |

### 4.3 版本池

- Chrome: 148.0.7778.167, 147.0.7730.214, 146.0.7698.120
- Firefox: 150.0.3, 149.0.2, 140.10.2esr
- iOS Safari: 17.6, 18.4

### 4.4 curl-impersonate 集成

> 源码：`src/flow/flow_impersonate.c`

查找顺序：
1. 环境变量 `CURL_IMPERSONATE_BIN`
2. `/usr/local/bin/curl_chrome145`
3. `/usr/bin/curl_chrome145`
4. PATH 中的 `curl_chrome145` / `curl_chrome146` / `curl_chrome142` / `curl-impersonate`

如果是 `curl-impersonate` 通用二进制，额外传入 `--impersonate chrome145 --compressed`。

---

## 5. 优化方案

### 5.1 指纹一致性修复

**问题：** browser_profile 可能生成 Firefox/Safari UA，但 curl-impersonate 底层始终模拟 Chrome 的 TLS/HTTP2 指纹（JA3/JA4）。这是一个明显的矛盾信号，容易被 TLS 指纹检测识别。

**方案：**
- 方案 A：将 browser_profile 限制为只生成 Chrome 系 profile，与 impersonate 二进制一致
- 方案 B：根据 profile 的浏览器类型选择对应的 impersonate 目标（chrome → curl_chrome145, firefox → curl_ff）
- 推荐方案 A，实现简单且 Chrome 市场份额最大

### 5.2 请求时序人性化

**问题：** 步骤之间几乎无延迟，机器特征明显。

**方案：**
```
延迟模型: delay = LogNormal(μ=ln(base_ms), σ=0.5)

步骤类型          base_ms    含义
页面加载后        1500-3000  模拟阅读时间
表单提交前        2000-5000  模拟输入时间
验证码提交        800-1500   模拟复制粘贴
重定向跟随        200-500    浏览器自动跳转
```

并发任务启动间隔用 Poisson 过程：`interval = Exponential(λ = 1/avg_gap_ms)`

### 5.3 代理智能调度

**问题：** 随机选取，无信誉评估，无地理一致性。

**方案：**

```
┌─────────────────────────────────────────────┐
│           代理调度器 (Proxy Scheduler)         │
├─────────────────────────────────────────────┤
│ 1. 地理匹配过滤                               │
│    proxy.exit_loc == profile.region           │
│                                              │
│ 2. Thompson Sampling 选择                     │
│    score = Beta(successes+1, failures+1).sample() │
│    选 score 最高的代理                         │
│                                              │
│ 3. 冷却检查                                   │
│    if proxy.last_blocked_at + cooldown > now: │
│        skip                                  │
│    cooldown = base * 2^consecutive_blocks     │
│                                              │
│ 4. IP 去重                                    │
│    同一 /24 段 5 分钟内不复用                   │
└─────────────────────────────────────────────┘
```

### 5.4 邮件系统优化

**问题：** 轮询模式延迟高、资源消耗大。

**方案对比：**

| 方案 | 延迟 | 资源消耗 | 实现复杂度 | 推荐场景 |
|------|------|----------|-----------|----------|
| 当前轮询 | 0-2.5s | 高（N*并发*0.4 QPS） | 已实现 | 低并发 |
| WebSocket 推送 | <100ms | 低 | 中 | 中等并发 |
| 本地 SMTP 收件 | <50ms | 极低 | 高 | 高并发/自有域名 |
| Redis Pub/Sub | <100ms | 低 | 中 | 分布式部署 |

**推荐路径：**
1. 短期：Rapid-Inbox 增加 WebSocket 推送接口，本地维护长连接
2. 中期：自建轻量 SMTP（如 maddy），MX 指向自己，验证码到达即入库
3. 长期：SMTP + Redis Pub/Sub，支持多节点分布式收件

### 5.5 并发架构统一

**问题：** `flow_engine`（curl_multi 异步）和 `flow_impersonate`（fork+exec 同步）两套执行路径，后者阻塞线程。

**方案：**

```
方案 A: 线程池 + 事件通知（推荐）
┌──────────────┐     ┌──────────────┐
│  主事件循环    │     │  线程池(N)    │
│  curl_multi   │◄────│  impersonate │
│  + pipe 通知  │     │  子进程执行    │
└──────────────┘     └──────────────┘

方案 B: 纯 libcurl + TLS 指纹补丁
- 用 BoringSSL 补丁实现 JA3 模拟
- 所有请求走 curl_multi
- 无 fork 开销，最高并发
- 实现复杂度高
```

### 5.6 风控对抗分层策略

| 检测层 | 信号 | 当前对抗 | 改进方向 |
|--------|------|----------|----------|
| 网络层 | IP 信誉、ASN 类型、/24 段密度 | 代理池 | 住宅代理优先；IP 信誉评分；/24 去重 |
| TLS 层 | JA3/JA4 指纹 | curl-impersonate | 修复 UA 与 TLS 不一致 |
| HTTP 层 | Header 顺序、缺失字段、Sec-Fetch 一致性 | 完整模拟 | Header 顺序与真实浏览器对齐 |
| 行为层 | 请求间隔、无鼠标事件、无 JS 执行 | 无 | 随机延迟；Sentinel Token 正确获取 |
| 账号层 | 注册速率、邮箱模式、同 IP 多账号 | 域名规则 | 时间分散；前缀多样化；IP 绑定限制 |
| Cookie 层 | oai-did 一致性、session 连续性 | cookie jar | 确保全流程 cookie 不丢失 |

### 5.7 资源消耗优化

**当前瓶颈分析：**

| 资源 | 消耗点 | 优化手段 |
|------|--------|----------|
| CPU | fork+exec curl-impersonate | 线程池复用 / 纯 libcurl |
| 内存 | 每 flow 独立 CURL easy handle | 连接池复用 |
| 网络 | 轮询邮件 API | WebSocket 推送 |
| 磁盘 I/O | impersonate workspace 文件读写 | tmpfs / 内存管道 |
| 代理带宽 | 完整 HTML 页面下载 | 只需 header（大部分步骤不需要 body） |

**快速优化建议：**
1. impersonate workspace 放到 `/dev/shm`（tmpfs）
2. 不需要 body 的步骤（如纯跳转）设置 `CURLOPT_NOBODY`
3. 邮件轮询改为指数退避（1s → 2s → 4s）而非固定 2.5s

### 5.8 降低拦截率的综合策略

**注册节奏控制：**
```
全局限制:
- 单 IP 每小时最多 2 次注册
- 单域名每小时最多 10 次注册
- 全局每分钟最多 N 次（根据代理池大小动态调整）

时间分布:
- 避开 UTC 0:00-6:00（低活跃时段，异常流量更显眼）
- 模拟工作时间分布：高斯分布，峰值在当地时间 10:00-14:00
```

**失败后策略：**
```
风控拦截 → 丢弃当前环境（代理+指纹+cookie）
         → 代理进入冷却（指数退避：5min → 10min → 20min → ...）
         → 下次使用全新环境重试
         → 连续 3 次风控 → 该代理标记 failed

非风控失败 → 保持环境，直接重试
           → 连续 3 次失败 → 换环境重试
```

---

## 6. 实施优先级

| 优先级 | 改进项 | 预期效果 | 工作量 |
|--------|--------|----------|--------|
| P0 | 修复 UA/TLS 指纹不一致 | 降低 TLS 层检测率 | 小 |
| P0 | 添加请求间随机延迟 | 降低行为层检测率 | 小 |
| P1 | 代理地理一致性匹配 | 降低网络层检测率 | 中 |
| P1 | 代理信誉评分（Thompson Sampling） | 提高成功率 | 中 |
| P1 | 邮件 WebSocket 推送 | 降低延迟和资源消耗 | 中 |
| P2 | impersonate 异步化（线程池） | 提高并发能力 | 大 |
| P2 | 注册节奏控制 | 降低账号层检测率 | 中 |
| P3 | 本地 SMTP 收件 | 极低延迟 | 大 |
| P3 | 纯 libcurl TLS 指纹 | 最高性能 | 极大 |
