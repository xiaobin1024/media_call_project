"""
场景 1：基础路径 —— 双方都能收到直连包，应走完 Probing → P2pProbation → P2p。

验证：
  1. 登录、呼叫、接听、中继分配（relay_info）信令闭环；
  2. UDP 1 字节探针能拿到 port_discovered（NAT 公网端口观察）；
  3. 双方上报 directRx=1 后收到 p2p_probation，epoch 递增；
  4. 观察期持续上报 3s 稳定后收到 p2p（reason=probation_ok），epoch 再递增。

用法：python test_path_basic.py [--host 127.0.0.1] [--port 8889]
"""

import argparse
import sys
import time

from fake_client import Checker, establish_call


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8889)
    args = ap.parse_args()

    c = Checker("场景1 基础路径 P2P")
    a, b, relay_id = establish_call(args.host, args.port, "basic_a", "basic_b")

    c.check("通话建立后服务端分配了 relayId", relay_id > 0, f"relayId={relay_id}")

    # UDP 端口观察：用带用户名的探针（与客户端实际发送的一致）。
    # 老式 1 字节探针服务端只能按公网 IP 匹配，同一 NAT 下多客户端会串号，故不再使用。
    c.check("带用户名的 UDP 探针回传 port_discovered", a.udp_probe_tagged(a.username))

    # 双方持续上报收到直连包
    a.keep_reporting(b.username, True, interval=0.5, duration=20.0, reason="first_direct")
    b.keep_reporting(a.username, True, interval=0.5, duration=20.0, reason="first_direct")

    # 期待进入观察期
    got = a.wait_for("path_decision", timeout=8.0,
                     predicate=lambda m: m.get("mode") == "p2p_probation")
    c.check("双方均收到直连 → 进入 p2p_probation", got is not None)
    probation_epoch = got[1]["epoch"] if got else 0
    c.check("p2p_probation 的 epoch 已递增(>=1)", probation_epoch >= 1,
            f"epoch={probation_epoch}")
    c.check("对端也收到同一份 p2p_probation 决策",
            b.latest("path_decision") is not None
            and b.latest("path_decision").get("mode") == "p2p_probation")

    # 观察期稳定后应转 P2P
    got = a.wait_for("path_decision", timeout=10.0,
                     predicate=lambda m: m.get("mode") == "p2p")
    c.check("观察期稳定 3s → 转入 p2p", got is not None)
    if got:
        p2p_epoch = got[1]["epoch"]
        c.check("p2p 的 epoch 大于观察期的 epoch", p2p_epoch > probation_epoch,
                f"{probation_epoch} -> {p2p_epoch}")
        c.check("p2p 决策原因为 probation_ok", got[1].get("reason") == "probation_ok",
                str(got[1].get("reason")))

    # 挂断后服务端应释放绑定（不再有新决策）
    a.hang_up(b.username)
    time.sleep(1.0)

    a.close()
    b.close()
    return c.summary()


if __name__ == "__main__":
    sys.exit(main())
