import json
from typing import Any, Optional
import requests
from fastmcp import FastMCP

UE_BASE_URL = "http://localhost:30010"
REMOTE_CONTROL_API = f"{UE_BASE_URL}/remote/object/property"
REMOTE_CONTROL_CALL = f"{UE_BASE_URL}/remote/object/call"
REMOTE_CONTROL_PRESET = f"{UE_BASE_URL}/remote/preset"
REMOTE_CONTROL_PRESETS = f"{UE_BASE_URL}/remote/presets"

mcp = FastMCP("ue_remote_control")


def _ue_put(endpoint: str, payload: dict) -> dict:
    response = requests.put(
        endpoint,
        headers={"Content-Type": "application/json"},
        data=json.dumps(payload),
        timeout=10,
    )
    response.raise_for_status()
    try:
        return response.json()
    except Exception:
        return {"status": response.status_code, "text": response.text}


def _ue_post(endpoint: str, payload: dict) -> dict:
    response = requests.post(
        endpoint,
        headers={"Content-Type": "application/json"},
        data=json.dumps(payload),
        timeout=10,
    )
    response.raise_for_status()
    try:
        return response.json()
    except Exception:
        return {"status": response.status_code, "text": response.text}


@mcp.tool()
def set_actor_property(
    object_path: str,
    property_name: str,
    property_value: Any,
    access: str = "WRITE_TRANSACTION_ACCESS",
) -> str:
    payload = {
        "objectPath": object_path,
        "access": access,
        "propertyName": property_name,
        "propertyValue": property_value,
    }
    result = _ue_put(REMOTE_CONTROL_API, payload)
    return json.dumps(result)


@mcp.tool()
def get_actor_property(
    object_path: str,
    property_name: str,
) -> str:
    payload = {
        "objectPath": object_path,
        "access": "READ_ACCESS",
        "propertyName": property_name,
    }
    result = _ue_put(REMOTE_CONTROL_API, payload)
    return json.dumps(result)


@mcp.tool()
def call_actor_function(
    object_path: str,
    function_name: str,
    parameters: Optional[dict] = None,
    generate_transaction: bool = True,
) -> str:
    payload = {
        "objectPath": object_path,
        "functionName": function_name,
        "parameters": parameters or {},
        "generateTransaction": generate_transaction,
    }
    result = _ue_post(REMOTE_CONTROL_CALL, payload)
    return json.dumps(result)


@mcp.tool()
def set_dmx_fixture_channels(
    object_path: str,
    channel_map: dict[str, Any],
) -> str:
    results = {}
    for channel_name, value in channel_map.items():
        payload = {
            "objectPath": object_path,
            "access": "WRITE_TRANSACTION_ACCESS",
            "propertyName": channel_name,
            "propertyValue": value,
        }
        try:
            result = _ue_put(REMOTE_CONTROL_API, payload)
            results[channel_name] = result
        except Exception as e:
            results[channel_name] = {"error": str(e)}
    return json.dumps(results)


@mcp.tool()
def batch_set_properties(
    operations: list[dict[str, Any]],
) -> str:
    results = []
    for op in operations:
        try:
            payload = {
                "objectPath": op["objectPath"],
                "access": op.get("access", "WRITE_TRANSACTION_ACCESS"),
                "propertyName": op["propertyName"],
                "propertyValue": op["propertyValue"],
            }
            result = _ue_put(REMOTE_CONTROL_API, payload)
            results.append({"objectPath": op["objectPath"], "property": op["propertyName"], "result": result})
        except Exception as e:
            results.append({"objectPath": op.get("objectPath"), "property": op.get("propertyName"), "error": str(e)})
    return json.dumps(results)


@mcp.tool()
def raw_remote_control(
    method: str,
    endpoint_path: str,
    payload: dict,
) -> str:
    url = f"{UE_BASE_URL}{endpoint_path}"
    method = method.upper()
    if method == "PUT":
        result = _ue_put(url, payload)
    elif method == "POST":
        result = _ue_post(url, payload)
    else:
        raise ValueError(f"Unsupported method: {method}. Use PUT or POST.")
    return json.dumps(result)


@mcp.tool()
def list_remote_presets() -> str:
    response = requests.get(REMOTE_CONTROL_PRESETS, timeout=10)
    response.raise_for_status()
    try:
        return json.dumps(response.json())
    except Exception:
        return response.text


@mcp.tool()
def get_remote_preset(preset_name: str) -> str:
    url = f"{REMOTE_CONTROL_PRESET}/{preset_name}"
    response = requests.get(url, timeout=10)
    response.raise_for_status()
    try:
        return json.dumps(response.json())
    except Exception:
        return response.text


@mcp.tool()
def set_preset_property(
    preset_name: str,
    property_label: str,
    property_value: Any,
) -> str:
    url = f"{REMOTE_CONTROL_PRESET}/{preset_name}/property"
    payload = {
        "propertyLabel": property_label,
        "propertyValue": property_value,
    }
    result = _ue_put(url, payload)
    return json.dumps(result)


if __name__ == "__main__":
    mcp.run()
