"""
场景 5：信令基础与异常输入 —— 登录、用户列表、重名登录踢下线、非法输入。

其中"重名登录"是**回归测试**：服务端按用户名查找会话并转发（forwardTo /
getSessionIp 取第一个匹配），早期允许同一用户名重复登录，导致同名旧连接仍占据
该用户名，后续呼叫信令会被转发到已失效的旧连接上，新登录的一端收不到任何消息。
现在改为：重复登录时先踢掉同名旧会话，再让本次登录生效。

验证：
  1. 登录成功并收到 user_list 广播；
  2. 空用户名被拒绝（login_resp success=false），且不会污染在线列表；
  3. 同一用户名重复登录时，旧连接被服务端踢下线（读到 EOF）；
  4. 踢下线后按用户名转发能正确到达"新的那一个"会话；
  5. 呼叫离线用户返回 call_error，不会静默失败。

用法：python test_signaling_basic.py [--host 127.0.0.1] [--port 8889]
"""

import argparse
import sys
import time

from fake_client import Checker, FakeClient, unique


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8889)
    args = ap.parse_args()

    c = Checker("场景5 信令基础与异常输入")

    # 1) 正常登录
    name = unique("sig")
    a = FakeClient(args.host, args.port, name)
    a.login()
    resp = a.wait_for("login_resp", timeout=5.0)
    c.check("登录成功（login_resp success=true）",
            resp is not None and str(resp[1].get("success")).lower() == "true",
            str(resp[1] if resp else None))
    c.check("登录后收到 user_list 广播", a.wait_for("user_list", timeout=5.0) is not None)

    # 2) 空用户名应被拒绝
    bad = FakeClient(args.host, args.port, "")
    bad.send({"type": "login", "username": ""})
    resp = bad.wait_for("login_resp", timeout=5.0)
    c.check("空用户名被拒绝（success=false）",
            resp is not None and str(resp[1].get("success")).lower() == "false",
            str(resp[1] if resp else None))
    bad.close()

    # 3) 重名登录：旧连接应被踢下线
    b = FakeClient(args.host, args.port, name)
    b.login()
    b.wait_for("login_resp", timeout=5.0)
    time.sleep(0.5)
    c.check("重名登录后旧会话被踢下线（旧连接读到 EOF）", a.is_closed(timeout=3.0))

    # 4) 踢下线后，按用户名转发必须到达新会话
    peer = FakeClient(args.host, args.port, unique("peer"))
    peer.login()
    peer.wait_for("login_resp", timeout=5.0)
    time.sleep(0.3)
    peer.send({"type": "call_offer", "target": name, "sdp": "0:61000", "lanIp": "127.0.0.1"})
    got = b.wait_for("call_offer", timeout=5.0)
    c.check("重名踢下线后转发命中新会话（不再发到失效旧连接）", got is not None)

    # 5) 呼叫离线用户
    peer.send({"type": "call_offer", "target": unique("nobody"), "sdp": "0:61000"})
    got = peer.wait_for("call_error", timeout=5.0)
    c.check("呼叫离线用户返回 call_error（不静默失败）", got is not None)

    # 6) 公网端口发现：同 IP 下多个会话时必须命中"发探针的那一个"
    #    老式 1 字节探针服务端只能按公网 IP 匹配，同一 NAT 后有多台客户端时会串号。
    x = FakeClient(args.host, args.port, unique("nat_x"))
    y = FakeClient(args.host, args.port, unique("nat_y"))
    x.login()
    y.login()
    time.sleep(0.3)

    c.check("带用户名的探针命中自己（回到正确的会话）", y.udp_probe_tagged(y.username))
    c.check("带用户名的探针不会串到同一 IP 下的其他会话",
            len(x.messages("port_discovered")) == 0,
            f"x 收到 {len(x.messages('port_discovered'))} 次")

    # 7) 老式 1 字节探针仍能被服务端接受（向后兼容），且不会把服务端搞坏。
    #    注意：老探针是按公网 IP 匹配的，同一 NAT 下多客户端时可能发错人，
    #    所以这里不断言"发给谁"，只断言"服务端照常工作、后续新探针仍准确"。
    y.udp_probe()
    time.sleep(1.0)
    c.check("老式探针后服务端仍正常：带用户名探针依旧命中自己",
            y.udp_probe_tagged(y.username))

    b.close()
    peer.close()
    x.close()
    y.close()
    return c.summary()


if __name__ == "__main__":
    sys.exit(main())
