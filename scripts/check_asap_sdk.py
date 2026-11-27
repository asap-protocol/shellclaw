"""Exercise ShellClaw with the official SDK and check JSON-RPC on the wire."""

import asyncio
from decimal import Decimal
import json
import sys
import unittest
from unittest.mock import patch

import httpx
from asap.errors import RemoteRPCError
from asap.models.envelope import Envelope
from asap.models.payloads import TaskRequest
from asap.transport.client import ASAPClient

SENDER = "urn:asap:agent:compliance-harness"
AGENT = "urn:asap:agent:under-test"
FIRST_ID = 5000000000000001
SECOND_ID = 9007199254740993


def echo(input_data, recipient=AGENT):
    return Envelope(
        asap_version="0.1",
        sender=SENDER,
        recipient=recipient,
        correlation_id="previous-request",
        trace_id="sdk-ci-trace",
        payload_type="TaskRequest",
        payload=TaskRequest(conversation_id="sdk-ci", skill_id="echo", input=input_data),
    )


class SDKChecks(unittest.IsolatedAsyncioTestCase):
    async def check_echo(self, request, encoding):
        encodings = []

        async def record_encoding(http_request):
            encodings.append(http_request.headers.get("content-encoding"))

        async with ASAPClient(self.url, max_retries=1, require_https=False) as client:
            client._client.event_hooks["request"].append(record_encoding)
            response = await client.send(request)

        self.assertEqual(encodings, [encoding])
        self.assertEqual(response.payload.status, "completed")
        self.assertEqual(response.payload.result, request.payload.input)
        self.assertEqual(response.correlation_id, request.id)
        self.assertEqual(response.payload.task_id, request.id)
        self.assertEqual(response.trace_id, request.trace_id)
        self.assertEqual(response.recipient, request.sender)
        self.assertEqual(response.sender, request.recipient)
        return response.payload.result

    async def test_identity_preserves_nested_numbers(self):
        result = await self.check_echo(echo({
            "first": FIRST_ID,
            "nested": [SECOND_ID, {"signed": -SECOND_ID, "float": 1.25, "bool": True}],
            "escaped": "línea\x00with NUL",
        }), None)
        self.assertIs(type(result["first"]), int)
        self.assertIs(type(result["nested"][0]), int)
        self.assertIs(type(result["nested"][1]["bool"]), bool)

    async def test_default_brotli(self):
        await self.check_echo(echo({"text": "Standard SDK compression. " * 90}), "br")

    async def test_default_gzip_without_brotli(self):
        with patch("asap.transport.compression.is_brotli_available", return_value=False):
            await self.check_echo(echo({"text": "Standard SDK compression. " * 90}), "gzip")

    async def test_invalid_recipient_is_remote_rpc_error(self):
        async with ASAPClient(self.url, max_retries=1, require_https=False) as client:
            request = echo({"text": "SDK error test"}, recipient="urn:asap:agent:other")
            with self.assertRaises(RemoteRPCError) as caught:
                await client.send(request)
        self.assertEqual(caught.exception.json_rpc_code, -32602)

    async def test_exact_numeric_ids_on_success_and_parse_error(self):
        envelope = echo({}).model_dump(mode="json")
        async with httpx.AsyncClient(trust_env=False) as client:
            request = {"jsonrpc": "2.0", "method": "asap.send", "id": FIRST_ID,
                       "params": {"envelope": envelope}}
            response = await client.post(f"{self.url}/asap", json=request)
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.json()["id"], FIRST_ID)
            self.assertIs(type(response.json()["id"]), int)

            request["id"] = SECOND_ID
            request["params"] = {"envelope": {}}
            response = await client.post(f"{self.url}/asap", json=request)
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.json()["error"]["code"], -32602)
            self.assertEqual(response.json()["id"], SECOND_ID)
            self.assertIs(type(response.json()["id"]), int)

    async def test_flat_http_contract(self):
        async with httpx.AsyncClient(trust_env=False) as client:
            request = {
                "jsonrpc": "2.0", "method": "asap.send", "id": SECOND_ID,
                "params": {"id": "legacy", "asap_version": "2.1", "sender": SENDER,
                           "recipient": AGENT, "payload_type": "task.cancel", "payload": {}},
            }
            response = await client.post(f"{self.url}/asap", json=request)
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.json()["result"]["payload"]["cancelled"], False)
            self.assertEqual(response.json()["id"], SECOND_ID)

            request["id"] = FIRST_ID
            request["params"] = {}
            response = await client.post(f"{self.url}/asap", json=request)
            self.assertEqual(response.status_code, 400)
            self.assertEqual(response.json()["error"]["code"], -32602)
            self.assertEqual(response.json()["id"], FIRST_ID)

    async def test_sender_rejection_keeps_http_status(self):
        envelope = echo({}).model_dump(mode="json")
        envelope["sender"] = "urn:asap:agent:untrusted"
        async with httpx.AsyncClient(trust_env=False) as client:
            response = await client.post(f"{self.url}/asap", json={
                "jsonrpc": "2.0", "method": "asap.send", "id": "sender-check",
                "params": {"envelope": envelope},
            })
        self.assertEqual(response.status_code, 400)
        self.assertEqual(response.json()["error"]["code"], -32002)
        self.assertEqual(response.json()["id"], "sender-check")

    async def test_decimal_tokens_in_echo_and_id(self):
        request = {
            "jsonrpc": "2.0", "method": "asap.send", "id": "RPC_ID",
            "params": {"envelope": echo({"decimal": "INPUT"}).model_dump(mode="json")},
        }
        rpc_id = "1.234567890123456789"
        decimal = "-1.234567890123456789e-123"
        content = json.dumps(request).replace('"RPC_ID"', rpc_id).replace('"INPUT"', decimal)
        async with httpx.AsyncClient(trust_env=False) as client:
            response = await client.post(
                f"{self.url}/asap", content=content, headers={"Content-Type": "application/json"},
            )
        self.assertEqual(response.status_code, 200)
        body = json.loads(response.content, parse_float=Decimal)
        self.assertEqual(body["id"], Decimal(rpc_id))
        self.assertEqual(body["result"]["envelope"]["payload"]["result"]["decimal"], Decimal(decimal))


if __name__ == "__main__":
    SDKChecks.url = sys.argv.pop(1)
    unittest.main(verbosity=2)
