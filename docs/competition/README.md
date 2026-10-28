# 幸福者 2026 · 赛题一：seekdb 实例部署

## 一、提交物

| 项目 | 内容 |
| --- | --- |
| 提交物 | seekdb 实例的遥测 UUID（`run/telemetry.json` 中的 `content.id`） |
| **实例 UUID** | **`c3ff6536-ead2-880b-a234-5df542cc57af`** |
| 来源文件 | `/var/lib/seekdb-docker/run/telemetry.json` → `content.id` |
| 生成时刻 | 2026-10-28 08:23:12 (+08:00)，实例首次 bootstrap 时生成 |
| reporter | `docker-seekdb`（官方镜像默认值） |
| seekdb 版本 | `5.7.25-OceanBase seekdb-v1.4.0.0` |

`run/telemetry.json` 原文：

```json
{"content": {"id": "c3ff6536-ead2-880b-a234-5df542cc57af", "host": {"os": "rocky", "cpu": "Intel(R) Core(TM) Ultra 9 275HX", "cpuCount": 24, "osVersion": "9.4", "memorySize": "15.3360825G"}, "event": "bootstraped", "version": "1.4.0.0", "instance": {"port": 2881, "hostHash": "12CA17B49AF2289436F303E0166030A21E525D266E209267433801A8FD4071A0", "createTimestamp": 1793146992399022}, "reporter": "docker-seekdb", "resource": {"cpuCount": 4, "logDiskSize": "0G", "memoryLimit": "12.268866G", "dataFileSize": "0.03125G"}, "telemetryVersion": 5}, "component": "seekdb"}
```

> 提取命令：`jq -r .content.id /var/lib/seekdb-docker/run/telemetry.json`

## 二、运行环境

| 项 | 值 |
| --- | --- |
| 宿主系统 | Windows 11 家庭中文版 26200（24 核 / 16 GB） |
| 运行环境 | WSL2 `Ubuntu-22.04`，已启用 systemd |
| Docker | 29.1.3（overlayfs，cgroup=systemd） |
| 镜像 | `docker.m.daocloud.io/oceanbase/seekdb:latest`（Docker Hub `oceanbase/seekdb:latest` 的加速副本） |
| 镜像 digest | `sha256:4db4961869d554313520d6081fca29265ba6da6db7313311bd8acbed771a956b` |
| 容器名 | `seekdb-docker` |
| 端口映射 | `0.0.0.0:2882 -> 2881/tcp`（容器内 2881） |
| 数据卷 | `/var/lib/seekdb-docker:/var/lib/seekdb`（宿主机 WSL 路径） |
| 重启策略 | `unless-stopped` |

## 三、部署步骤

```bash
# 1. 启动 docker（WSL 内 systemd 管理）
systemctl start docker && systemctl enable docker

# 2. 拉取镜像（Docker Hub 直连被墙，走 daocloud 镜像加速）
docker pull docker.m.daocloud.io/oceanbase/seekdb:latest

# 3. 准备数据目录并启动实例
mkdir -p /var/lib/seekdb-docker
docker run -d --name seekdb-docker \
  -p 2882:2881 \
  -v /var/lib/seekdb-docker:/var/lib/seekdb \
  --restart unless-stopped \
  docker.m.daocloud.io/oceanbase/seekdb:latest

# 4. 等待首次 bootstrap 完成（约 20~30s），读取遥测 UUID
cat /var/lib/seekdb-docker/run/telemetry.json
```

启动后目录结构：`/var/lib/seekdb-docker/{run,log,etc,store}`，
其中 `run/telemetry.json` 即提交物来源。

## 四、实例可连接性 / SQL 验证

```bash
$ mysql -h127.0.0.1 -P2882 -uroot -e "select version(), @@port, now();"
version()	@@port	now()
5.7.25-OceanBase seekdb-v1.4.0.0	2881	2026-10-28 08:25:03

$ mysql -h127.0.0.1 -P2882 -uroot -e "show databases;"
comp_verify
information_schema
mysql
oceanbase
test
```

写入/查询验证（`comp_verify.t_hello`）：

```sql
create table t_hello(
  id int primary key auto_increment,
  msg varchar(64),
  ts timestamp default current_timestamp
);
insert into t_hello(msg) values ('hello seekdb from xfz');
select * from t_hello;
-- id=1  msg=hello seekdb from xfz  ts=2026-10-28 08:23:52
```

## 五、证据文件

均存放于 `deploy/evidence/`：

| 文件 | 说明 |
| --- | --- |
| `01_docker_and_telemetry.txt` | `docker ps` / `docker images` / 镜像 digest / `telemetry.json` 原文 |
| `02_sql_verification.txt` | mysql 客户端连接 2882 端口执行 SQL 的实际输出 |
| `03_telemetry.json` | `run/telemetry.json` 原件副本 |
| `04_container_logs.txt` | 容器首次 bootstrap 的启动日志 |
| `05_restart_persistence.txt` | `docker restart` 后 UUID 不变、数据仍在的验证 |

网络侧佐证：另起一个**全新数据目录**的临时实例 bootstrap 时，用 `tcpdump` 抓到进程向
`openwebapi.oceanbase.com`（遥测上报端点 `https://openwebapi.oceanbase.com/api/web/oceanbase/report`）
发起的 TLS 连接（SNI 明文命中 3 次），随后删除该临时实例；说明官方镜像在 bootstrap 时
会完成遥测上报，本实例的 `createTimestamp=1793146992399022` 与之同批次。

## 六、踩坑记录（重要）

1. **只有带 `REPORTER=docker-seekdb` 的官方镜像会写 `run/telemetry.json`**。
   用 deb 包裸装并以 systemd 启动的 `seekdb --nodaemon` 实例，`/var/lib/seekdb/run/`
   下只有 `seekdb.pid`、`sql.sock`、`seekdb` 软链，**不会生成 `telemetry.json`**，
   无法取得比赛要求的 UUID。
2. 实例 UUID 在**首次 bootstrap** 时生成并写入文件；之后用同一数据目录重启，
   UUID 保持不变；换新数据目录才会产生新 UUID。
3. Docker Hub 直连不可用，`docker.m.daocloud.io` 镜像加速可用。
4. `docker restart seekdb-docker`（数据卷不变）后 UUID 保持 `c3ff6536-ead2-880b-a234-5df542cc57af`
   不变，表数据也完好；容器重启策略为 `unless-stopped`，WSL 内 `docker` 服务已 `enable`。

## 七、其它实例（仅作部署验证，非提交值）

| 形态 | 位置 | 端口 | 遥测文件 | 备注 |
| --- | --- | --- | --- | --- |
| deb + systemd（server 模式） | `/var/lib/seekdb` | 2881 | 无 | SQL 连通已验证 |
| pylibseekdb 嵌入式 | `/var/lib/seekdb-inst` | - | 有，ID `ceeb0da4-dd73-866e-805c-db806c5b3025`（`sent:true`） | 嵌入式 C 客户端模式 |

## 八、复现脚本

`deploy/` 目录下保留了本次部署的脚本（编号即执行顺序），关键几个：

- `18_pull.sh`：镜像拉取
- `21_docker_run.sh`：启动实例
- `22_verify_sql.sh`：SQL 验证
- `24_evidence.sh`：证据采集
- `28_tcpdump_test.sh`：遥测上报网络化验证

---

部署与验证完成时间：2026-10-28 08:25 (+08:00)
