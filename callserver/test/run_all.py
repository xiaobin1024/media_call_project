"""
路径测试总入口：拉起服务端 → 依次跑 4 个场景 → 汇总结果 → 优雅停止服务端。

用法：
    python run_all.py                        # 自动查找并拉起服务端，跑全部场景
    python run_all.py --server-bin ../build/call_server
    python run_all.py --no-server            # 服务端已在跑，只跑场景

说明：
  - 场景 2/3 依赖服务端 4s 超时、10s 冷却、3s 稳定窗口等真实计时，
    单套约 30s，整套约 1.5 分钟，属正常耗时，不是卡住。
  - 结束时用 SIGTERM 停止服务端并等待其自行退出，顺带验证优雅关闭。
"""

import argparse
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = [
    "test_signaling_basic.py",
    "test_path_basic.py",
    "test_path_reprobe_ok.py",
    "test_path_reprobe_fail.py",
    "test_relay_media.py",
]


def find_server_bin() -> str:
    candidates = [
        os.path.join(HERE, "..", "build", "call_server"),
        os.path.join(HERE, "..", "build", "call_server.exe"),
        os.path.join(HERE, "..", "build", "Debug", "call_server.exe"),
        os.path.join(HERE, "..", "build", "Release", "call_server.exe"),
    ]
    for c in candidates:
        if os.path.exists(c):
            return os.path.abspath(c)
    return ""


def wait_port(host: str, port: int, timeout: float = 10.0) -> bool:
    import socket
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8889)
    ap.add_argument("--server-bin", default="")
    ap.add_argument("--no-server", action="store_true")
    args = ap.parse_args()

    proc = None
    if not args.no_server:
        bin_path = args.server_bin or find_server_bin()
        if not bin_path:
            print("未找到服务端可执行文件，请用 --server-bin 指定，或加 --no-server 使用已运行的服务端")
            return 2
        print(f"[runner] 启动服务端: {bin_path} --port {args.port}")
        proc = subprocess.Popen([bin_path, "0.0.0.0", str(args.port)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_port(args.host, args.port):
            print("[runner] 服务端端口未就绪，退出")
            proc.terminate()
            return 2
        print(f"[runner] 服务端已就绪 {args.host}:{args.port}")

    results = []
    for name in SCENARIOS:
        print(f"\n[runner] ==== {name} ====")
        rc = subprocess.call([sys.executable, os.path.join(HERE, name),
                              "--host", args.host, "--port", str(args.port)])
        results.append((name, rc))

    print("\n[runner] ================= 汇总 =================")
    failed = 0
    for name, rc in results:
        print(f"  {'OK    ' if rc == 0 else 'FAILED'}  {name}")
        if rc != 0:
            failed += 1
    print(f"[runner] {len(results) - failed}/{len(results)} 个场景通过")

    # 优雅停止：发 SIGTERM 后等待自行退出
    if proc is not None:
        print("[runner] 发送 SIGTERM，等待服务端自行退出...")
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
            print(f"[runner] 服务端已自行退出（返回码 {rc}），优雅关闭正常")
        except subprocess.TimeoutExpired:
            proc.kill()
            print("[runner] 服务端 10s 内未退出，已强制 KILL（优雅关闭异常）")
            failed += 1

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
