import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def request(base, path, method="GET", body=None, token=None, expected=None, legacy_key=None, auth_header=None):
    data = None if body is None else json.dumps(body).encode()
    headers = {}
    if token:
        headers["Authorization"] = "Bearer " + token
    if auth_header:
        headers["Authorization"] = auth_header
    if legacy_key:
        headers["X-CustomDB-Key"] = legacy_key
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as response:
            status, payload = response.status, response.read()
    except urllib.error.HTTPError as error:
        status = error.code
        try:
            payload = error.read()
        except OSError:
            # Windows may reset a deliberately closed pre-routing rejection after headers arrive.
            payload = b""
    except ConnectionResetError:
        # WinSock can surface the reset before urllib exposes the already-selected
        # pre-routing denial status. Accept only the denial codes this request expects.
        if os.name != "nt" or expected not in (401, 403):
            raise
        status, payload = expected, b""
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
                    "CUSTOMDB_DATA_DIR": data_dir, "CUSTOMDB_API_KEY": "legacy-secret"})
        refused_env = env.copy()
        refused_env["CUSTOMDB_HOST"] = "0.0.0.0"
        refused_env.pop("CUSTOMDB_ALLOW_NON_LOOPBACK", None)
        refused = subprocess.run([server_exe], env=refused_env, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE, timeout=5)
        assert refused.returncode == 2
        assert b"CUSTOMDB_ALLOW_NON_LOOPBACK=1" in refused.stderr
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

            # A second process must not share the same identity and database directory.
            second_env = env.copy()
            second_env["CUSTOMDB_PORT"] = str(port + 1)
            second_process = subprocess.run([server_exe], env=second_env, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.PIPE, timeout=5)
            assert second_process.returncode == 2
            assert b"already using this data directory" in second_process.stderr

            request(base, "/v1/databases", expected=401)
            request(base, "/v1/databases", legacy_key="legacy-secret", expected=401)
            request(base, "/v1/databases", token="not-a-valid-token", expected=401)
            request(base, "/v1/databases", auth_header="bEaReR not-a-valid-token", expected=401)
            request(base, "/v1/unknown", token="not-a-valid-token", expected=401)
            request(base, "/v1/databases?access_token=not-a-valid-token", expected=401)
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
            request(base, "/v1/databases/alpha/kv/one",
                    auth_header="bEaReR " + writer["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=reader["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", "PUT", {"value": "2"},
                    token=reader["access_token"], expected=403)
            request(base, "/v1/databases/alpha/kv/one", "PUT", {"value": "2"},
                    token=limited["access_token"], expected=403)
            _, account_list = request(base, "/v1/auth/users", token=admin["access_token"], expected=200)
            assert all("password" not in account for account in account_list["users"])
            request(base, "/v1/auth/users/limited01/grants", "PUT",
                    {"grants": [{"database": "alpha", "permission": "write"}]},
                    token=admin["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", "PUT", {"value": "3"},
                    token=limited["access_token"], expected=200)
            request(base, "/v1/auth/users/reader01/grants", "PUT",
                    {"grants": [{"database": "alpha", "permission": "write"}]},
                    token=admin["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", "PUT", {"value": "4"},
                    token=reader["access_token"], expected=403)
            request(base, "/v1/databases/alpha/backups", "POST", {"name": "auth-snapshot"},
                    token=admin["access_token"], expected=201)
            request(base, "/v1/databases/alpha/backups/auth-snapshot/restore", "POST", {"confirm": True},
                    token=writer["access_token"], expected=403)
            request(base, "/v1/databases/beta/kv/one", token=writer["access_token"], expected=403)
            request(base, "/v1/databases", token=reader["access_token"], expected=403)
            request(base, "/v1/unknown", token=writer["access_token"], expected=403)
            request(base, "/v1/auth/users", token=writer["access_token"], expected=403)
            request(base, "/v1/auth/users", "POST",
                    {"username": "blocked01", "password": "a sufficiently long password",
                     "role": "read_only", "grants": []},
                    token=reader["access_token"], expected=403)
            request(base, "/v1/auth/users", "POST",
                    {"username": "newuser01", "password": "a sufficiently long password",
                     "role": "read_only", "grants": [{"database": "alpha", "permission": "read"}]},
                    token=admin["access_token"], expected=201)
            created = login(base, "newuser01", "a sufficiently long password")
            assert created["user"]["role"] == "read_only"
            request(base, "/v1/auth/users/newuser01/password", "PUT",
                    {"password": "replacement account password"}, token=admin["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=created["access_token"], expected=401)
            login(base, "newuser01", "a sufficiently long password", expected=401)
            replacement = login(base, "newuser01", "replacement account password")
            request(base, "/v1/auth/password", "PUT",
                    {"current_password": "replacement account password", "new_password": "another sufficiently long password"},
                    token=replacement["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=replacement["access_token"], expected=401)
            login(base, "newuser01", "replacement account password", expected=401)
            changed = login(base, "newuser01", "another sufficiently long password")
            request(base, "/v1/databases/alpha/kv/one", token=changed["access_token"], expected=200)
            request(base, "/v1/auth/logout", "POST", token=changed["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=changed["access_token"], expected=401)
            request(base, "/v1/auth/users/writer01/enabled", "PUT", {"enabled": False},
                    token=admin["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=writer["access_token"], expected=401)
            request(base, f"/v1/auth/tokens/{reader['token_id']}/revoke", "POST",
                    token=admin["access_token"], expected=200)
            request(base, "/v1/databases/alpha/kv/one", token=reader["access_token"], expected=401)

            request(base, "/v1/auth/users", "POST",
                    {"username": "secondadmin", "password": "second administrator password",
                     "role": "admin", "grants": []},
                    token=admin["access_token"], expected=201)
            second_admin = login(base, "secondadmin", "second administrator password")
            request(base, "/v1/auth/users/rootadmin/enabled", "PUT", {"enabled": False},
                    token=admin["access_token"], expected=200)
            request(base, "/v1/databases", token=admin["access_token"], expected=401)
            request(base, "/v1/auth/users/secondadmin/enabled", "PUT", {"enabled": False},
                    token=second_admin["access_token"], expected=409)
            request(base, "/v1/auth/users", token=second_admin["access_token"], expected=200)

            # Failed password attempts are throttled per remote address.
            for _ in range(5):
                login(base, "secondadmin", "wrong password", expected=401)
            _, throttled = request(base, "/v1/auth/login", "POST",
                {"username": "secondadmin", "password": "wrong password"}, expected=429)
            assert "too many" in throttled["error"]
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
