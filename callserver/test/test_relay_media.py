"""
场景 4：UDP 中继转发 —— 验证媒体包能完整穿过服务端，且未知 relayId 被丢弃。

这组是**回归测试**：早期服务端中继接收缓冲只有 64 字节，1200 字节的视频分片
会被截断成 43 字节，表现为"中继模式下对方画面静止/花屏"。缓冲改为 4096 后修复。
这里直接断言"发多少收多少"，防止该问题再次出现。

验证：
  1. A 先发（服务端学习端点 A），B 再发（学习端点 B 并转发给 A）；
  2. A 与 B 收到的负载长度与内容必须与发送完全一致（1200B / 4000B 两种）；
  3. 未知 relayId 的数据报应被丢弃，不会串到别的通话里。

用法：python test_relay_media.py [--host 127.0.0.1] [--port 8889]
"""

import argparse
import os
import sys

from fake_client import Checker, establish_call


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8889)
    args = ap.parse_args()

    c = Checker("场景4 UDP 中继转发")
    a, b, relay_id = establish_call(args.host, args.port, "relay_a", "relay_b")
    c.check("通话建立后服务端分配了 relayId", relay_id > 0, f"relayId={relay_id}")

    # 1) A 先发：服务端学习端点 A（此时 B 未学习，不应转发）
    payload_a = os.urandom(1200)
    a.send_relay(relay_id, payload_a)
    c.check("仅一端发过包时不转发（对端尚未学习）", b.recv_relay(timeout=1.5) is None)

    # 2) B 再发：学习端点 B，并把这一包转发给 A
    payload_b = os.urandom(1200)
    b.send_relay(relay_id, payload_b)
    got_at_a = a.recv_relay(timeout=3.0)
    c.check("B 的包经服务端中继到达 A", got_at_a is not None)
    c.check("A 收到的负载长度未被截断（1200B）",
            got_at_a is not None and len(got_at_a) == 1200,
            f"实际 {len(got_at_a) if got_at_a else 0} 字节")
    c.check("A 收到的负载内容与 B 发送一致", got_at_a == payload_b)

    # 3) A 再发：此时双向已学习，应转发给 B
    payload_a2 = os.urandom(1200)
    a.send_relay(relay_id, payload_a2)
    got_at_b = b.recv_relay(timeout=3.0)
    c.check("A 的包经服务端中继到达 B", got_at_b is not None)
    c.check("B 收到的负载长度未被截断（1200B）",
            got_at_b is not None and len(got_at_b) == 1200,
            f"实际 {len(got_at_b) if got_at_b else 0} 字节")
    c.check("B 收到的负载内容与 A 发送一致", got_at_b == payload_a2)

    # 4) 比 RTP 分片更大的包，确认服务端接收缓冲（4096B）没有截断。
    #    注意：公网 MTU 约 1500B，超过就会 IP 分片并被大量丢弃——那是链路层限制，
    #    不是服务端问题。所以跨公网跑时用 1400B（单个 IP 包装得下），
    #    本机回环才用 4000B 去压缓冲上限。
    is_loopback = args.host in ("127.0.0.1", "localhost", "::1")
    big_size = 4000 if is_loopback else 1400
    print(f"  (大包检查：{big_size}B，{'回环' if is_loopback else '跨公网按 MTU 降档'})")
    big = os.urandom(big_size)
    a.send_relay(relay_id, big)
    got_big = b.recv_relay(timeout=3.0)
    c.check(f"{big_size}B 大包也完整送达（缓冲未截断）",
            got_big is not None and len(got_big) == big_size,
            f"实际 {len(got_big) if got_big else 0} 字节")

    # 5) 未知 relayId 应被丢弃，不会串到本通话
    bogus = os.urandom(64)
    a.send_relay(relay_id + 9999, bogus)
    c.check("未知 relayId 的数据报被丢弃（不串话）", b.recv_relay(timeout=1.5) is None)

    a.hang_up(b.username)
    a.close()
    b.close()
    return c.summary()


if __name__ == "__main__":
    sys.exit(main())
