# OpenNet.Traversal

OpenNet 的 Linux NAT 行为与入站端口探测节点。

## 协议

- UDP `3478`/`3479`：RFC 5389 STUN Binding，并实现 RFC 5780
  `CHANGE-REQUEST`、`RESPONSE-ORIGIN` 和 `OTHER-ADDRESS`。
- HTTP `48100`：
  - `GET /health`
  - `POST /v1/probes/tcp`，正文 `{"port": 6881}`
  - `POST /v1/probes/udp`，正文 `{"port": 6881}`

端口探测只允许探测 HTTP 请求来源地址，不能用作任意目标扫描器。TCP 探测
在连接后发送标准 68 字节 BitTorrent 握手；UDP 探测发送 BEP 29 uTP SYN，
以便直接检测已经由 libtorrent 占用的 TCP/UDP 监听端口，无需再次绑定客户端
端口。

TCP 的 `reachable=true` 表示服务端已完成到客户端的 TCP 三次握手，并成功
写入完整 BitTorrent 握手。服务端不能在不知道客户端当前 torrent info-hash
的情况下要求 libtorrent 返回应用层握手，因此不会把“收到 BitTorrent
响应握手”作为开放条件；否则没有活动 torrent 时会产生假阴性。UDP 则必须
收到合法的 uTP v1 响应才判定开放。

回连默认超时由 `--probe-timeout-ms` 控制，默认每次 TCP/UDP 探测 2500 ms。
超时不是 HTTP 错误：API 返回 HTTP 200、`reachable=false` 和
`evidence="timeout"`。OpenNet 客户端对单次探测请求设置 5 秒上限；未取得
HTTP 响应显示为 `Not tested`，取得明确的超时/拒绝结果才显示
`Blocked / unreachable`。

NAT 映射与过滤行为使用一个临时诊断 UDP socket 测量，不与 libtorrent
监听 socket 竞争。完整 RFC 5780 过滤测试要求节点主机拥有两个公网 IPv4；
用 `--alternate-bind` 和 `--alternate-advertise` 配置第二地址。

## 动态 DNS（DDNS）

公网地址会变化的节点可直接配置一个同时由 A/AAAA 记录维护的 DNS-only
主机名：

```sh
OpenNet.Traversal \
  --bind 0.0.0.0 \
  --advertise-hostname traversal.example.com \
  --directory-url http://127.0.0.1:5090/api/v1/traversal/servers
```

程序启动时必须从该主机名解析到一个 IPv4 地址；AAAA 记录是可选的。之后会
按照 `--heartbeat-seconds` 的周期重新解析：

- A/AAAA 未变化时不更新运行时状态；
- 地址变化时，Directory 注册和新的 STUN `RESPONSE-ORIGIN` /
  `OTHER-ADDRESS` 响应立即使用新地址；
- 首次注册后会复用 Server 返回的节点 ID，因此地址变化会更新同一条数据库
  记录，而不是在每次变化时创建新节点；
- DNS 查询整体失败或没有返回 A 记录时保留上一次有效地址；
- A 记录存在但 AAAA 被删除时，会停止向 Directory 发布 IPv6 地址。

`--advertise-hostname` 不能与 `--advertise` 或 `--advertise-ipv6` 同时使用。
静态地址部署继续使用原有参数。DDNS 节点应使用 `--bind 0.0.0.0`，这样公网
IPv4 变化不需要重新绑定 STUN socket；若绑定某个会消失的具体本地地址，地址
变化后仍然需要重启进程。完整 RFC 5780 所需的第二个 IPv4 仍必须通过
`--alternate-bind` / `--alternate-advertise` 单独配置，不会从 DNS 返回的
第二个 A 记录自动推断。

域名必须直接解析到节点公网地址。Cloudflare 等 DNS 服务应使用 DNS only，
普通 HTTP 代理不能代理 UDP 3478/3479 或此探测协议。

## 构建

```sh
cmake -S src/OpenNet.Traversal -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

查看全部参数：

```sh
OpenNet.Traversal --help
```

`deploy/opennet-traversal.service` 是 systemd 示例。部署时必须替换文档用途的
`203.0.113.10`，开放节点配置的两个 UDP 端口和 HTTP 端口，并将节点注册到
OpenNet.Server。推荐在 `/etc/opennet/traversal.env` 设置
`OPENNET_DIRECTORY_API_KEY=...`；节点会按心跳周期用 `PUT` 刷新自身配置与
健康时间。也可使用 `OPENNET_DIRECTORY_URL`，避免将协调服务 URL 写入进程
参数。
