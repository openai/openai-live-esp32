"""Credential-free loopback integration / adversarial tests against pinned aiortc.

Private aiortc hooks are used only to inject packets from this test's own endpoint.
"""

import argparse
import asyncio
import re
import struct
from pathlib import Path

import aioice.ice
from aiortc import (
    AudioStreamTrack,
    RTCConfiguration,
    RTCPeerConnection,
    RTCSessionDescription,
)
from aiortc.mediastreams import MediaStreamError

# Keep traffic on loopback even on a laptop with VPN / multiple interfaces.
aioice.ice.get_host_addresses = lambda **kwargs: ["127.0.0.1"]


async def session(
    proc, *, reject=False, server=False, attacks=False, corpus=None, media="both"
):
    use_audio, use_data = media != "data", media != "audio"
    pc = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    received = 0
    consumers = []
    replies = asyncio.Queue()
    opened = asyncio.Event()
    captured = {}
    native_pong = asyncio.Event()

    @pc.on("datachannel")
    def native_channel(channel):
        assert channel.label == "native"
        channel.on(
            "message", lambda data: native_pong.set() if data == "pong" else None
        )
        channel.send("ping")

    flight = bytearray([1 if server else 0])

    @pc.on("track")
    def on_track(track):
        async def consume():
            nonlocal received
            try:
                while True:
                    await track.recv()
                    received += 1
            except MediaStreamError:
                pass

        consumers.append(asyncio.create_task(consume()))

    try:
        line = await asyncio.wait_for(proc.stdout.readline(), 10)
        assert line.startswith(b"OFFER "), line
        offer = (await proc.stdout.readexactly(int(line.split()[1]))).decode()
        await pc.setRemoteDescription(RTCSessionDescription(offer, "offer"))
        sender = pc.addTrack(AudioStreamTrack()) if use_audio else None
        if use_data:
            dc = pc.createDataChannel("events", id=1 if server else 0)
            dc.on("open", opened.set)
            dc.on("message", replies.put_nowait)
        dtls = pc.sctp.transport if use_data else sender.transport
        if server:
            dtls._set_role("server")
        raw_send = dtls.transport._send

        async def capture(data):
            if corpus and 20 <= data[0] <= 63:
                flight.extend(struct.pack("!H", len(data)) + data)
                if len(flight) < 16384:
                    corpus.mkdir(parents=True, exist_ok=True)
                    (corpus / f"flight-{server}-{len(flight)}").write_bytes(flight)
            if len(data) >= 12 and data[0] >> 6 == 2:
                captured["rtcp" if 192 <= data[1] <= 223 else "rtp"] = data
            await raw_send(data)

        dtls.transport._send = capture
        await pc.setLocalDescription(await pc.createAnswer())
        answer = pc.localDescription.sdp
        if reject:
            answer = re.sub(
                r"(?m)^(a=fingerprint:sha-256 ).+$",
                r"\g<1>" + ":".join(["00"] * 32) + "\r",
                answer,
            )
        answer = answer.encode()
        proc.stdin.write(f"ANSWER {len(answer)}\n".encode() + answer)
        await proc.stdin.drain()
        if not reject and use_data:
            await asyncio.wait_for(opened.wait(), 5)
            for value in ("ping", "", "microphone 🎙", "a" * 65536):
                dc.send(value)
                reply = await asyncio.wait_for(replies.get(), 3)
                assert reply == ("pong" if value == "ping" else value), (
                    len(value),
                    len(reply),
                )
            await asyncio.wait_for(native_pong.wait(), 3)
            if attacks:
                # Authenticated SCTP delivery must still reject invalid UTF-8 / unknown PPIDs / oversize messages.
                await pc.sctp._send(dc.id, 51, b"\xc0\x80")
                await pc.sctp._send(dc.id, 999, b"unknown")
                await pc.sctp._send(dc.id, 51, b"z" * 65537)
                # Allow ordinary RTCP to arrive, then inject replay and invalid authentication tags.
                deadline = asyncio.get_running_loop().time() + 2
                while (
                    "rtcp" not in captured
                    and asyncio.get_running_loop().time() < deadline
                ):
                    await asyncio.sleep(0.02)
                assert {"rtp", "rtcp"} <= captured.keys(), captured.keys()
                for kind in ("rtp", "rtcp"):
                    packet = captured[kind]
                    await raw_send(packet)
                    await raw_send(packet[:-1] + bytes([packet[-1] ^ 1]))
                # Well authenticated, structurally invalid RTCP; correct sender SSRC, invalid report count.
                await dtls._send_rtp(struct.pack("!BBHI", 0x81, 201, 1, sender._ssrc))
                # Invalid SCTP packet delivered through valid DTLS must be discarded by usrsctp.
                await dtls._send_data(bytes(32))
                # One more good message proves the association survived malformed input.
                await asyncio.sleep(0.1)
                dc.send("ping")
                assert await asyncio.wait_for(replies.get(), 2) == "pong"
        line = await asyncio.wait_for(proc.stdout.readline(), 10)
        assert line.startswith(b"RESULT "), line
        stats = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line.decode())}
        if reject:
            assert (
                stats["dtls_failures"] == 1 and stats["audio"] == stats["messages"] == 0
            ), stats
            assert not opened.is_set()
        else:
            if use_audio:
                assert received >= 10 and stats["audio"] >= 10, (received, stats)
            else:
                assert received == stats["audio"] == 0, stats
            assert stats["messages"] == ((6 if attacks else 5) if use_data else 0), (
                stats
            )
            if attacks:
                assert stats["bad_rtp"] >= 2 and stats["bad_rtcp"] >= 3, stats
        print(
            f"PASS role={'server' if server else 'client'} reject={reject} attacks={attacks} decoded={received} {stats}",
            flush=True,
        )
    finally:
        await pc.close()
        await asyncio.gather(*consumers)


async def run(args):
    proc = await asyncio.create_subprocess_exec(
        args.executable,
        str(args.rounds),
        "reject" if args.reject else "accept",
        args.media,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
    )
    try:
        for _ in range(args.rounds):
            await session(
                proc,
                reject=args.reject,
                server=args.server,
                attacks=args.attacks,
                corpus=args.corpus,
                media=args.media,
            )
        assert await asyncio.wait_for(proc.wait(), 3) == 0
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--reject", action="store_true")
    parser.add_argument(
        "--server", action="store_true", help="aiortc acts as DTLS server"
    )
    parser.add_argument("--attacks", action="store_true")
    parser.add_argument("--media", choices=["both", "audio", "data"], default="both")
    parser.add_argument(
        "--corpus", type=Path, help="save local DTLS test flights as fuzz seeds"
    )
    args = parser.parse_args()
    if args.attacks and args.media != "both":
        parser.error("--attacks requires --media both")
    asyncio.run(run(args))
