"""
场景 3：回切失败 —— 观察期内一方上报中断，应立刻退回中继，且迟到的旧上报不得误判。

验证：
  1. 双方上报直连 → 进入 p2p_probation；
  2. 随后 B 停止上报，A 继续上报，3s 后服务端按 report_timeout 退回 relay，epoch 递增；
  3. 退回 relay 后，A 单方面的迟到直连上报**不得**让路径立刻切回（10s 冷却 + 需双方确认）。

这是"双向确认 + 单一状态源"的回归测试：单方面的乐观上报不能决定路径。

用法：python test_path_reprobe_fail.py [--host 127.0.0.1] [--port 8889]
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

    c = Checker("场景3 回切失败 / 上报超时")
    a, b, relay_id = establish_call(args.host, args.port, "fail_a", "fail_b")
    c.check("通话建立后服务端分配了 relayId", relay_id > 0, f"relayId={relay_id}")

    # 1) 双方上报直连，进入观察期
    a.keep_reporting(b.username, True, interval=0.5, duration=12.0, reason="first_direct")
    b.keep_reporting(a.username, True, interval=0.5, duration=3.0, reason="first_direct")

    got = a.wait_for("path_decision", timeout=8.0,
                     predicate=lambda m: m.get("mode") == "p2p_probation")
    c.check("双方直连 → 进入 p2p_probation", got is not None)
    probation_epoch = got[1]["epoch"] if got else 0

    # 2) B 停止上报（上面的 duration=3s 到期），A 仍在报；3s 后应 report_timeout 退回 relay
    got = a.wait_for("path_decision", timeout=12.0,
                     predicate=lambda m: m.get("mode") == "relay")
    c.check("一方上报中断 → 按 report_timeout 退回 relay", got is not None)
    relay_epoch = got[1]["epoch"] if got else 0
    c.check("退回原因为 report_timeout", got is not None
            and got[1].get("reason") == "report_timeout",
            str(got[1].get("reason") if got else None))
    c.check("relay epoch 大于观察期 epoch", relay_epoch > probation_epoch,
            f"{probation_epoch} -> {relay_epoch}")

    # 3) 退回 relay 后，A 单方面继续上报直连，冷却期内不得切回
    #    since=relay_at 很关键：否则会把第 1 步那条 p2p_probation 再次匹配上（假阳性）
    relay_at = got[0] if got else time.time()
    a.keep_reporting(b.username, True, interval=0.5, duration=6.0, reason="late_report")
    back = a.wait_for("path_decision", timeout=5.0, since=relay_at,
                      predicate=lambda m: m.get("mode") == "p2p_probation")
    c.check("单方面迟到上报不会让路径切回（需双方确认 + 冷却）", back is None,
            "仅 A 上报就切回了观察期" if back else "")

    final = a.latest("path_decision")
    c.check("最终路径仍是 relay", final is not None and final.get("mode") == "relay",
            str(final.get("mode") if final else None))

    a.hang_up(b.username)
    time.sleep(0.5)
    a.close()
    b.close()
    return c.summary()


if __name__ == "__main__":
    sys.exit(main())
