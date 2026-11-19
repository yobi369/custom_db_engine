import json
from concurrent.futures import ThreadPoolExecutor
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def request(base, path, method="GET", body=None, key="test-secret", expected=None):
    data = None if body is None else (body.encode() if isinstance(body, str) else json.dumps(body).encode())
    headers = {"X-CustomDB-Key": key}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as response:
            status, payload = response.status, response.read()
    except urllib.error.HTTPError as error:
        status, payload = error.code, error.read()
    if expected is not None and status != expected:
        raise AssertionError(f"{method} {path}: expected {expected}, got {status}: {payload!r}")
    return status, json.loads(payload) if payload else {}


def main():
    executable = os.path.abspath(sys.argv[1])
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="customdb-server-test-") as data_dir:
        env = os.environ.copy()
        env.update({
            "CUSTOMDB_HOST": "127.0.0.1",
            "CUSTOMDB_PORT": str(port),
            "CUSTOMDB_DATA_DIR": data_dir,
            "CUSTOMDB_API_KEY": "test-secret",
        })
        process = subprocess.Popen([executable], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        base = f"http://127.0.0.1:{port}"
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode(errors="replace"))
                try:
                    request(base, "/health", key="", expected=200)
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.05)
            else:
                raise TimeoutError("CustomDB server did not become ready")

            _, live = request(base, "/health/live", key="", expected=200)
            assert live["status"] == "alive"
            _, ready = request(base, "/health/ready", key="", expected=200)
            assert ready["status"] == "ready"
            assert ready["checks"]["data_directory"] is True
            assert ready["checks"]["filesystem_capacity"] is True
            assert isinstance(ready["available_bytes"], int) and ready["available_bytes"] >= 0
            request(base, "/v1/databases", key="wrong", expected=401)
            request(base, "/v1/databases", "POST", {"name": "../escape"}, expected=400)
            request(base, "/v1/databases", "POST", "{malformed-json", expected=400)
            request(base, "/v1/databases", "POST", {}, expected=400)
            request(base, "/v1/databases", "POST", {"name": "integration"}, expected=201)
            _, listed = request(base, "/v1/databases")
            assert "integration" in listed["databases"]
            request(base, "/v1/databases/missing/kv/key", expected=404)
            request(base, "/v1/databases", "POST", {"name": "integration"}, expected=409)

            request(base, "/v1/databases/integration/kv/greeting", "PUT", {"value": "hello"}, expected=200)
            _, result = request(base, "/v1/databases/integration/kv/greeting")
            assert result["value"] == "hello"
            request(base, "/v1/databases/integration/kv/greeting", "DELETE", expected=200)
            request(base, "/v1/databases/integration/kv/greeting", expected=404)

            request(base, "/v1/databases/integration/collections/users/documents", "POST",
                    "{malformed-json", expected=400)
            request(base, "/v1/databases/integration/collections/users/documents", "POST",
                    ["not", "an", "object"], expected=400)
            request(base, "/v1/databases/integration/collections/users/documents", "POST",
                    {"name": "Ada", "role": "admin"}, expected=201)
            _, result = request(base, "/v1/databases/integration/collections/users/documents?role=admin")
            assert result["count"] == 1 and result["documents"][0]["name"] == "Ada"

            request(base, "/v1/databases/integration/collections/users/documents", "PUT",
                    {"name": "Ada Lovelace", "role": "engineer"}, expected=400)
            request(base, "/v1/databases/integration/collections/users/documents?role=admin", "PUT",
                    {"name": "Ada Lovelace", "role": "engineer"}, expected=200)
            _, result = request(base, "/v1/databases/integration/collections/users/documents?role=engineer")
            assert result["count"] == 1 and result["documents"][0]["name"] == "Ada Lovelace"
            request(base, "/v1/databases/integration/collections/users/documents?role=missing", "DELETE",
                    expected=404)
            request(base, "/v1/databases/integration/collections/users/documents", "DELETE",
                    expected=400)
            request(base, "/v1/databases/integration/collections/users/documents?role=engineer", "DELETE",
                    expected=200)
            _, result = request(base, "/v1/databases/integration/collections/users/documents")
            assert result["count"] == 0

            request(base, "/v1/databases/integration/schemas", "POST",
                    {"collection": "members", "type": "object", "required": ["name"],
                     "properties": {"name": {"type": "string"}}}, expected=201)
            request(base, "/v1/databases/integration/collections/members/documents", "POST",
                    {"age": 10}, expected=400)
            request(base, "/v1/databases/integration/collections/members/documents", "POST",
                    {"name": "Grace"}, expected=201)

            request(base, "/v1/databases/integration/indexes", "PUT", {"field": "name"}, expected=201)
            _, result = request(base, "/v1/databases/integration/collections/members/documents?name=Grace")
            assert result["count"] == 1
            request(base, "/v1/databases/integration/indexes?field=name", "DELETE", expected=200)

            request(base, "/v1/databases/integration/transactions", "POST",
                    {"operations": [{"type": "put", "key": "atomic", "value": "committed"}]}, expected=200)
            _, result = request(base, "/v1/databases/integration/kv/atomic")
            assert result["value"] == "committed"
            request(base, "/v1/databases/integration/transactions", "POST",
                    {"operations": [{"type": "put", "key": "rolled-back", "value": "no"},
                                    {"type": "not_supported"}]}, expected=400)
            request(base, "/v1/databases/integration/kv/rolled-back", expected=404)

            request(base, "/v1/databases/integration/integrity", key="wrong", expected=401)
            _, integrity = request(base, "/v1/databases/integration/integrity")
            assert integrity["status"] == "ok"
            request(base, "/v1/databases/integration/backups", "POST",
                    {"name": "before-restore"}, expected=201)
            _, backups = request(base, "/v1/databases/integration/backups")
            assert any(item["name"] == "before-restore" for item in backups["backups"])
            request(base, "/v1/databases/integration/kv/atomic", "PUT",
                    {"value": "changed-after-backup"}, expected=200)
            request(base, "/v1/databases/integration/backups/before-restore/restore",
                    "POST", {}, expected=400)
            request(base, "/v1/databases/integration/backups/before-restore/restore",
                    "POST", {"confirm": True}, expected=200)
            _, restored = request(base, "/v1/databases/integration/kv/atomic")
            assert restored["value"] == "committed"
            _, integrity = request(base, "/v1/databases/integration/integrity")
            assert integrity["status"] == "ok"
            request(base, "/v1/databases/integration/backups", "POST",
                    {"name": "../escape"}, expected=400)
            request(base, "/v1/databases/integration/backups", "POST",
                    {"name": "before-restore"}, expected=409)

            # Concurrent HTTP clients must remain correct under server serialization.
            def write_concurrent(index):
                request(base, f"/v1/databases/integration/kv/concurrent-{index}",
                        "PUT", {"value": f"value-{index}"}, expected=200)
            with ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(write_concurrent, range(32)))
            for index in range(32):
                _, value = request(base, f"/v1/databases/integration/kv/concurrent-{index}")
                assert value["value"] == f"value-{index}"

            # Restart against the same data directory and verify durable state.
            process.terminate()
            process.wait(timeout=5)
            process = subprocess.Popen([executable], env=env, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.PIPE)
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode(errors="replace"))
                try:
                    request(base, "/health", key="", expected=200)
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.05)
            else:
                raise TimeoutError("CustomDB server did not restart")
            _, persisted = request(base, "/v1/databases/integration/kv/atomic")
            assert persisted["value"] == "committed"
            _, persisted_docs = request(base, "/v1/databases/integration/collections/members/documents?name=Grace")
            assert persisted_docs["count"] == 1
            _, persisted_backups = request(base, "/v1/databases/integration/backups")
            assert any(item["name"] == "before-restore" for item in persisted_backups["backups"])
            _, integrity = request(base, "/v1/databases/integration/integrity")
            assert integrity["status"] == "ok"
            print("CustomDB HTTP integration and restart persistence tests passed")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
