# BeamMP Launcher VOIP Protocol

This document defines the launcher-side VOIP protocol additions that the BeamMP server must relay for in-session voice chat.

## Packet transport

- `VOIP:FRAME:*` packets: UDP (low latency)
- `VOIP:HELLO:*`, `VOIP:STATE:*`, `VOIP:BYE:*`: TCP (reliable control)
- `VOIP_POS:*` packets: UDP (spatial updates for attenuation)

## Voice envelope format

`VOIP:<TYPE>:<senderId>:<sequence>:<timestampMs>:<voiceActive>:<payload>`

- `<TYPE>`: `HELLO`, `STATE`, `FRAME`, `BYE`
- `<senderId>`: BeamMP client ID
- `<sequence>`: monotonic frame/control sequence per sender
- `<timestampMs>`: sender-side packet timestamp
- `<voiceActive>`: `1` or `0`
- `<payload>`: encoded voice frame bytes for `FRAME`, empty for control packets

## Spatial packet format

`VOIP_POS:<playerId>:<x>:<y>:<z>:<vx>:<vy>:<vz>`

- Position/velocity should be in the same coordinate space used by vehicle state updates.
- Server may proximity-filter relays for bandwidth savings, but launcher performs final attenuation.

## Server relay requirements

- Relay VOIP packets only to clients in the same multiplayer session.
- Preserve sender IDs and sequence/timestamp values.
- Do not mutate voice payload bytes.
- Keep relaying latency as low as possible, especially for `FRAME` packets.
