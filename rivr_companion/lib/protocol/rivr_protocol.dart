import 'dart:convert';
import '../models/metrics.dart';
import '../models/chat_message.dart';
import '../models/rivr_node.dart';

/// Families of structured events emitted by the Rivr firmware serial log.
sealed class RivrEvent {}

class MetricsEvent extends RivrEvent {
  final RivrMetrics metrics;
  MetricsEvent(this.metrics);
}

class ChatEvent extends RivrEvent {
  final ChatMessage message;
  ChatEvent(this.message);
}

class NodeEvent extends RivrEvent {
  final RivrNode node;
  NodeEvent(this.node);
}

class RawLineEvent extends RivrEvent {
  final String line;
  RawLineEvent(this.line);
}

/// Parses raw text lines from the Rivr firmware serial output and emits typed
/// [RivrEvent] objects.
///
/// The parser is stateless — call [parseLine] for each line received.
class RivrProtocol {
  // ── @MET JSON ──────────────────────────────────────────────────────────────
  // Example:  @MET {"node":3735928559,"dc":12,"qdep":0,...}
  static final _metPattern = RegExp(r'^@MET\s+(\{.+\})\s*$');

  // ── PKT_CHAT line ──────────────────────────────────────────────────────────
  // Example:  I CHAT: rx from 0x1A2B3C4D [ALICE]: hello world
  static final _chatRxPattern = RegExp(
      r'CHAT.*?rx from (0x[0-9A-Fa-f]+)(?:\s+\[([^\]]+)\])?\s*:\s*(.+)',
      caseSensitive: false);

  // ── Beacon / node info in ntable output ───────────────────────────────────
  // Example:  0x1A2B3C4D  ALICE    1  -87  +8  72  15s  123
  static final _ntableRowPattern = RegExp(
      r'(0x[0-9A-Fa-f]{8})\s+(\S*)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)',
      caseSensitive: false);

  /// Parse one line of firmware output and return an event, or null if the
  /// line carries no structured information of interest.
  static RivrEvent? parseLine(String line) {
    line = line.trim();
    if (line.isEmpty) return null;

    // @MET JSON
    final metMatch = _metPattern.firstMatch(line);
    if (metMatch != null) {
      final metrics = _parseMetrics(metMatch.group(1)!);
      if (metrics != null) return MetricsEvent(metrics);
    }

    // Chat RX
    final chatMatch = _chatRxPattern.firstMatch(line);
    if (chatMatch != null) {
      final nodeIdStr = chatMatch.group(1)!;
      final callsign = chatMatch.group(2) ?? '';
      final text = chatMatch.group(3)!.trim();
      final nodeId = int.tryParse(nodeIdStr) ?? 0;
      final msg = ChatMessage(
        id: '${DateTime.now().microsecondsSinceEpoch}',
        text: text,
        senderNodeId: nodeId,
        senderName: callsign.isNotEmpty ? callsign : nodeIdStr,
        timestamp: DateTime.now(),
        origin: MessageOrigin.remote,
      );
      return ChatEvent(msg);
    }

    // ntable row → node update
    final nbMatch = _ntableRowPattern.firstMatch(line);
    if (nbMatch != null) {
      final nodeId = int.parse(nbMatch.group(1)!, radix: 16);
      final callsign = nbMatch.group(2) ?? '';
      final hops = int.tryParse(nbMatch.group(3)!) ?? 0;
      final rssi = int.tryParse(nbMatch.group(4)!) ?? -120;
      final snr = int.tryParse(nbMatch.group(5)!) ?? 0;
      final score = int.tryParse(nbMatch.group(6)!) ?? 0;
      final node = RivrNode(
        nodeId: nodeId,
        callsign: callsign == '-' ? '' : callsign,
        rssiDbm: rssi,
        snrDb: snr,
        hopCount: hops,
        linkScore: score,
        lossPercent: 0,
        lastSeen: DateTime.now(),
      );
      return NodeEvent(node);
    }

    return RawLineEvent(line);
  }

  // ── Parse @MET fields ─────────────────────────────────────────────────────
  static RivrMetrics? _parseMetrics(String json) {
    try {
      final m = jsonDecode(json) as Map<String, dynamic>;
      return RivrMetrics(
        nodeId: _i(m, 'node'),
        dcPct: _i(m, 'dc'),
        qDepth: _i(m, 'qdep'),
        txTotal: _i(m, 'tx'),
        rxTotal: _i(m, 'rx'),
        routeCache: _i(m, 'rc'),
        lnkCnt: _i(m, 'lnk_cnt'),
        lnkBest: _i(m, 'lnk_best'),
        lnkRssi: _i(m, 'lnk_rssi'),
        lnkLoss: _i(m, 'lnk_loss'),
        relaySkip: _i(m, 'relay_skip'),
        relayDelay: _i(m, 'relay_delay'),
        relayDensity: _i(m, 'relay_density'),
        relayFwd: _i(m, 'relay_fwd'),
        relaySel: _i(m, 'relay_sel'),
        relayCan: _i(m, 'relay_can'),
        rxDecodeFail: _i(m, 'rx_fail'),
        rxDedupeDrop: _i(m, 'dedupe'),
        txQueueFull: _i(m, 'txq_full'),
        dutyBlocked: _i(m, 'dc_block'),
        radioHardReset: _i(m, 'radio_rst'),
        collectedAt: DateTime.now(),
      );
    } catch (_) {
      return null;
    }
  }

  static int _i(Map<String, dynamic> m, String key) {
    final v = m[key];
    if (v == null) return 0;
    if (v is int) return v;
    if (v is double) return v.toInt();
    return int.tryParse(v.toString()) ?? 0;
  }

  /// Build the serial command to send a chat message.
  static String buildChatCommand(String text) => 'chat $text\n';

  /// Request the node table printout.
  static const String cmdNtable = 'ntable\n';

  /// Request a metrics snapshot.
  static const String cmdMetrics = 'metrics\n';

  /// Request the forward-candidate set status.
  static const String cmdFwdset = 'fwdset\n';

  /// Request a routing stats snapshot.
  static const String cmdRtstats = 'rtstats\n';
}
