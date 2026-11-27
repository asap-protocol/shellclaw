"""Start a local gateway and run the ASAP compliance and SDK checks."""

from collections.abc import Iterator
from contextlib import contextmanager
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import URLError
from urllib.request import ProxyHandler, build_opener

ROOT = Path(__file__).resolve().parents[1]


def wait_for_gateway(process: subprocess.Popen, url: str) -> None:
    opener = build_opener(ProxyHandler({}))
    deadline = time.monotonic() + 10
    while process.poll() is None and time.monotonic() < deadline:
        try:
            with opener.open(f"{url}/health", timeout=0.25) as response:
                if response.status == 200:
                    return
        except (URLError, TimeoutError):
            time.sleep(0.05)
    raise RuntimeError("The test gateway did not become ready")


@contextmanager
def running_gateway(binary: Path) -> Iterator[str]:
    fixture = ROOT / "tests/fixtures/ci_asap_test.toml"
    with tempfile.TemporaryDirectory(prefix="shellclaw-asap-") as directory:
        home = Path(directory)
        (home / ".shellclaw").mkdir()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config = home / "config.toml"
        config.write_text(fixture.read_text(encoding="utf-8").format(port=port), encoding="utf-8")
        url = f"http://127.0.0.1:{port}"
        # Use only fixture settings and an empty ShellClaw home.
        gateway_env = {"HOME": str(home), "PATH": os.environ["PATH"]}
        with subprocess.Popen(
            [binary, "--config", config], cwd=home, env=gateway_env,
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        ) as gateway:
            try:
                wait_for_gateway(gateway, url)
                yield url
            finally:
                if gateway.poll() is None:
                    gateway.terminate()
                    try:
                        gateway.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        gateway.kill()


def run(binary: Path) -> None:
    python_path = f"{Path(sys.executable).parent}{os.pathsep}{os.environ['PATH']}"
    env = dict(os.environ, PATH=python_path, ASAP_COMPLIANCE_SKIP_PIP="1",
               NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
    env.setdefault("ASAP_COMPLIANCE_TIMEOUT", "5")
    with running_gateway(binary) as url:
        subprocess.run([ROOT / "scripts/run_asap_compliance.sh", url], env=env, check=True)
    # Start a new gateway so the validators do not consume the SDK tests' rate limit.
    with running_gateway(binary) as url:
        subprocess.run([sys.executable, ROOT / "scripts/check_asap_sdk.py", url], env=env, check=True)


if __name__ == "__main__":
    try:
        run(Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/shellclaw")
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
