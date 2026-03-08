import 'package:equatable/equatable.dart';

/// Parsed snapshot of a single @MET JSON line emitted by the Rivr firmware.
class RivrMetrics extends Equatable {
  // --- Identity ---
  final int nodeId;

  // --- Radio health ---
  final int dcPct;         // duty-cycle used %
  final int qDepth;        // TX queue depth
  final int txTotal;       // lifetime TX frames
  final int rxTotal;       // lifetime RX frames
  final int routeCache;    // live route-cache entries

  // --- Link quality ---
  final int lnkCnt;        // live neighbor count
  final int lnkBest;       // best link score 0-100
  final int lnkRssi;       // EWMA RSSI of best neighbor (dBm)
  final int lnkLoss;       // avg loss % across live neighbors

  // --- Relay ---
  final int relaySkip;     // phase-4+5 suppressed relays (total)
  final int relayDelay;    // lifetime extra holdoff applied (ms)
  final int relayDensity;  // viable neighbor count (density tier input)
  final int relayFwd;      // relay frames that completed TX
  final int relaySel;      // relay candidates selected
  final int relayCan;      // relay frames cancelled (phase-4)

  // --- Packet errors ---
  final int rxDecodeFail;
  final int rxDedupeDrop;
  final int txQueueFull;
  final int dutyBlocked;
  final int radioHardReset;

  final DateTime collectedAt;

  const RivrMetrics({
    required this.nodeId,
    required this.dcPct,
    required this.qDepth,
    required this.txTotal,
    required this.rxTotal,
    required this.routeCache,
    required this.lnkCnt,
    required this.lnkBest,
    required this.lnkRssi,
    required this.lnkLoss,
    required this.relaySkip,
    required this.relayDelay,
    required this.relayDensity,
    required this.relayFwd,
    required this.relaySel,
    required this.relayCan,
    required this.rxDecodeFail,
    required this.rxDedupeDrop,
    required this.txQueueFull,
    required this.dutyBlocked,
    required this.radioHardReset,
    required this.collectedAt,
  });

  factory RivrMetrics.empty() => RivrMetrics(
    nodeId: 0, dcPct: 0, qDepth: 0, txTotal: 0, rxTotal: 0,
    routeCache: 0, lnkCnt: 0, lnkBest: 0, lnkRssi: -120, lnkLoss: 0,
    relaySkip: 0, relayDelay: 0, relayDensity: 0,
    relayFwd: 0, relaySel: 0, relayCan: 0,
    rxDecodeFail: 0, rxDedupeDrop: 0, txQueueFull: 0,
    dutyBlocked: 0, radioHardReset: 0,
    collectedAt: DateTime.fromMillisecondsSinceEpoch(0),
  );

  @override
  List<Object?> get props => [nodeId, dcPct, txTotal, rxTotal, collectedAt];
}
