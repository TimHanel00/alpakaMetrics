# SPDX-License-Identifier: MPL-2.0
import json
import subprocess
import sys

result = json.loads(subprocess.check_output([sys.argv[1], "--json"], text=True))
assert result["schemaVersion"] == 1
assert result["measurementId"] == 9007199254740993
assert result["label"] == 'quote"slash\\line\n\t'
assert result["provenance"] == {
    "sessionId": 42,
    "queueId": 7,
    "kind": "kernel",
    "queueName": "queue",
    "deviceName": "device",
    "api": "Host",
}
metrics = {entry["name"]: entry for entry in result["metrics"]}
assert metrics["unsigned"]["value"] == 9007199254740993
assert metrics["unsigned"]["valueType"] == "uint64"
assert metrics["signed"]["value"] == -5
assert metrics["signed"]["valueType"] == "int64"
assert metrics["double"]["value"] == 1.25
assert metrics["double"]["valueType"] == "double"
assert metrics["unavailable"]["value"] is None
assert metrics["unavailable"]["status"] == "unsupported"
assert metrics["unavailable"]["diagnostic"] == "missing"
