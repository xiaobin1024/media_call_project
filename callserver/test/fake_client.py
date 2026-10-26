"""
假客户端：用最少的代码模拟 MediaCall 客户端的信令与 UDP 行为，用于路径状态机测试。

协议要点（与服务端 src/session.cpp / src/signaling_server.cpp 对应）：
  - TCP 信令：换行分隔的 JSON，每条以 \n 结尾。
  - 所有字段值都是字符串（服务端 findField 只认 "key":"value" 形式）。
  - UDP：TCP 端口 + 1。发 1 字节探针 → 服务端回 port_discovered；
          发 8 字节中继头 + 负载 → 服务端按 relayId 转发给对端。
"""

import json
import socket
import struct
import threading
import time

RELAY_MAGIC = 0x5A4C
RELAY_HEADER_SIZE = 8


def _relay_header(relay_id: int) -> bytes:
    """magic(2) | version(1) | reserved(1) | relayId(4, big-endian)"""
    return struct.pack("!HBBI", RELAY_MAGIC, 1, 0, relay_id)


class FakeClient:
    """一个假客户端：一条 TCP 信令连接 + 一个 UDP socket。"""

    def __init__(self, host: str, tcp_port: int, username: str):
        self.host = host
        self.tcp_port = tcp_port
        self.udp_port = tcp_port + 1
        self.username = username

        self.sock = socket.create_connection((host, tcp_port), timeout=10)
        self.sock.settimeout(None)

        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.settimeout(2.0)

        self._msgs = []
        self._lock = threading.Lock()
        self._buf = b""
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # ---------- 底层 ----------

    def _read_loop(self):
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return
                self._buf += chunk
                while b"\n" in self._buf:
                    line, self._buf = self._buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line.decode("utf-8", "replace"))
                    except ValueError:
                        continue
                    with self._lock:
                        self._msgs.append((time.time(), msg))
        except OSError:
            return

    def send(self, obj: dict):
        self.sock.sendall((json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8"))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        try:
            self.udp.close()
        except OSError:
            pass

    def is_closed(self, timeout: float = 2.0) -> bool:
        """服务端是否已关闭这条信令连接（被踢下线时读到的就是 EOF）。"""
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(1) == b""
        except socket.timeout:
            return False
        except OSError:
            return True

    # ---------- 消息查询 ----------

    def messages(self, msg_type=None):
        with self._lock:
            if msg_type is None:
                return list(self._msgs)
            return [(t, m) for t, m in self._msgs if m.get("type") == msg_type]

    def wait_for(self, msg_type: str, timeout: float, predicate=None, since: float = None):
        """等到第一条满足条件的消息，返回 (timestamp, msg)；超时返回 None。

        since：只看该时刻之后到达的消息。必须显式传入，否则会把本场景早先
        收到的同类消息（例如第一次的 p2p_probation）再次匹配上，造成假阳性。
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ts, msg in self.messages(msg_type):
                if since is not None and ts < since:
                    continue
                if predicate is None or predicate(msg):
                    return ts, msg
            time.sleep(0.05)
        return None

    def latest(self, msg_type: str):
        msgs = self.messages(msg_type)
        return msgs[-1][1] if msgs else None

    # ---------- 信令动作 ----------

    def login(self):
        self.send({"type": "login", "username": self.username})
        return self.wait_for("login_resp", timeout=5.0) is not None

    def offer(self, target: str, sdp: str = "0:61000", lan_ip: str = "127.0.0.1"):
        self.send({"type": "call_offer", "target": target, "sdp": sdp, "lanIp": lan_ip})

    def early(self, target: str, sdp: str = "0:61001", lan_ip: str = "127.0.0.1"):
        self.send({"type": "call_early", "target": target, "sdp": sdp, "lanIp": lan_ip})

    def answer(self, target: str, sdp: str = "0:61002", lan_ip: str = "127.0.0.1"):
        self.send({"type": "call_answer", "target": target, "sdp": sdp, "lanIp": lan_ip})

    def hang_up(self, target: str):
        self.send({"type": "hang_up", "target": target})

    def report(self, target: str, direct_rx: bool, reason: str = "test"):
        """上报是否收到直连包。direct_rx=False 会触发服务端回退中继。"""
        self.send({
            "type": "path_report",
            "target": target,
            "directRx": "1" if direct_rx else "0",
            "rxProbes": "1" if direct_rx else "0",
            "videoLoss": "0.00",
            "reason": reason,
        })

    def keep_reporting(self, target: str, direct_rx: bool, interval: float = 0.5,
                       duration: float = 30.0, reason: str = "test"):
        """后台线程持续上报，模拟客户端每秒一次的直连探测上报。"""
        def loop():
            deadline = time.time() + duration
            while time.time() < deadline:
                try:
                    self.report(target, direct_rx, reason)
                except OSError:
                    return
                time.sleep(interval)

        t = threading.Thread(target=loop, daemon=True)
        t.start()
        return t

    # ---------- UDP ----------

    def udp_probe(self) -> bool:
        """发 1 字节探针，服务端应回传 port_discovered（TCP 上的消息）。"""
        self.udp.sendto(b"\x00", (self.host, self.udp_port))
        return self.wait_for("port_discovered", timeout=5.0) is not None

    def udp_probe_tagged(self, name: str) -> bool:
        """带用户名的探测包（新格式 0x01 + 用户名），服务端按用户名精确匹配。"""
        self.udp.sendto(b"\x01" + name.encode("utf-8"), (self.host, self.udp_port))
        return self.wait_for("port_discovered", timeout=5.0) is not None

    def send_relay(self, relay_id: int, payload: bytes):
        self.udp.sendto(_relay_header(relay_id) + payload, (self.host, self.udp_port))

    def recv_relay(self, timeout: float = 3.0):
        try:
            data, _addr = self.udp.recvfrom(65536)
            return data
        except socket.timeout:
            return None


class Checker:
    """极简断言收集器：记录每条断言的 PASS/FAIL，最后统一打印。"""

    def __init__(self, name: str):
        self.name = name
        self.passed = 0
        self.failed = 0

    def check(self, desc: str, ok: bool, detail: str = ""):
        if ok:
            self.passed += 1
            print(f"  PASS  {desc}")
        else:
            self.failed += 1
            print(f"  FAIL  {desc}" + (f"  [{detail}]" if detail else ""))
        return ok

    def summary(self) -> int:
        total = self.passed + self.failed
        status = "OK" if self.failed == 0 else "FAILED"
        print(f"[{self.name}] {status}: {self.passed}/{total} 断言通过")
        return 0 if self.failed == 0 else 1


def unique(tag: str) -> str:
    """生成带运行标记的用户名，保证重复执行时不会与上一轮的残留会话重名。"""
    return f"{tag}_{int(time.time()) % 100000}"


def establish_call(host, port, user_a="tester_a", user_b="tester_b"):
    """两个假客户端登录并完成 offer/early/answer，返回 (a, b, relay_id)。"""
    a = FakeClient(host, port, unique(user_a))
    b = FakeClient(host, port, unique(user_b))
    assert a.login(), "A 登录失败"
    assert b.login(), "B 登录失败"
    time.sleep(0.3)

    a.offer(b.username)
    got = b.wait_for("call_offer", timeout=5.0)
    assert got, "B 未收到 call_offer"

    b.early(a.username)
    a.wait_for("call_early", timeout=5.0)

    b.answer(a.username)
    got = a.wait_for("call_answer", timeout=5.0)
    assert got, "A 未收到 call_answer"

    relay_info = a.wait_for("relay_info", timeout=5.0)
    relay_id = relay_info[1]["relayId"] if relay_info else 0
    b.wait_for("relay_info", timeout=5.0)
    return a, b, relay_id
