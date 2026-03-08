import 'dart:async';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../services/connection_manager.dart';
import '../protocol/rivr_protocol.dart';
import '../models/chat_message.dart';
import '../models/rivr_node.dart';
import '../models/metrics.dart';

// ── Singleton connection manager ───────────────────────────────────────────

final connectionManagerProvider = Provider<ConnectionManager>((ref) {
  final manager = ConnectionManager();
  ref.onDispose(manager.dispose);
  return manager;
});

// ── Reactive connection state ─────────────────────────────────────────────

final connectionStateProvider = StreamProvider<RivrConnState>((ref) {
  return ref.watch(connectionManagerProvider).stateStream;
});

// ── Event dispatcher — feeds all downstream providers ────────────────────

/// Central event stream.  All model providers listen to this.
final eventStreamProvider = StreamProvider<RivrEvent>((ref) {
  return ref.watch(connectionManagerProvider).eventStream;
});

// ── Chat messages ──────────────────────────────────────────────────────────

class ChatNotifier extends Notifier<List<ChatMessage>> {
  static const _maxMessages = 500;

  @override
  List<ChatMessage> build() {
    ref.listen(eventStreamProvider, (_, next) {
      next.whenData((event) {
        if (event is ChatEvent) {
          // Enrich sender name with callsign from node table if available
          final msg = event.message;
          final nodes = ref.read(nodesProvider);
          final node = nodes[msg.senderNodeId];
          final enriched = (node != null && node.callsign.isNotEmpty)
              ? ChatMessage(
                  id: msg.id,
                  text: msg.text,
                  senderNodeId: msg.senderNodeId,
                  senderName: '${node.callsign} (${msg.senderName})',
                  timestamp: msg.timestamp,
                  origin: msg.origin,
                )
              : msg;
          _add(enriched);
        }
      });
    });
    return [];
  }

  void _add(ChatMessage msg) {
    final next = [...state, msg];
    state = next.length > _maxMessages
        ? next.sublist(next.length - _maxMessages)
        : next;
  }

  void addSystem(String text) => _add(ChatMessage.system(text));

  void addLocal(ChatMessage msg) => _add(msg);
}

final chatProvider =
    NotifierProvider<ChatNotifier, List<ChatMessage>>(ChatNotifier.new);

// ── Nodes ──────────────────────────────────────────────────────────────────

class NodesNotifier extends Notifier<Map<int, RivrNode>> {
  @override
  Map<int, RivrNode> build() {
    ref.listen(eventStreamProvider, (_, next) {
      next.whenData((event) {
        if (event is NodeEvent) {
          state = {...state, event.node.nodeId: event.node};
        }
      });
    });
    return {};
  }

  List<RivrNode> get sorted => state.values.toList()
    ..sort((a, b) => b.linkScore.compareTo(a.linkScore));
}

final nodesProvider =
    NotifierProvider<NodesNotifier, Map<int, RivrNode>>(NodesNotifier.new);

// ── Metrics ────────────────────────────────────────────────────────────────

class MetricsNotifier extends Notifier<List<RivrMetrics>> {
  static const _maxHistory = 60;  // keep last 60 snapshots (≈5 min at 5 s)

  @override
  List<RivrMetrics> build() {
    ref.listen(eventStreamProvider, (_, next) {
      next.whenData((event) {
        if (event is MetricsEvent) {
          final next = [...state, event.metrics];
          state = next.length > _maxHistory
              ? next.sublist(next.length - _maxHistory)
              : next;
        }
      });
    });
    return [];
  }

  RivrMetrics get latest => state.isNotEmpty ? state.last : RivrMetrics.empty();
}

final metricsProvider =
    NotifierProvider<MetricsNotifier, List<RivrMetrics>>(MetricsNotifier.new);

/// Convenience: just the most recent snapshot.
final latestMetricsProvider = Provider<RivrMetrics>((ref) {
  return ref.watch(metricsProvider.notifier).latest;
});

// ── Raw log lines ─────────────────────────────────────────────────────────

class LogNotifier extends Notifier<List<String>> {
  static const _maxLines = 200;

  @override
  List<String> build() {
    ref.listen(eventStreamProvider, (_, next) {
      next.whenData((event) {
        if (event is RawLineEvent) {
          final l = [...state, event.line];
          state = l.length > _maxLines ? l.sublist(l.length - _maxLines) : l;
        }
      });
    });
    return [];
  }
}

final logProvider = NotifierProvider<LogNotifier, List<String>>(LogNotifier.new);
