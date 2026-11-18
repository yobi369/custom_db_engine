#!/usr/bin/env python3
"""Lightweight stdlib validation for the committed OpenAPI contract."""
import json
import pathlib
import sys

spec_path = pathlib.Path(__file__).resolve().parents[1] / "docs" / "openapi.json"
spec = json.loads(spec_path.read_text(encoding="utf-8"))
assert spec.get("openapi") == "3.1.0", "OpenAPI version must be 3.1.0"
assert spec.get("info", {}).get("title"), "missing API title"
assert spec.get("paths"), "paths must not be empty"

expected = {
    "/health": {"get"},
    "/v1/auth/login": {"post"},
    "/v1/auth/users": {"get", "post"},
    "/v1/auth/users/{username}/grants": {"put"},
    "/v1/auth/users/{username}/enabled": {"put"},
    "/v1/auth/users/{username}/password": {"put"},
    "/v1/auth/password": {"put"},
    "/v1/auth/logout": {"post"},
    "/v1/auth/tokens/{token_id}/revoke": {"post"},
    "/v1/databases": {"get", "post"},
    "/v1/databases/{database}/kv/{key}": {"get", "put", "delete"},
    "/v1/databases/{database}/collections/{collection}/documents": {"get", "post", "put", "delete"},
    "/v1/databases/{database}/schemas": {"post"},
    "/v1/databases/{database}/indexes": {"put", "delete"},
    "/v1/databases/{database}/integrity": {"get"},
    "/v1/databases/{database}/backups": {"get", "post"},
    "/v1/databases/{database}/backups/{backup}/restore": {"post"},
    "/v1/databases/{database}/transactions": {"post"},
}
for path, methods in expected.items():
    assert path in spec["paths"], f"missing documented route: {path}"
    actual = {m for m in spec["paths"][path] if m in {"get", "post", "put", "delete", "patch"}}
    assert methods <= actual, f"{path}: missing methods {methods - actual}"

components = spec.get("components", {}).get("schemas", {})
operation_ids = set()
for path, path_item in spec["paths"].items():
    for method, operation in path_item.items():
        if method not in {"get", "post", "put", "delete", "patch"}:
            continue
        opid = operation.get("operationId")
        assert opid, f"operationId missing for {method.upper()} {path}"
        assert opid not in operation_ids, f"duplicate operationId: {opid}"
        operation_ids.add(opid)
        body = operation.get("requestBody", {}).get("content", {}).get("application/json", {}).get("schema", {})
        ref = body.get("$ref")
        if ref:
            name = ref.split("/")[-1]
            assert name in components, f"unknown schema reference: {ref}"

print(f"OpenAPI contract validated: {len(expected)} paths, {len(operation_ids)} operations")
