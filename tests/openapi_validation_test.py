#!/usr/bin/env python3
"""Lightweight stdlib validation for the committed OpenAPI contract."""
import json
import pathlib

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
methods = {"get", "post", "put", "delete", "patch", "head", "options"}
assert set(spec["paths"]) == set(expected), "documented route set differs from expected API route set"
for path, expected_methods in expected.items():
    actual_methods = {m for m in spec["paths"][path] if m in methods}
    assert actual_methods == expected_methods, f"{path}: expected methods {expected_methods}, got {actual_methods}"

schemas = spec.get("components", {}).get("schemas", {})
operation_ids = set()
operation_count = 0

def resolve_ref(ref):
    assert ref.startswith("#/"), f"unsupported external reference: {ref}"
    value = spec
    for segment in ref[2:].split("/"):
        segment = segment.replace("~1", "/").replace("~0", "~")
        assert isinstance(value, dict) and segment in value, f"unresolved schema reference: {ref}"
        value = value[segment]
    return value

def check_refs(value):
    if isinstance(value, dict):
        if "$ref" in value:
            resolve_ref(value["$ref"])
        for child in value.values():
            check_refs(child)
    elif isinstance(value, list):
        for child in value:
            check_refs(child)

check_refs(spec)
for path, path_item in spec["paths"].items():
    # Avoid dependency on regex: OpenAPI path variables are bracket-delimited.
    path_parameters = {part.split("}", 1)[0] for part in path.split("{")[1:]}
    for method, operation in path_item.items():
        if method not in methods:
            continue
        operation_count += 1
        opid = operation.get("operationId")
        assert opid, f"operationId missing for {method.upper()} {path}"
        assert opid not in operation_ids, f"duplicate operationId: {opid}"
        operation_ids.add(opid)
        assert operation.get("responses"), f"responses missing for {method.upper()} {path}"
        parameters = operation.get("parameters", [])
        declared_path_parameters = {p.get("name") for p in parameters if p.get("in") == "path"}
        assert path_parameters <= declared_path_parameters, (
            f"{method.upper()} {path}: path parameters not declared: "
            f"{path_parameters - declared_path_parameters}"
        )
        for parameter in parameters:
            assert parameter.get("name") and parameter.get("in"), f"invalid parameter in {method.upper()} {path"
            assert "schema" in parameter, f"parameter schema missing in {method.upper()} {path"
        body = operation.get("requestBody", {}).get("content", {}).get("application/json", {}).get("schema", {})
        if body.get("$ref"):
            assert body["$ref"].split("/")[-1] in schemas, f"unknown request schema: {body['$ref']}"

# Arbitrary document filter names cannot be modeled exactly in OpenAPI. Ensure the
# documented placeholder exists and destructive filter operations explain the rule.
documents_path = "/v1/databases/{database}/collections/{collection}/documents"
for method in ("get", "put", "delete"):
    params = spec["paths"][documents_path][method].get("parameters", [])
    assert any(p.get("name") == "field" and p.get("in") == "query" for p in params), (
        f"{method.upper()} documents must describe dynamic equality-filter query parameters"
    )
assert any(p.get("name") == "field" and p.get("in") == "query" and p.get("required") is True
           for p in spec["paths"]["/v1/databases/{database}/indexes"]["delete"].get("parameters", [])), (
    "DELETE index must declare its required field query parameter"
)

print(f"OpenAPI contract validated: {len(expected)} paths, {operation_count} operations")
