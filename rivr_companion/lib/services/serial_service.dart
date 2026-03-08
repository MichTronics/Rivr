import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:usb_serial/usb_serial.dart';

import '../protocol/rivr_protocol.dart';
import 'connection_manager.dart';

/// USB Serial transport (CDC-ACM / CH340 / CP210x / FTDI).
///
/// On Android, USB host permission dialog is shown automatically by the
/// usb_serial package.  On Linux and Windows, no special setup is required
/// for most CDC-ACM devices.
///
/// Baud rate defaults to 115200, matching firmware_core/main.c UART config.
class SerialService extends RivrTransport {
  final int baudRate;
  SerialService({this.baudRate = 115200});

  final _stateCtrl = StreamController<RivrConnState>.broadcast();
  final _eventCtrl = StreamController<RivrEvent>.broadcast();

  UsbPort? _port;
  StreamSubscription<Uint8List>? _rxSub;
  final _lineBuffer = StringBuffer();

  @override
  Stream<RivrConnState> get stateStream => _stateCtrl.stream;

  @override
  Stream<RivrEvent> get eventStream => _eventCtrl.stream;

  // ── Scan: enumerate connected USB devices ─────────────────────────────────
  @override
  Future<void> startScan() async {
    _emit(ConnectionStatus.scanning, 'USB scan');
    final devices = await UsbSerial.listDevices();
    for (final d in devices) {
      _eventCtrl.add(RawLineEvent('USB_SCAN:${d.deviceId}:${d.manufacturerName ?? ''}:${d.productName ?? ''}'));
    }
    _emit(ConnectionStatus.disconnected, '');
  }

  // ── Connect by device ID ──────────────────────────────────────────────────
  @override
  Future<void> connect(String deviceId) async {
    _emit(ConnectionStatus.connecting, deviceId);
    try {
      final devices = await UsbSerial.listDevices();
      final device = devices.firstWhere(
        (d) => d.deviceId.toString() == deviceId,
        orElse: () => throw Exception('USB device $deviceId not found'),
      );

      final port = await device.create();
      if (port == null) throw Exception('Failed to open USB port');
      _port = port;

      final ok = await port.open();
      if (!ok) throw Exception('Failed to open port (permission denied?)');

      await port.setPortParameters(
        baudRate, UsbPort.DATABITS_8, UsbPort.STOPBITS_1, UsbPort.PARITY_NONE);

      _rxSub = port.inputStream?.listen(_onBytes);
      _emit(ConnectionStatus.connected,
          device.productName ?? 'USB device $deviceId');
    } catch (e) {
      _emit(ConnectionStatus.error, deviceId, error: e.toString());
    }
  }

  void _onBytes(Uint8List bytes) {
    final chunk = utf8.decode(bytes, allowMalformed: true);
    for (final ch in chunk.codeUnits) {
      if (ch == 10 /* \n */) {
        final line = _lineBuffer.toString();
        _lineBuffer.clear();
        final event = RivrProtocol.parseLine(line);
        if (event != null) _eventCtrl.add(event);
      } else {
        _lineBuffer.writeCharCode(ch);
      }
    }
  }

  // ── Send ──────────────────────────────────────────────────────────────────
  @override
  Future<void> send(String command) async {
    await _port?.write(Uint8List.fromList(utf8.encode(command)));
  }

  // ── Disconnect ────────────────────────────────────────────────────────────
  @override
  Future<void> disconnect() async {
    await _rxSub?.cancel();
    await _port?.close();
    _port = null;
    _emit(ConnectionStatus.disconnected, '');
  }

  @override
  void dispose() {
    disconnect();
    _stateCtrl.close();
    _eventCtrl.close();
  }

  void _emit(ConnectionStatus status, String name, {String? error}) {
    _stateCtrl.add(RivrConnState(
      status: status,
      deviceName: name,
      errorMessage: error,
    ));
  }
}
