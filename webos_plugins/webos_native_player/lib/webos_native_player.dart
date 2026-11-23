// SPDX-License-Identifier: BSD-3-Clause
// Video on LG webOS TVs through the TV's own decoders: the plugin reads the source with a bundled
// FFmpeg and feeds the elementary streams to LG's media pipeline, which draws into an exported
// window under the Flutter surface. The page above the video must be transparent there.
import 'dart:async';
import 'dart:convert';
import 'dart:ui' as ui;

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

/// The subtitle showing now: pictures (PGS, VobSub, DVB; positioned on [canvas]) or text.
@immutable
class NativeSubtitle {
  final String text;
  final Size canvas;
  final List<(Rect, ui.Image)> images;
  const NativeSubtitle({this.text = '', this.canvas = Size.zero, this.images = const []});
  bool get isEmpty => text.isEmpty && images.isEmpty;
}

class _Cue {
  _Cue(this.start, this.end, this.canvas, this.text, this.images);
  final int start, end;  // ms; end -1: until the next cue
  final Size canvas;
  final String text;
  final List<(Rect, ui.Image?)> images;
}

/// An audio or subtitle track in the file.
@immutable
class NativeTrack {
  final int index;          // FFmpeg stream index (the one to pass back)
  final bool isAudio;
  final String codec;       // FFmpeg codec name: eac3, truehd, dts, aac, subrip, ass, hdmv_pgs_subtitle...
  final int channels;       // audio
  final bool image;         // subtitle drawn as pictures (PGS, VobSub, DVB)
  final String? language;   // ISO 639-2 as tagged in the file
  final String? title;
  final bool isDefault;
  final bool forced;

  const NativeTrack({required this.index, required this.isAudio, required this.codec, this.channels = 0, this.image = false,
      this.language, this.title, this.isDefault = false, this.forced = false});

  factory NativeTrack.fromJson(Map<String, dynamic> j) => NativeTrack(
        index: j['index'] as int,
        isAudio: j['type'] == 'audio',
        codec: j['codec'] as String? ?? '',
        channels: j['channels'] as int? ?? 0,
        image: j['image'] == true,
        language: j['language'] as String?,
        title: j['title'] as String?,
        isDefault: j['default'] == true,
        forced: j['forced'] == true,
      );

  @override
  String toString() => '#$index ${isAudio ? 'audio' : 'subtitle'} $codec${language != null ? ' $language' : ''}${title != null ? ' "$title"' : ''}';
}

@immutable
class NativePlayerValue {
  final bool initialized;
  final bool playing;
  final bool completed;
  final Duration duration;
  final Duration position;
  final Size size;
  final String videoCodec;
  final String audioCodec;
  final String? error;

  /// Why playback failed, with [error]: `decoder_busy` (another app or player holds the TV's
  /// decoder), `stalled` (the TV took the stream but shows no picture), `feed_refused`,
  /// `load_refused`, `pipeline_error`, `open_failed`, `read_failed` or `unsupported_codec`.
  final String? errorCode;

  /// How far the stream has been read and handed to the TV (for a buffered range on a seek bar).
  final Duration buffered;

  /// Playback information for a stats overlay: container, videoCodec, width, height, fps,
  /// hdrType (the TV's own report: SDR, HDR10, DolbyVision...), dolbyVision, audioSource,
  /// audioOut, audioPath, leadMs, videoKbps (measured), feedRetries.
  final Map<String, String> info;

  /// The file's audio and subtitle tracks, and the audio track playing (FFmpeg stream index).
  final List<NativeTrack> tracks;
  final int audioStream;

  /// Playback speed (1.0 normal) and the highest this video allows (its frame rate times the speed
  /// may not pass 60).
  final double speed;
  final double maxSpeed;

  const NativePlayerValue({
    this.initialized = false,
    this.playing = false,
    this.completed = false,
    this.duration = Duration.zero,
    this.position = Duration.zero,
    this.size = Size.zero,
    this.videoCodec = '',
    this.audioCodec = '',
    this.error,
    this.errorCode,
    this.buffered = Duration.zero,
    this.info = const {},
    this.tracks = const [],
    this.audioStream = -1,
    this.speed = 1.0,
    this.maxSpeed = 2.0,
  });

  NativePlayerValue copyWith({
    bool? initialized,
    bool? playing,
    bool? completed,
    Duration? duration,
    Duration? position,
    Size? size,
    String? videoCodec,
    String? audioCodec,
    String? error,
    String? errorCode,
    Duration? buffered,
    Map<String, String>? info,
    List<NativeTrack>? tracks,
    int? audioStream,
    double? speed,
    double? maxSpeed,
  }) =>
      NativePlayerValue(
        initialized: initialized ?? this.initialized,
        playing: playing ?? this.playing,
        completed: completed ?? this.completed,
        duration: duration ?? this.duration,
        position: position ?? this.position,
        size: size ?? this.size,
        videoCodec: videoCodec ?? this.videoCodec,
        audioCodec: audioCodec ?? this.audioCodec,
        error: error ?? this.error,
        errorCode: errorCode ?? this.errorCode,
        buffered: buffered ?? this.buffered,
        info: info ?? this.info,
        tracks: tracks ?? this.tracks,
        audioStream: audioStream ?? this.audioStream,
        speed: speed ?? this.speed,
        maxSpeed: maxSpeed ?? this.maxSpeed,
      );
}

class NativePlayerController extends ValueNotifier<NativePlayerValue> with WidgetsBindingObserver {
  NativePlayerController(this.url,
      {this.startAt = Duration.zero,
      this.audioStream,
      int? subtitleStream,
      Duration audioDelay = Duration.zero,
      this.releaseInBackground = true})
      : _subtitleStream = subtitleStream,
        _audioDelay = audioDelay,
        super(const NativePlayerValue());

  final String url;
  final Duration startAt;

  /// FFmpeg stream index of the audio track; null picks the file's default.
  final int? audioStream;

  /// Release the TV's decoder while the app is in the background (another app can then play
  /// video) and reopen at the same position when it comes back.
  final bool releaseInBackground;

  static const _channel = MethodChannel('webos_native_player');
  static const _windows = MethodChannel('webos/exported_windows');

  int? _id;
  String? _windowId;
  StreamSubscription? _events;
  Timer? _positionTimer;
  bool _disposed = false;
  bool _suspended = false;
  bool _playingBeforeSuspend = false;
  Completer<void>? _ready;
  final List<(DateTime, int)> _byteSamples = [];
  int? _audioOverride;  // set by setAudioStream; used when reopening after the background
  int? _subtitleStream;
  String _subtitleMode = 'none';
  Duration _audioDelay;

  /// Lip-sync offset: positive plays the sound later than the picture.
  Duration get audioDelay => _audioDelay;  // ass, pictures, text or none (from the plugin)
  bool _assInFlight = false;
  final List<_Cue> _cues = [];
  int _cueGeneration = -1;
  Timer? _subtitleTimer;
  DateTime _positionAt = DateTime.now();

  /// The subtitle to show now (decoded from the file; see [setSubtitleStream]).
  final ValueNotifier<NativeSubtitle> subtitle = ValueNotifier(const NativeSubtitle());

  /// Starts loading; completes when the first picture's pipeline is ready, or throws.
  Future<void> initialize() async {
    _windowId = await _windows.invokeMethod<String>('create', {});
    if (releaseInBackground) WidgetsBinding.instance.addObserver(this);
    _positionTimer = Timer.periodic(const Duration(milliseconds: 250), (_) => _pollStatus());
    _subtitleTimer = Timer.periodic(const Duration(milliseconds: 100), (_) => _pickSubtitle());
    return _open(startAt, audioStream);
  }

  Future<void> _open(Duration at, int? audio) async {
    final ready = _ready = Completer<void>();
    _byteSamples.clear();
    _id = await _channel.invokeMethod<int>('open', {
      'url': url,
      'windowId': _windowId,
      'startMs': at.inMilliseconds,
      if (audio != null) 'audioStream': audio,
      if (_subtitleStream != null) 'subtitleStream': _subtitleStream,
      if (_audioDelay != Duration.zero) 'audioDelayMs': _audioDelay.inMilliseconds,
    });
    _events = EventChannel('webos_native_player/events/$_id').receiveBroadcastStream().listen((raw) => _onEvent(Map<String, String>.from(raw as Map), at));
    return ready.future;
  }

  void _onEvent(Map<String, String> e, Duration openedAt) {
    final ready = _ready;
    switch (e['event']) {
      case 'initialized':
        final at = int.tryParse(e['position'] ?? '');
        List<NativeTrack>? tracks;
        try {
          tracks = [for (final t in jsonDecode(e['tracks'] ?? '[]') as List) NativeTrack.fromJson(t as Map<String, dynamic>)];
        } on Object catch (_) {}
        value = value.copyWith(
          initialized: true,
          playing: true,
          duration: Duration(milliseconds: int.tryParse(e['duration'] ?? '') ?? 0),
          position: at != null ? Duration(milliseconds: at) : openedAt,
          tracks: tracks,
          audioStream: int.tryParse(e['audioStream'] ?? ''),
          size: Size(double.tryParse(e['width'] ?? '') ?? 0, double.tryParse(e['height'] ?? '') ?? 0),
          videoCodec: e['videoCodec'],
          audioCodec: e['audioCodec'],
        );
        _subtitleMode = e['subtitleMode'] ?? _subtitleMode;
        // The window's source rect must be the video's real size (3840x2160 for 4K): a smaller
        // one makes the TV's scaler treat the picture as a crop of it (colour faults in 4K).
        _placeWindow(value.size);
        if (ready != null && !ready.isCompleted) ready.complete();
      case 'subtitleChanged':
        _subtitleMode = e['subtitleMode'] ?? 'none';
        subtitle.value = const NativeSubtitle();
      case 'speedChanged':
        value = value.copyWith(speed: (int.tryParse(e['speed'] ?? '') ?? 1000) / 1000);
      case 'speedRefused':
        value = value.copyWith(speed: value.speed, maxSpeed: (int.tryParse(e['maxSpeed'] ?? '') ?? 1000) / 1000);
        debugPrint('webos_native_player: speed ${e['speed']} refused (max ${e['maxSpeed']})');
      case 'audioChanged':
        value = value.copyWith(audioStream: int.tryParse(e['audioStream'] ?? ''), audioCodec: e['audioCodec']);
      case 'completed':
        value = value.copyWith(completed: true, playing: false);
      case 'seeked':
        value = value.copyWith(position: Duration(milliseconds: int.tryParse(e['position'] ?? '') ?? 0));
      case 'error':
        value = value.copyWith(error: e['message'] ?? 'Playback error', errorCode: e['code'] ?? 'error', playing: false);
        if (ready != null && !ready.isCompleted) ready.completeError(StateError(value.error!));
      default:
        debugPrint('webos_native_player: $e');
    }
  }

  Future<void> _placeWindow(Size video) async {
    final win = _windowId;
    if (win == null || video.width <= 0 || video.height <= 0 || _disposed) return;
    final rects = {
      'windowId': win,
      'srcRect': [0, 0, video.width.toInt(), video.height.toInt()],
      'dstRect': [0, 0, 1920, 1080],
    };
    // setExportedWindow places the plane with the compositor's set_exported_window (source and
    // destination), as LG's SDL does for native apps; 'resize' uses set_crop_region, which leaves
    // 4K buffer-fed video with a colour seam across the middle on webOS 6.
    try {
      await _windows.invokeMethod<void>('setExportedWindow', rects);
    } on Object catch (e) {
      debugPrint('webos_native_player: setExportedWindow failed ($e); using resize');
      await _windows.invokeMethod<void>('resize', rects);
    }
  }

  Future<void> _pollStatus() async {
    final id = _id;
    if (id == null || _disposed || !value.initialized) return;
    final raw = await _channel.invokeMapMethod<String, String>('status', {'id': id});
    if (raw == null || _disposed || id != _id) return;
    final info = Map<String, String>.from(raw);
    // Video bitrate over the last ten seconds of feeding.
    final now = DateTime.now();
    final bytes = int.tryParse(info['fedVideoBytes'] ?? '') ?? 0;
    _byteSamples.add((now, bytes));
    _byteSamples.removeWhere((s) => now.difference(s.$1) > const Duration(seconds: 10));
    final first = _byteSamples.first;
    final secs = now.difference(first.$1).inMilliseconds / 1000;
    if (secs >= 2) info['videoKbps'] = ((bytes - first.$2) * 8 / 1000 / secs).round().toString();
    final ms = int.tryParse(info['position'] ?? '') ?? 0;
    if (ms > 0) _positionAt = now;
    if (_subtitleStream != null) await _pollSubtitles(id);
    if (_disposed || id != _id) return;
    final maxSpeed = int.tryParse(info['maxSpeed'] ?? '');
    value = value.copyWith(
      maxSpeed: maxSpeed != null ? maxSpeed / 1000 : null,
      position: ms > 0 ? Duration(milliseconds: ms) : null,
      buffered: Duration(milliseconds: int.tryParse(info['buffered'] ?? '') ?? 0),
      info: info,
    );
  }

  Future<void> _pollSubtitles(int id) async {
    final raw = await _channel.invokeMapMethod<String, Object?>('subtitles', {'id': id});
    if (raw == null || _disposed) return;
    final generation = raw['generation'] as int? ?? 0;
    if (generation != _cueGeneration) {
      _cueGeneration = generation;
      _cues.clear();
    }
    for (final c in (raw['cues'] as List? ?? const [])) {
      final m = (c as Map).cast<String, Object?>();
      final images = <(Rect, ui.Image?)>[];
      final cue = _Cue(m['start'] as int, m['end'] as int,
          Size((m['cw'] as int).toDouble(), (m['ch'] as int).toDouble()), m['text'] as String? ?? '', images);
      for (final i in (m['images'] as List? ?? const [])) {
        final im = (i as Map).cast<String, Object?>();
        final w = im['w'] as int, h = im['h'] as int;
        final rect = Rect.fromLTWH((im['x'] as int).toDouble(), (im['y'] as int).toDouble(), w.toDouble(), h.toDouble());
        final slot = images.length;
        images.add((rect, null));
        ui.decodeImageFromPixels(im['rgba'] as Uint8List, w, h, ui.PixelFormat.rgba8888, (img) {
          images[slot] = (rect, img);
        });
      }
      _cues.add(cue);
    }
    _cues.sort((a, b) => a.start.compareTo(b.start));
    // Keep the last minute and what is ahead.
    final pos = value.position.inMilliseconds;
    _cues.removeWhere((c) => c.end >= 0 && c.end < pos - 60000);
  }

  void _pickSubtitle() {
    if (_disposed) return;
    var pos = value.position.inMilliseconds;
    if (value.playing) pos += (DateTime.now().difference(_positionAt).inMilliseconds * value.speed).round().clamp(0, 2000);
    if (_subtitleMode == 'ass') {
      _renderAss(pos);
      return;
    }
    _Cue? current;
    for (final c in _cues) {
      if (c.start > pos) break;
      current = c;
    }
    NativeSubtitle next = const NativeSubtitle();
    if (current != null && (current.end < 0 || pos < current.end)) {
      final ready = [for (final i in current.images) if (i.$2 != null) (i.$1, i.$2!)];
      if (ready.length == current.images.length) {
        next = NativeSubtitle(text: current.text, canvas: current.canvas, images: ready);
      } else {
        return;  // pictures still decoding: keep what is shown
      }
    }
    final prev = subtitle.value;
    if (prev.text != next.text || prev.images.length != next.images.length ||
        (next.images.isNotEmpty && !identical(prev.images.first.$2, next.images.first.$2))) {
      subtitle.value = next;
    }
  }

  // ASS is drawn by libass for the moment shown (lines move and fade): one request at a time.
  Future<void> _renderAss(int pos) async {
    final id = _id;
    if (id == null || _assInFlight) return;
    _assInFlight = true;
    try {
      const canvas = Size(1920, 1080);
      final raw = await _channel.invokeMapMethod<String, Object?>('assFrame', {'id': id, 'ms': pos, 'w': 1920, 'h': 1080});
      if (raw == null || _disposed) return;  // unchanged
      final w = raw['w'] as int, h = raw['h'] as int;
      if (w <= 0 || h <= 0) {
        subtitle.value = const NativeSubtitle();
        return;
      }
      final done = Completer<ui.Image>();
      ui.decodeImageFromPixels(raw['rgba'] as Uint8List, w, h, ui.PixelFormat.rgba8888, done.complete);
      final img = await done.future;
      if (_disposed) return;
      final rect = Rect.fromLTWH((raw['x'] as int).toDouble(), (raw['y'] as int).toDouble(), w.toDouble(), h.toDouble());
      subtitle.value = NativeSubtitle(canvas: canvas, images: [(rect, img)]);
    } finally {
      _assInFlight = false;
    }
  }

  /// Decodes this subtitle track of the file ([NativeTrack.index]; null: none) for
  /// [NativeSubtitleView] / [subtitle].
  Future<void> setSubtitleStream(int? index) async {
    _subtitleStream = index;
    _cues.clear();
    subtitle.value = const NativeSubtitle();
    if (_id == null) return;
    await _channel.invokeMethod('setSubtitleStream', {'id': _id, 'index': index ?? -1});
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (_disposed) return;
    if ((state == AppLifecycleState.hidden || state == AppLifecycleState.paused) && !_suspended && _id != null) {
      _suspend();
    } else if (state == AppLifecycleState.resumed && _suspended) {
      _resume();
    }
  }

  Future<void> _suspend() async {
    _suspended = true;
    _playingBeforeSuspend = value.playing;
    final id = _id;
    _id = null;
    await _events?.cancel();
    if (id != null) await _channel.invokeMethod('dispose', {'id': id});
    debugPrint('webos_native_player: decoder released in the background at ${value.position}');
  }

  Future<void> _resume() async {
    _suspended = false;
    if (value.completed || value.error != null) return;
    try {
      final speed = value.speed;
      await _open(value.position, _audioOverride ?? audioStream);
      if (speed != 1.0) await setSpeed(speed);
      if (!_playingBeforeSuspend) await pause();
    } on Object catch (e) {
      debugPrint('webos_native_player: reopening after the background failed: $e');
    }
  }

  Future<void> play() async {
    if (_id == null) return;
    await _channel.invokeMethod('play', {'id': _id});
    value = value.copyWith(playing: true, completed: false);
  }

  Future<void> pause() async {
    if (_id == null) return;
    await _channel.invokeMethod('pause', {'id': _id});
    value = value.copyWith(playing: false);
  }

  /// Playback speed, 0.5 to 2.0 (above [NativePlayerValue.maxSpeed] it is refused). The sound keeps
  /// its pitch. Applied by reloading the TV's player at the position (a short pause).
  Future<void> setSpeed(double speed) async {
    if (_id == null) return;
    await _channel.invokeMethod('setSpeed', {'id': _id, 'permille': (speed * 1000).round()});
  }

  /// Sound this much later (negative: earlier) than the picture; applied at once.
  Future<void> setAudioDelay(Duration delay) async {
    _audioDelay = delay;
    if (_id == null) return;
    await _channel.invokeMethod('setAudioDelay', {'id': _id, 'ms': delay.inMilliseconds});
  }

  /// Switches to another audio track of the file while playing ([NativeTrack.index]).
  Future<void> setAudioStream(int index) async {
    if (_id == null) return;
    _audioOverride = index;
    await _channel.invokeMethod('setAudioStream', {'id': _id, 'index': index});
  }

  Future<void> seekTo(Duration to) async {
    if (_id == null) return;
    await _channel.invokeMethod('seekTo', {'id': _id, 'ms': to.inMilliseconds});
    value = value.copyWith(position: to, completed: false);
  }

  @override
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    WidgetsBinding.instance.removeObserver(this);
    _positionTimer?.cancel();
    _subtitleTimer?.cancel();
    await _events?.cancel();
    final id = _id, win = _windowId;
    subtitle.dispose();
    super.dispose();
    if (id != null) await _channel.invokeMethod('dispose', {'id': id});
    // The pipeline unloads in the background; give it the window a little longer.
    if (win != null) {
      Future<void>.delayed(const Duration(seconds: 3), () => _windows.invokeMethod<void>('dispose', {'windowId': win}));
    }
  }
}

/// The file's subtitles over the video: pictures scaled from their canvas to this widget's size,
/// text with [textStyle] (or the app draws [NativePlayerController.subtitle] itself).
class NativeSubtitleView extends StatelessWidget {
  const NativeSubtitleView({super.key, required this.controller, this.textStyle, this.bottom = 80, this.drawText = true});
  final NativePlayerController controller;
  final TextStyle? textStyle;
  final double bottom;
  /// False when the app draws text cues itself ([NativePlayerController.subtitle]); pictures and
  /// ASS are still drawn here.
  final bool drawText;

  @override
  Widget build(BuildContext context) => ValueListenableBuilder<NativeSubtitle>(
        valueListenable: controller.subtitle,
        builder: (context, sub, _) {
          if (sub.isEmpty) return const SizedBox.shrink();
          if (sub.images.isNotEmpty) return CustomPaint(size: Size.infinite, painter: _SubtitlePainter(sub));
          if (!drawText) return const SizedBox.shrink();
          return Align(
            alignment: Alignment.bottomCenter,
            child: Padding(
              padding: EdgeInsets.only(bottom: bottom, left: 80, right: 80),
              child: Text(sub.text,
                  textAlign: TextAlign.center,
                  style: textStyle ??
                      const TextStyle(color: Color(0xFFFFFFFF), fontSize: 44, shadows: [Shadow(blurRadius: 4, color: Color(0xFF000000))])),
            ),
          );
        },
      );
}

class _SubtitlePainter extends CustomPainter {
  _SubtitlePainter(this.sub);
  final NativeSubtitle sub;

  @override
  void paint(Canvas canvas, Size size) {
    final cw = sub.canvas.width > 0 ? sub.canvas.width : size.width;
    final ch = sub.canvas.height > 0 ? sub.canvas.height : size.height;
    final sx = size.width / cw, sy = size.height / ch;
    final paint = Paint()..filterQuality = FilterQuality.medium;
    for (final (rect, img) in sub.images) {
      final dst = Rect.fromLTWH(rect.left * sx, rect.top * sy, rect.width * sx, rect.height * sy);
      canvas.drawImageRect(img, Rect.fromLTWH(0, 0, img.width.toDouble(), img.height.toDouble()), dst, paint);
    }
  }

  @override
  bool shouldRepaint(_SubtitlePainter old) => !identical(old.sub, sub);
}

/// Where the video shows: a transparent hole in the Flutter surface.
class NativeVideoView extends StatelessWidget {
  const NativeVideoView({super.key});
  @override
  Widget build(BuildContext context) => const SizedBox.expand();
}
