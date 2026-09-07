# Shared market-data channel runtime

The reusable socket layer has stable ownership outside this disposable SSE
module:

```
sse/experimental/memory_pipeline/udp_runtime/UdpChannelRuntime.h
sse/experimental/memory_pipeline/udp_runtime/UdpChannelRuntime.cpp
```

`UdpChannelRuntime` owns one UDP receive worker per configured channel and
emits an exchange-neutral `Datagram`. It handles bind, `SO_REUSEADDR`,
multicast membership, receive timestamps, and source metadata. It deliberately
does not decode records or calculate factors.

The callback receives a zero-copy payload view that is valid only for the
duration of the callback. A decoder or recorder that needs longer ownership
must copy it explicitly.

The intended long-term split is:

```
UdpChannelRuntime -> SSE decoder -> SSE 50-factor engine -> native SSE model
                  -> SZE decoder -> SZE factor engine -> existing SZE model
```

SSE live batching is intentionally above this shared socket layer. Link the
`sse_live_sampling` target and follow `LIVE_SAMPLING.md` after the SSE decoder
has serialized a logical feed lane. Do not add the 100us policy to
`UdpChannelRuntime`: it would change SZE behavior and cannot reason about SSE
sequence recovery or CompleteOrderBookSH candidates.

The current SZE receivers are still production SZE-specific adapters. They
have not been changed in this offline phase. When the feed migration is
approved, their socket loops can be adapted to this runtime without sharing
exchange-specific wire parsing.
