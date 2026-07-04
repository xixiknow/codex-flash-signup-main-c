# Mongoose Svelte 内嵌界面

这是一个基于 Mongoose 的小型 HTTP/WebSocket 示例项目，前端使用 Vite + Svelte + TypeScript 构建。
前端会先编译成静态文件，再通过 `mg_fs_packed` 打包进最终可执行文件中，运行时不需要单独携带 `web/dist` 目录。

## 构建

```sh
make
```

Linux 下也可以使用封装脚本：

```sh
chmod +x ./build.sh
./build.sh
```

常用目标：

```sh
./build.sh rebuild
./build.sh clean
./build.sh run
```

Windows PowerShell 可以直接运行：

```powershell
.\build.cmd
```

或者：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

构建脚本需要 `npm`、Python 3、`gcc`，以及可被编译器找到的 libcurl 和 SQLite3 开发库。必要时可通过 `CC`、`CFLAGS`、`LDFLAGS`、`CURL_CFLAGS`、`CURL_LIBS`、`SQLITE_CFLAGS`、`SQLITE_LIBS` 环境变量覆盖编译参数。

## 运行

```sh
./build/mongoose-svelte
```

Windows PowerShell 下运行：

```powershell
.\build\mongoose-svelte.exe
```

打开 `http://127.0.0.1:8000` 会跳转到控制台。

开发时也可以直接使用：

```sh
make run
```

`make run` 每次都会生成并打印一个随机管理员密码，同时重置本地数据库中的 `admin` 登录密码，方便本地调试。

也可以指定监听地址：

```sh
./build/mongoose-svelte http://0.0.0.0:9000
```

默认启用 Web/API 鉴权。首次启动时需要通过环境变量创建管理员账号：

```sh
APP_ADMIN_USER=admin APP_ADMIN_PASSWORD='请换成强密码' ./build/mongoose-svelte
```

创建后账号会写入 `data/app.db`，后续启动不需要继续携带密码环境变量。公网部署时建议让程序只监听 `127.0.0.1:8000`，前面使用 Caddy 或 Nginx 负责 HTTPS 反向代理。HTTPS 反代场景可设置 `APP_COOKIE_SECURE=1`，让登录 Cookie 带上 `Secure` 标记；如果需要登录限速按真实访客 IP 计算，再设置 `APP_TRUST_PROXY=1` 读取反代传来的 `X-Forwarded-For`。

仅本地临时调试时可以关闭鉴权：

```sh
APP_AUTH_DISABLED=1 ./build/mongoose-svelte
```

## 项目结构

- `src/` - C 应用代码
- `src/http_client/` - libcurl 外发请求与浏览器画像生成框架
- `src/identity/` - 注册身份生成器，生成姓名、生日、邮箱和密码
- `src/flow/` - 通用流程状态机和 `curl-impersonate` 请求执行器
- `web/` - Vite + Svelte + TypeScript 前端
- `third_party/mongoose/` - 项目内置的 `mongoose.c` 和 `mongoose.h`
- `tools/pack_assets.py` - 将静态资源打包为 `mg_fs_packed` 可读取的 C 文件

## Web 栏目

- `/login` - 管理后台登录页
- `/console` - 控制台，查看服务状态、本机 CPU、内存、存储、网络和 SQLite 数据库状态
- `/proxy-pool` - 代理池，管理 HTTP、SOCKS5 和 SOCKS5H 代理
- `/mail` - 邮件配置，管理 Rapid-Inbox 只读 API Key 和可用邮箱域名规则
- `/redeem` - 兑换码，配置 paymesh 兑换接口地址，导入/管理兑换码，一键兑换获取邮箱，或兑换并注册 GPT 账号
- `/accounts` - 账号管理，查看账号状态、上传状态，并对选中账号发起 OAuth、刷新 Token、重新上传、加入/退出工作区、导出凭证或删除
- `/registration` - 注册工作台，查看注册/OAuth 任务状态，启动单次、批量或无限任务，并通过 WebSocket 查看流程日志

## 调试接口

除 `/api/auth/*` 外，所有 `/api/*` 和 `WebSocket /ws` 都需要有效登录会话；未登录访问 API 会返回 `401`，未登录访问页面会跳转到 `/login`。

- `GET /api/auth/me` - 查看当前登录状态
- `POST /api/auth/login` - 管理员登录，成功后写入 HttpOnly Cookie
- `POST /api/auth/logout` - 退出登录并清除 Cookie
- `GET /api/status` - 查看服务状态、本机资源和 SQLite 数据库状态
- `WebSocket /ws` - 发送 `{"type":"system_subscribe","interval_ms":3000}` 后按周期接收 `system_status`
- `GET /api/browser-profile` - 随机生成一个浏览器画像
- `GET /api/browser-profile?region=JP&device=mobile` - 按地区和设备类型生成画像
- `GET /api/proxies` - 查看代理池
- `POST /api/proxies/import` - 批量导入代理
- `POST /api/proxies/test` - 批量测试代理，默认测试全部；传 `ids` 时只测试指定代理
- `POST /api/proxies/delete` - 删除指定代理
- `GET /api/mail/config` - 查看 Rapid-Inbox 配置和域名规则
- `POST /api/mail/config` - 保存 Rapid-Inbox 接口地址和只读 API Key
- `POST /api/mail/domains` - 添加可用邮箱域名规则
- `POST /api/mail/domains/delete` - 删除域名规则
- `POST /api/mail/fetch` - 读取公开邮箱验证码、邮件列表或邮件详情
- `GET /api/accounts/summary` - 快速读取账号总量、活跃、过期、临时、失败、已上传和未上传统计
- `GET /api/accounts?limit=50&status=active&upload_state=uploaded` - 使用游标分页读取账号列表
- `GET /api/accounts/detail?id=1` - 点击账号后读取密码、Access Token、Refresh Token、Account ID 和 Workspace ID
- `POST /api/accounts/action` - 对账号执行 `oauth`、`refresh-token`、`validate-token`、`reupload`、`workspace-join`、`workspace-leave`、`export-credentials`（`format=codex|cpa|sub2api`）或 `delete`
- `GET /api/redeem/config` - 查看 paymesh 兑换接口地址（无需鉴权）
- `POST /api/redeem/config` - 保存 paymesh 兑换接口地址
- `GET /api/redeem?status=new&limit=50` - 使用游标分页读取兑换码列表
- `POST /api/redeem/import` - 批量导入兑换码文本，一行一个并自动去重
- `POST /api/redeem/delete` - 删除指定兑换码
- `POST /api/redeem/redeem` - 对选中兑换码调用 `POST /api/v1/redeem` 兑换并回写邮箱与状态
- `POST /api/redeem/register` - 对选中兑换码创建兑换码注册任务，支持 `workflow=register_only|register_then_oauth`，通过验证码轮询 `GET /api/v1/order/lookup?poll=true` 拿码后注册 GPT 账号
- `GET /api/registration/status` - 查看注册工作台状态、可用域名、活跃代理和临时账号数量
- `POST /api/registration/start` - 创建注册/OAuth 任务，支持 `register_only`、`register_then_oauth`、`oauth_only`，并可通过 `register_provider=platform|temporary` 选择过期账号注册或临时账号注册
- `POST /api/registration/stop` - 请求停止指定任务
- `GET /api/registration/tasks` - 查看注册/OAuth 任务列表
- `GET /api/registration/task?id=rt-...` - 查看指定任务详情和日志
- `WebSocket /ws` - 发送 `{"type":"registration_subscribe","task_id":"rt-..."}` 后接收任务状态和日志推流

浏览器画像包含 `User-Agent`、`Accept-Language`、`Sec-CH-UA`、系统、设备、地区和时区等字段。
后续外发请求可以将该画像传给 `http_client_perform()`，由 libcurl 统一设置请求头、代理和超时。

代理池支持以下格式，每行一个：

```text
http://127.0.0.1:8080
http://user:pass@127.0.0.1:8080
socks5://127.0.0.1:1080
socks5h://user:pass@example.com:1080
```

批量测试使用 `https://cloudflare.com/cdn-cgi/trace` 探测代理是否可用，并保存出口 IP、地区、Cloudflare 机房、HTTP 协议和 TLS 版本。
其中 `socks5://` 使用本地 DNS 解析，测试时会通过 Cloudflare DoH 获取真实公网解析并强制 IPv4，避免本机 fake-ip 或 IPv6 优先导致部分代理连接失败；`socks5h://` 保持由代理端远程解析域名。

邮件域名规则支持精确域名和前缀通配符：

```text
x.com
*.x.com
*.*.x.com
```

`x.com` 表示可以创建 `local@x.com`；`*.x.com` 表示可以创建 `local@r1.x.com`；`*.*.x.com` 表示可以创建 `local@r1.r2.x.com`。Rapid-Inbox 请求只使用 `public.read` API Key，并直接访问配置的接口地址。

姓名生成能力已收回为内部工具函数，不再提供公开页面或 HTTP 接口。

## 注册/OAuth 流框架

注册流程按任务编排执行，当前注册工作台支持两种注册方式：`platform` 过期账号注册和 `temporary` 临时账号注册，并可在注册成功后继续执行 OAuth。

- `src/identity/identity_generator.c` 根据内部姓名库、邮件域名规则和年龄规则生成注册身份材料。
- 年龄控制在 19 到 25 岁，并生成对应生日。
- 邮箱本地部分由姓名归一化后追加 0 到 8 位随机数字或字母组成。
- 邮件域名复用 `/mail` 页面配置的规则：`a.com` 生成一级域名邮箱，`*.a.com` 会随机生成 1 到 5 位子域标签，例如 `x9.a.com`。
- `src/flow/flow_engine.c` 保留通用状态机；`src/flow/flow_impersonate.c` 使用 `curl-impersonate` 执行具体请求，每个 flow 使用独立 cookie jar，并绑定同一代理和同一浏览器画像。
- `curl-impersonate` 单次请求遇到代理断开、连接失败等进程级错误时会最多重试 3 次；多次失败后才将当前 flow 标记失败。
- 注册工作台使用 `curl-impersonate` 执行注册 HAR 路径；`platform` 对应过期账号注册，`temporary` 对应临时账号注册。
- provider 通过 `flow_provider` 接口声明下一步请求和响应处理逻辑，后续不同注册方式可以作为不同 provider 接入。
- `register_only` 只执行注册，成功账号按所选注册方式入库为过期账号或临时账号。
- `register_then_oauth` 注册成功后继续执行 OAuth；目标数量可以按注册任务数或 OAuth 成功数统计。注册任务数按已启动的注册流程计数，成功和失败会分别统计，避免高并发下因在途流程继续成功而超过目标。任务也可以开启无限模式，直到手动停止。任务可配置注册成功后的 OAuth 延迟秒数，用于给邮箱投递和账号状态留出缓冲窗口。
- 注册工作台可开启「OAuth 成功后自动上传」；OAuth 成功写入账号库后，会复用上传配置中已启用的默认 Aether 服务上传该账号，上传失败只记录上传统计和任务日志，不影响 OAuth 成功计数。
- 调度模式支持「常规」和「高速模式」。高速模式会将注册流程拆成「前置阶段」「等待邮箱」「后置阶段」三段：只限制验证码前置阶段的并发，进入等待邮箱验证码后释放启动槽，同时用最大存活数控制后台仍在推进的流程总量。
- `oauth_only` 由账号管理页触发，对选中账号执行 OAuth；OAuth 失败时账号状态保持不变。
- 兑换码注册由兑换码页触发，使用 `register_provider=redeem`：邮箱来自 paymesh 兑换接口返回的 `emailAddress`，验证码来自 `GET /api/v1/order/lookup?poll=true` 轮询，注册成功后回写兑换码状态为 `registered` 并绑定账号 ID。姓名与生日仍由身份生成器提供。
- 工作区功能在账号管理页对已入库账号执行：使用账号 `access_token` 加入（`invites/request`）或退出（从 `access_token` 的 JWT 解析用户 ID 后 `DELETE .../users/{user_id}`）工作区，完成后可选择自动导入 Aether 或导出 `codex` / `cpa` / `sub2api` 凭证 JSON。
- 失败 flow 只释放内存资源，不写入新账号；成功注册通过 `account_insert_success()` 写入账号和密钥表。
- OAuth 架构放在 `src/oauth/oauth_provider.c`，默认域名先与注册路径保持一致，使用 `https://auth.openai.com`；后续切换私有域时优先从该 provider 的集中配置点调整。
- OAuth provider 使用 Codex OAuth 参数生成授权链接，校验 callback 中的 `state`，再用授权码和 PKCE verifier 换取 Access Token / Refresh Token。命中手机号验证标记时直接记为 OAuth 失败，不再重试。
- OAuth 阶段会同时启动两条独立 OAuth 链路抢验证码；任意一条验证码校验通过后立刻取消另一条，并由胜出的链路继续后续授权和 Token 兑换。验证码等待超时时间为 60 秒，超时后不再因为短时间未接码而重建 OAuth 链路。

## 账号存储

账号状态目前固定为 `active`、`expired`、`temp`、`failed`，页面展示为活跃账号、过期账号、临时账号和失败账号。
上传状态固定为 `uploaded` 和 `not_uploaded`，页面展示为已上传和未上传。

账号列表读取 `accounts` 热表，详情页才读取 `account_secrets` 中的密码、Token、Account ID 和 Workspace ID。
统计卡片读取 `account_stats` 单行计数表，由 SQLite 触发器在账号增删和状态变化时维护，避免千万级账号下每次打开页面都对主表做聚合统计。

兑换码存储在 `redeem_codes` 表，状态为 `new`、`redeemed`、`registered`、`failed`，分别表示未兑换、已兑换出邮箱、已注册成账号、兑换或注册失败。兑换成功后写入 `email`、`product_name`、`card_id`、`session_id`、`end_time`；注册成功后写入 `account_id`。paymesh 接口地址存在 `mail_settings` 表的 `redeem_base_url` 键，默认 `https://sms.paymesh.cn`。
