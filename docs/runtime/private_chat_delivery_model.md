# Private Chat Delivery Model

This document describes the delivery state machine for outgoing private
messages, the expiry and timeout rules, and the rationale for the chosen
design (Option B — optimistic wire-ACK + optional end-to-end receipt).

---

## Delivery states

```
QUEUED(0)
    │
    ├─ route found ──────────────────────────────► SENT(2)
    │                                                  │
    └─ no route → AWAITING_ROUTE(1)                    │
            │                                          │
            ├─ route found ───────────────────────────►│
            │                                          │
            └─ expiry ─────────────────────────────► FAILED_NO_ROUTE(6)
                                                        ▲
QUEUED / AWAITING_ROUTE ──── queue expiry ──────────► EXPIRED(8)

SENT(2)
    │
    ├─ wire-level ACK from next hop ─────────────► FORWARDED(3)
    │                                                  │
    └─ retry budget exhausted ───────────────────► FAILED_RETRY(7)

FORWARDED(3)
    │
    ├─ delivery receipt received ────────────────► DELIVERED(4)  (terminal)
    │
    └─ receipt timeout (60 s) ───────────────────► DELIVERY_UNCONFIRMED(5)  (terminal)

SENT(2)
    └─ retry budget exhausted ───────────────────► FAILED_RETRY(7)  (terminal)
```

| State | Value | Terminal | Failure |
|-------|-------|----------|---------|
| `QUEUED`                | 0 | No  | No  |
| `AWAITING_ROUTE`        | 1 | No  | No  |
| `SENT`                  | 2 | No  | No  |
| `FORWARDED`             | 3 | No  | No  |
| `DELIVERED`             | 4 | Yes | No  |
| `DELIVERY_UNCONFIRMED`  | 5 | Yes | No  |
| `FAILED_NO_ROUTE`       | 6 | Yes | Yes |
| `FAILED_RETRY`          | 7 | Yes | Yes |
| `EXPIRED`               | 8 | Yes | Yes |

---

## Timing constants

| Constant                         | Value  | Meaning                                         |
|----------------------------------|--------|-------------------------------------------------|
| `PRIVATE_CHAT_QUEUE_EXPIRY_MS`   | 30 000 | Max time in queue before EXPIRED/FAILED_NO_ROUTE|
| `PRIVATE_CHAT_RECEIPT_TMO_MS`    | 60 000 | Max FORWARDED→DELIVERED wait before UNCONFIRMED |
| `PRIVATE_CHAT_QUEUE_SIZE`        | 8      | Max simultaneous outgoing messages              |
| `PRIVATE_CHAT_RATE_INTERVAL_MS`  | 5 000  | Min interval between sends to the same peer     |
| `PRIVATE_CHAT_RECEIPT_RATE_MAX`  | 4      | Max receipts generated per rate window          |
| `PRIVATE_CHAT_RECEIPT_RATE_WIN_MS`| 10 000| Receipt rate-limit window                       |

---

## Design rationale — Option B

Early design considered three approaches:

**Option A — Flood with deliver-once suppression.**
Reuses the existing flood engine; no routing required.  Rejected because all
nodes on the mesh would receive the payload, which is not acceptable for
"private" messaging even without encryption.

**Option B — Unicast with wire-ACK + optional E2E receipt.**  *(chosen)*
Uses the existing route-cache and retry-table infrastructure.  Wire-ACK
(`FORWARDED` state) confirms next-hop delivery without waiting for the far
end.  An optional delivery receipt (`DELIVERED`) provides confirmation the
application layer received the message.  `DELIVERY_UNCONFIRMED` covers the
common case where the receipt is lost in transit — the sender knows the frame
reached the next hop but cannot confirm full delivery.  This avoids blocking
the UX on every packet.

**Option C — Store-and-forward with persistent relay queue.**
More reliable on very lossy links.  Deferred to a future release;
`PCHAT_FLAG_STORE_FWD_OK` is reserved in the flag byte for when this is
implemented.

---

## Tick-driven state machine

`private_chat_tick(uint32_t now_ms)` is called from the main loop.  Per tick:

1. Each entry in the outgoing queue is checked for expiry (`now_ms >=
   entry.enqueued_ms + PRIVATE_CHAT_QUEUE_EXPIRY_MS`).
2. Entries in `FORWARDED` state are checked for receipt timeout (`now_ms >=
   entry.forwarded_at_ms + PRIVATE_CHAT_RECEIPT_TMO_MS`).
3. On state change the companion app is notified via
   `rivr_ble_companion_push_pchat_state()` (opcode 0x89).

---

## Wire-ACK vs route-cache path

When `private_chat_send()` is called:

1. `route_cache_best_hop()` is consulted for the destination node ID.
2. **Route hit:** an `rf_tx_request_t` is pushed to `rf_tx_queue` with
   `due_ms = now_ms`.  The retry engine (`retry_table`) tracks retransmits
   and calls back with wire-ACK success/failure.
3. **Route miss:** a `ROUTE_REQUEST` is emitted (via `routing_request_route()`)
   and the entry sits as `AWAITING_ROUTE` until a `ROUTE_REPLY` populates
   the route cache, after which `private_chat_tick()` promotes the entry to
   `SENT`.

---

## Memory layout

All state is in BSS.  No heap allocation occurs in any hot path.

```c
static pchat_entry_t g_pchat_queue[PRIVATE_CHAT_QUEUE_SIZE];
static pchat_dedup_entry_t g_pchat_dedup[PRIVATE_CHAT_DEDUP_SIZE];
static uint32_t g_rcpt_rate_bucket;
static uint32_t g_rcpt_rate_window_start_ms;
```

`pchat_entry_t` contains the full encoded payload so retransmits do not
re-encode.  The encoded frame is stored at enqueue time.

---

## Error codes

| Constant                   | Value | Meaning                           |
|----------------------------|-------|-----------------------------------|
| `PCHAT_OK`                 |  0    | Success                           |
| `PCHAT_ERR_QUEUE_FULL`     | -1    | Outgoing queue is full            |
| `PCHAT_ERR_INVALID_PEER`   | -2    | dst_id is 0 or broadcast          |
| `PCHAT_ERR_BODY_TOO_LONG`  | -3    | Body exceeds `PRIVATE_CHAT_MAX_BODY` |
| `PCHAT_ERR_DUPLICATE`      | -4    | Received msg already in dedup cache|
| `PCHAT_ERR_DST_MISMATCH`   | -5    | Received frame has wrong dst_id   |
| `PCHAT_ERR_INVALID_PAYLOAD`| -6    | Payload too short or body_len bad |
