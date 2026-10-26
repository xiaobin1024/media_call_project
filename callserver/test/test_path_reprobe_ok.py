"""
场景 2：回切成功 —— 打洞失败先退中继，冷却期后双方恢复直连，应成功回切 P2P。

验证：
  1. 建立通话后**不**上报直连，4s 超时后服务端判 direct_timeout 退入 relay，epoch 递增；
  2. relay 冷却 10s 内即便持续上报直连也不得回切（防抖）；
  3. 冷却结束后双方持续直连 3s 稳定 → p2p_probation（reason=reprobe_ok）；
  4. 再稳定 3s → p2p，epoch 全程单调递增。

用法：python test_path_reprobe_ok.py [--host 127.0.0.1] [--port 8889]
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

    c = Checker("场景2 回切成功 relay→P2P")
    a, b, relay_id = establish_call(args.host, args.port, "re_a", "re_b")
    c.check("通话建立后服务端分配了 relayId", relay_id > 0, f"relayId={relay_id}")

    # 1) 不上报直连，等 Probing 4s 超时
    got = a.wait_for("path_decision", timeout=10.0,
                     predicate=lambda m: m.get("mode") == "relay")
    c.check("4s 内无直连 → 回退 relay", got is not None)
    relay_epoch = got[1]["epoch"] if got else 0
    c.check("relay 决策原因为 direct_timeout", got is not None
            and got[1].get("reason") == "direct_timeout",
            str(got[1].get("reason") if got else None))
    relay_at = time.time()

    # 2) 冷却期内持续上报直连，但不得回切
    a.keep_reporting(b.username, True, interval=0.5, duration=40.0, reason="reprobe")
    b.keep_reporting(a.username, True, interval=0.5, duration=40.0, reason="reprobe")

    early = a.wait_for("path_decision", timeout=6.0, since=relay_at,
                       predicate=lambda m: m.get("mode") == "p2p_probation")
    c.check("10s 冷却期内不会提前回切（防抖生效）", early is None,
            "冷却期内就收到了 p2p_probation" if early else "")

    # 3) 冷却结束 + 3s 稳定窗口后应回切观察期
    got = a.wait_for("path_decision", timeout=20.0,
                     predicate=lambda m: m.get("mode") == "p2p_probation")
    c.check("冷却后双方直连稳定 → 回切 p2p_probation", got is not None)
    probation_epoch = got[1]["epoch"] if got else 0
    c.check("回切原因为 reprobe_ok", got is not None
            and got[1].get("reason") == "reprobe_ok",
            str(got[1].get("reason") if got else None))
    c.check("回切 epoch 大于 relay epoch", probation_epoch > relay_epoch,
            f"{relay_epoch} -> {probation_epoch}")
    c.check("回切发生在冷却期之后", time.time() - relay_at >= 10.0,
            f"距 relay {time.time() - relay_at:.1f}s")

    # 4) 再稳定后进入 p2p
    got = a.wait_for("path_decision", timeout=10.0,
                     predicate=lambda m: m.get("mode") == "p2p")
    c.check("回切后稳定 → 进入 p2p", got is not None)
    if got:
        c.check("p2p epoch 大于观察期 epoch", got[1]["epoch"] > probation_epoch,
                f"{probation_epoch} -> {got[1]['epoch']}")

    a.hang_up(b.username)
    time.sleep(0.5)
    a.close()
    b.close()
    return c.summary()


if __name__ == "__main__":
    sys.exit(main())
