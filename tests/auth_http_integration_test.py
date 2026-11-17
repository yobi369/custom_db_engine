import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def request(base, path, method="GET", body=None, token=None, expected=None):
    data = None if body is None else json.dumps(body).encode()
    headers = {}
    if token:
        headers["Authorization"] = "Bearer " + token
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as response:
            status, payload = response.status, response.read()
    except urllib.error.HTTPError as error:
        status, payload = error.code, error.read()
    decoded = json.loads(payload) if payload else {}
    if expected is not None and status != expected:
        raise AssertionError(f"{method} {path}: expected {expected}, got {status}: {decoded!r}")
    return status, decoded


def login(base, username, password, expected=200):
    _, result = request(base, "/v1/auth/login", "POST",
                        {"username": username, "password": password}, expected=expected)
    return result


def main():
    server_exe, fixture_exe = map(os.path.abspath, sys.argv[1:3])
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="customdb-auth-http-") as data_dir:
        subprocess.run([fixture_exe, data_dir], check=True)
        env = os.environ.copy()
        env.update({"CUSTOMDB_HOST": "127.0.0.1", "CUSTOMDB_PORT": str(port),
                    "CUSTOMDB_DATA_DIR": data_dir})
        process = subprocess.Popen([server_exe], env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE)
        base = f"http://127.0.0.1:{port}"
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode(errors="replace"))
                try:
                    request(base, "/health", expected=200)
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.05)
            else:
                raise TimeoutError("CustomDB auth server did not become ready")

            request(base, "/v1/databases", expected=401)
            login(base, "rootadmin", "wrong password", expected=401)
            admin = login(base, "rootadmin", "administrator password 123")
            writer = login(base, "writer01", "writer password 123")
            reader = login(base, "reader01", "reader password 123")
            limited = login(base, "limited01", "limited password 123")

            request(base, "/v1/databases", "POST", {"name": "alpha"},
                    token=admin["access_token"], expected=201)
            request(base, "/v1/databases", "POST", {"name": "beta"},
                    token=admin["access_token"], expected=201)
            request(base, "/v1/databases/alpha/kv/one", "PUT", {"value": "1"},
                    token=writer["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=reader["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", "PUT", {"value": "2"},
                    token=reader["access_token"], expected=403)
            request(base, "/v1/databases/alpha/kv/one", "PUT", {"value": "2"},
                    token=limited["access_token"], expected=403)
            request(base, "/v1/databases/beta/kv/one", token=writer["access_token"], expected=403)
            request(base, "/v1/databases", token=reader["access_token"], expected=403)
            request(base, "/v1/auth/users", "POST",
                    {"username": "blocked01", "password": "a sufficiently long password",
                     "role": "read_only", "grants": []},
                    token=reader["access_token"], expected=403)
            request(base, "/v1/auth/users", "POST",
                    {"username": "newuser01", "password": "a sufficiently long password",
                     "role": "read_only", "grants": [{"database": "alpha", "permission": "read"}]},
                    token=admin["access_token"], expected=201)
            request(base, f"/v1/auth/tokens/{reader['token_id']}/revoke", "POST",
                    token=admin["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=reader["access_token"], expected=401)
            print("CustomDB bearer authentication and authorization matrix passed")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
