# Private Chat Compatibility Matrix

This document describes how `PKT_PRIVATE_CHAT` (type 12) and
`PKT_DELIVERY_RECEIPT` (type 13) behave in mixed-version fleets — i.e. when
some nodes run firmware that does not implement private chat.

---

## Version naming

| Version label | Meaning                                              |
|---------------|------------------------------------------------------|
| **v-new**     | Firmware with private chat (implements type 12/13)   |
| **v-old**     | Firmware without private chat (does not know type 12/13) |

---

## Peer-to-peer path matrix

| Sender  | Relay   | Destination | Expected outcome                                     |
|---------|---------|-------------|------------------------------------------------------|
| v-new   | v-new   | v-new       | Full delivery; receipt generated; `DELIVERED` state. |
| v-new   | v-old   | v-new       | Old relay increments `rx_invalid_type` counter and **drops** the frame. Retry budget exhausts → `FAILED_RETRY`. |
| v-new   | v-new   | v-old       | Frame reaches destination. Destination calls `protocol_decode()` which passes type 12 through to the app layer; old app calls nothing → frame silently dropped. Sender's receipt-timeout expires → `DELIVERY_UNCONFIRMED`. |
| v-new   | —       | v-old       | Same as above (direct link, no relay).               |
| v-old   | any     | any         | Source cannot originate private chat; the feature is UI-blocked by the companion app's firmware version check. |

---

## Old relay behaviour

An old relay node checks incoming packet types against the known range
(`PKT_TYPE <= PKT_METRICS`).  Packet type 12 exceeds this range.  The relay
increments its `rx_invalid_type` diagnostic counter and calls
`RIVR_FWD_DROP_INVALID` — the frame is **not forwarded**.

This is the **intended degradation path**: the private chat sender eventually
exhausts its retry budget and reports `FAILED_RETRY`.  The user sees the
error in the companion app and knows delivery failed; there is no silent
black-hole ambiguity.

### `PCHAT_FLAG_STORE_FWD_OK`

Setting this flag tells a (future) store-and-forward relay that it may buffer
the frame.  For mixed-version fleets this flag must be **cleared** (default)
so that an old relay that knows only flood-or-drop semantics does not attempt
any special handling.

---

## Old destination behaviour

An old destination node receives the frame (if all relays are v-new) and calls
`protocol_decode()`.  The decoded `pkt_type = 12` is passed to the application
dispatch table.  Because old firmware has no handler registered for type 12,
the packet is dropped after decode without calling any callback.  No receipt is
generated.

The sender transitions `FORWARDED → DELIVERY_UNCONFIRMED` after the 60 s
receipt timeout (`PRIVATE_CHAT_RECEIPT_TMO_MS`).  The companion app displays
the `deliveryUnconfirmed` state with a single-tick icon, distinguishing this
from a clean `DELIVERED`.

---

## Companion app version check

The companion app must only enable the private chat UI if:

1. The connected node is v-new (private chat support detected from the
   `@MET` feature flags or firmware version string — exact check TBD post
   feature-flag implementation).
2. Until a feature-flag mechanism is in place, the UI is always available and
   the user receives a `FAILED_RETRY` or `DELIVERY_UNCONFIRMED` response from
   the firmware if the state machine cannot complete.

---

## Rollout recommendation

For reliable private chat in a mesh:

1. Upgrade all relay nodes to v-new first.
2. Then upgrade destination nodes.
3. Finally upgrade source nodes (companion app pairing).

This ordering ensures that relay infrastructure is always v-new before
end-to-end paths are exercised.

---

## Summary

| Scenario                        | Delivery state at sender |
|---------------------------------|--------------------------|
| All nodes v-new                 | `DELIVERED`              |
| Old relay in path               | `FAILED_RETRY`           |
| Old destination, v-new relays   | `DELIVERY_UNCONFIRMED`   |
| Source is v-old                 | Feature unavailable       |
