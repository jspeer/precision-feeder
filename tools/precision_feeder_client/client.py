"""FeederClient: talks to one feeder over BLE (needs bleak: pip install bleak).

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

from __future__ import annotations

import asyncio
import secrets

from .protocol import *  # noqa: F401,F403
from .protocol import (
    AUTH_DETAILS, AUTH_UUID, CALIBRATION_UUID, CHANNEL_COMMAND, COMMAND_UUID, CONFIG_UUID, EVENT_LOG_UUID, INFO_UUID,
    OP_AUTH, PROFILE_UUID, RECORD_CHANNELS, RECORD_OPCODES, RESPONSE_UUID, STATUS_UUID, Info, Response, Status,
    auth_proof, calibration_query, command, config_query, next_seq, parse_calibration, parse_config,
    parse_event_log, parse_info, parse_profile_event, parse_profile_header, parse_response, parse_status,
    profile_query, session_key, signed,
)


class FeederClient:
    """Talks to one feeder. Commands carry a sequence number and are retried with the SAME number
    if no answer arrives, which is safe: the device replays its earlier answer instead of acting twice."""

    def __init__(self, address: str, key: bytes, retries: int = 2, timeout: float = 5.0):
        from bleak import BleakClient  # imported here so the pure functions work without bleak

        self._client = BleakClient(address)
        self._key = key
        self._session = b""
        self._retries = retries
        self._timeout = timeout
        self._seq = next_seq(secrets.randbelow(255))  # every write uses the next one; the session starts at AUTH
        # Answers are matched on (seq, opcode): seq alone is not unique (record writes all answer with seq 0).
        self._waiting: dict[tuple[int, int], asyncio.Future] = {}
        self.events: list[EventLog] = []

    async def __aenter__(self) -> "FeederClient":
        await self._client.connect()
        await self._client.start_notify(RESPONSE_UUID, self._on_response)
        await self._client.start_notify(EVENT_LOG_UUID, lambda _c, d: self.events.append(parse_event_log(bytes(d))))
        await self.authenticate()
        return self

    async def authenticate(self) -> None:
        """Read the challenge and prove the key. Every write after this is signed."""
        nonce = bytes(await self._client.read_gatt_char(AUTH_UUID))
        resp = await self._send(OP_AUTH, command(self._next(), OP_AUTH, auth_proof(self._key, nonce)))
        if not resp.ok:
            raise PermissionError(f"BLE authentication refused: {AUTH_DETAILS.get(resp.detail, resp.detail)}")
        self._session = session_key(self._key, nonce)

    async def __aexit__(self, *exc) -> None:
        await self._client.disconnect()

    def _on_response(self, _char, data: bytearray) -> None:
        resp = parse_response(bytes(data))
        fut = self._waiting.get((resp.seq, resp.opcode))
        if fut is not None and not fut.done():
            fut.set_result(resp)

    def _next(self) -> int:
        self._seq = next_seq(self._seq)
        return self._seq

    async def _send(self, opcode: int, data: bytes, uuid: str = COMMAND_UUID) -> Response:
        """Write `data` (sequence number first) and wait for the answer with the same (seq, opcode). A lost
        answer is retried with the SAME bytes: the device replays its answer instead of acting twice."""
        key = (data[0], opcode)
        loop = asyncio.get_running_loop()
        for attempt in range(self._retries + 1):
            fut = loop.create_future()
            self._waiting[key] = fut
            await self._client.write_gatt_char(uuid, data, response=True)
            try:
                return await asyncio.wait_for(fut, self._timeout)
            except asyncio.TimeoutError:
                continue
            finally:
                self._waiting.pop(key, None)
        raise TimeoutError(f"no answer to opcode 0x{opcode:02X} after {self._retries + 1} tries")

    async def command(self, opcode: int, args: bytes = b"") -> Response:
        body = command(self._next(), opcode, args)
        return await self._send(opcode, signed(self._session, CHANNEL_COMMAND, body))

    async def info(self) -> Info:
        return parse_info(bytes(await self._client.read_gatt_char(INFO_UUID)))

    async def status(self) -> Status:
        return parse_status(bytes(await self._client.read_gatt_char(STATUS_UUID)))

    async def write_record(self, uuid: str, data: bytes) -> Response:
        """Write a Profile/Config/Calibration record (or query) and wait for the device's Response."""
        body = bytes([self._next()]) + data
        return await self._send(RECORD_OPCODES[uuid], signed(self._session, RECORD_CHANNELS[uuid], body), uuid)

    async def read_profile_header(self, profile: int) -> dict:
        await self.write_record(PROFILE_UUID, profile_query(profile, None))
        return parse_profile_header(bytes(await self._client.read_gatt_char(PROFILE_UUID)))

    async def read_profile_event(self, profile: int, event: int) -> dict:
        await self.write_record(PROFILE_UUID, profile_query(profile, event))
        return parse_profile_event(bytes(await self._client.read_gatt_char(PROFILE_UUID)))

    async def read_config(self, key: int) -> int:
        await self.write_record(CONFIG_UUID, config_query(key))
        return parse_config(bytes(await self._client.read_gatt_char(CONFIG_UUID)))[1]

    async def read_calibration(self, index: int) -> int:
        """A position's offset, tenths of a degree."""
        await self.write_record(CALIBRATION_UUID, calibration_query(index))
        return parse_calibration(bytes(await self._client.read_gatt_char(CALIBRATION_UUID)))[1]

