# Private Chat — Companion App UX

This document describes the private chat screens in the Rivr Companion app,
the BLE/serial control-plane opcodes they use, and the UX rules governing
message display and delivery state feedback.

---

## Screen overview

```
RivrShell (bottom nav)
    │
    │  (no tab — navigated to via push)
    ▼
ConversationListScreen    ← entry point, accessible from future nav addition
    │
    ├─ FAB (+) ──► NewPrivateChatScreen
    │                   │
    │                   └─ tap node ──► PrivateChatScreen (pushReplacement)
    │
    └─ tap conversation ──► PrivateChatScreen
```

---

## ConversationListScreen

**File:** `lib/screens/conversation_list_screen.dart`

- Lists conversations sorted by `lastActivityAt` descending (provider
  already sorts).
- Each row shows:
  - Avatar with first letter of peer display name.
  - Peer display name (callsign if known, else `0xDEADBEEF`).
  - Last message preview, truncated to one line.
  - Relative time (today → `HH:mm`, older → `MMM d`).
  - Unread badge (red pill with count, hidden when zero).
  - Delivery status icon for last outgoing message (see §Delivery chips).
- Empty state: lock icon + "No private chats yet. Tap + to start one."
- FAB: navigates to `NewPrivateChatScreen`.
- Tap row: calls `privateChatProvider.notifier.markRead(id)` then pushes
  `PrivateChatScreen`.

---

## PrivateChatScreen

**File:** `lib/screens/private_chat_screen.dart`

- Constructor parameters: `conversationId`, `peerNodeId`, `peerDisplayName`.
- `AppBar` title: peer display name (large) + node ID in hex (small subtitle).
- `markRead` is called via `addPostFrameCallback` on `initState` so the
  unread badge clears as soon as the screen is visible.
- Message list auto-scrolls to bottom when new messages arrive.

### Message bubbles

- Outgoing messages (sent by this device): right-aligned, `primaryContainer`
  background.
- Incoming messages: left-aligned, `secondaryContainer` background.
- Each bubble shows: body text, `HH:mm` time, delivery status icon (outgoing
  only — see §Delivery chips).
- Maximum bubble width: 75 % of screen width.

### Input bar

- `TextField` with `maxLength = 200`, `MaxLengthEnforcement.enforced`.
- Character counter appears only when `length >= 180` (last 20 chars).
- Supports multi-line input (max 4 lines visible, scrollable).
- `TextInputAction.send` triggers `_send()` on keyboard submit.
- Send button disabled while a send is in-flight to prevent double-tap.
- Empty or whitespace-only input is rejected silently.

### Sending a message

1. `PrivateChatNotifier.sendMessage(peerNodeId, body)` is called.
2. An optimistic `PrivateMessage` with `status = queued` is inserted
   immediately — the outgoing bubble appears before BLE round-trip.
3. `RivrCompanionCodec.buildSendPrivate(dstNodeId, body)` builds the CP
   frame and `ConnectionManager.sendBinary()` writes it to the BLE NUS RX
   characteristic.
4. Firmware responds with `RIVR_CP_PKT_OK` + `[cmd:1][msg_id:8 LE]`.
   The `PrivateChatStateEvent` updates the optimistic bubble's state once
   the firmware assigns an `msg_id`.

---

## NewPrivateChatScreen

**File:** `lib/screens/new_private_chat_screen.dart`

- Lists all nodes from `nodesProvider`, sorted by `linkScore` descending,
  excluding the phone's own node ID.
- Each row: avatar, display name, node ID hex, hop-count chip (hidden for
  direct neighbors).
- Empty state: hub icon + "No nodes discovered yet. Broadcast a message to
  populate the node table."
- Tap: deriving the canonical `conversationId` via `Conversation.idFor(myId,
  peerNodeId)`, then `Navigator.pushReplacement` to `PrivateChatScreen`.

---

## Delivery chips

Both `ConversationListScreen` and `PrivateChatScreen` display a compact icon
indicating the delivery state of the most recent outgoing message.

| Status                | Icon                  | Color              |
|-----------------------|-----------------------|--------------------|
| `queued`              | `schedule`            | `onSurfaceVariant` |
| `awaitingRoute`       | `route`               | `onSurfaceVariant` |
| `sent`                | `check`               | `onSurfaceVariant` |
| `forwarded`           | `sync`                | `primary`          |
| `delivered`           | `done_all`            | `secondary`        |
| `deliveryUnconfirmed` | `done`                | `tertiary`         |
| `failedNoRoute`       | `error_outline`       | `error`            |
| `failedRetry`         | `error_outline`       | `error`            |
| `expired`             | `error_outline`       | `error`            |

In `PrivateChatScreen` these icons also carry a `Tooltip` with the human-readable
`PrivateMessageStatus.label` string for accessibility.

---

## CP protocol opcodes

### RIVR_CP_CMD_SEND_PRIVATE (0x07) — companion → firmware

Sends a private message to a peer node.

```
Header: [0x52 0x43 0x01 0x07 len]
Payload: [dst_id:4 LE][body:N bytes UTF-8]
```

Response on success: `RIVR_CP_PKT_OK` (0x81) with payload `[cmd:1=0x07][msg_id:8 LE]`.

### RIVR_CP_PKT_PRIVATE_CHAT_RX (0x88) — firmware → companion

Firmware pushes an incoming private message to the companion.

```
Header: [0x52 0x43 0x01 0x88 len]
Payload (27 + N bytes):
  [msg_id:8 LE][from_id:4 LE][to_id:4 LE][sender_seq:4 LE]
  [timestamp_s:4 LE][flags:2 LE][body_len:1][body:N]
```

### RIVR_CP_PKT_PRIVATE_CHAT_STATE (0x89) — firmware → companion

Firmware reports a state change for an outgoing message.

```
Header: [0x52 0x43 0x01 0x89 0x0D]
Payload (13 bytes): [msg_id:8 LE][peer_id:4 LE][state:1]
```

`state` maps to `pchat_delivery_state_t` (0–8).

### RIVR_CP_PKT_DELIVERY_RECEIPT (0x8A) — firmware → companion

Firmware pushes a delivery receipt received from the peer.

```
Header: [0x52 0x43 0x01 0x8A 0x11]
Payload (17 bytes): [orig_msg_id:8 LE][sender_id:4 LE][timestamp_s:4 LE][status:1]
```

---

## Persistence

Messages are persisted to `shared_preferences` via `PrivateChatStorage`:

- Key `"pchat_messages"` — JSON array of up to 1 000 messages (oldest pruned).
- Key `"pchat_conversations"` — JSON array of all conversation summaries.

On cold start the provider loads persisted state synchronously in `build()`
before subscribing to the event stream.
