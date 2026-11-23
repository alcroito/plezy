// SPDX-License-Identifier: BSD-3-Clause
// webos_native_player example: plays one video through the TV's own decoders.
//
//   flutter-webos build webos --release --dart-define=VIDEO_URL=http://<server>/<file>.mkv
//
// Remote: OK pauses / resumes, Left / Right seek 10 s, Up changes the speed (1, 1.5, 2, 0.5x),
// Down picks the next audio track, Back exits.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:webos_native_player/webos_native_player.dart';

const videoUrl = String.fromEnvironment('VIDEO_URL');

void main() => runApp(const MaterialApp(
      debugShowCheckedModeBanner: false,
      // The video plane is under the Flutter surface: everything above it stays transparent.
      color: Colors.transparent,
      home: PlayerPage(),
    ));

class PlayerPage extends StatefulWidget {
  const PlayerPage({super.key});

  @override
  State<PlayerPage> createState() => _PlayerPageState();
}

class _PlayerPageState extends State<PlayerPage> {
  NativePlayerController? _controller;
  String? _error;

  @override
  void initState() {
    super.initState();
    if (videoUrl.isNotEmpty) _open();
  }

  Future<void> _open() async {
    final c = NativePlayerController(videoUrl);
    c.addListener(() => setState(() {}));
    setState(() => _controller = c);
    try {
      await c.initialize();
      await c.play();
    } catch (e) {
      setState(() => _error = '$e');
    }
  }

  @override
  void dispose() {
    _controller?.dispose();
    super.dispose();
  }

  KeyEventResult _onKey(FocusNode node, KeyEvent e) {
    final c = _controller;
    if (c == null || e is! KeyDownEvent) return KeyEventResult.ignored;
    final v = c.value;
    final k = e.logicalKey;
    if (k == LogicalKeyboardKey.select || k == LogicalKeyboardKey.enter || k == LogicalKeyboardKey.mediaPlayPause) {
      v.playing ? c.pause() : c.play();
    } else if (k == LogicalKeyboardKey.arrowRight) {
      c.seekTo(v.position + const Duration(seconds: 10));
    } else if (k == LogicalKeyboardKey.arrowUp) {
      const speeds = [1.0, 1.5, 2.0, 0.5];
      c.setSpeed(speeds[(speeds.indexOf(v.speed) + 1) % speeds.length]);
    } else if (k == LogicalKeyboardKey.arrowDown) {
      final audio = [for (final t in v.tracks) if (t.isAudio) t];
      if (audio.length > 1) {
        final i = audio.indexWhere((t) => t.index == v.audioStream);
        c.setAudioStream(audio[(i + 1) % audio.length].index);
      }
    } else if (k == LogicalKeyboardKey.arrowLeft) {
      final to = v.position - const Duration(seconds: 10);
      c.seekTo(to < Duration.zero ? Duration.zero : to);
    } else {
      return KeyEventResult.ignored;
    }
    return KeyEventResult.handled;
  }

  static String _audioLine(NativePlayerValue v) {
    final audio = [for (final t in v.tracks) if (t.isAudio) t];
    if (audio.length < 2) return '';
    final cur = audio.where((t) => t.index == v.audioStream).firstOrNull;
    return '\nAudio ${audio.indexOf(cur ?? audio.first) + 1}/${audio.length}: ${cur?.title ?? cur?.codec ?? '?'}   (Down: next)';
  }

  static String _t(Duration d) =>
      '${d.inMinutes}:${(d.inSeconds % 60).toString().padLeft(2, '0')}';

  @override
  Widget build(BuildContext context) {
    final c = _controller;
    final v = c?.value;
    final status = videoUrl.isEmpty
        ? 'Build with --dart-define=VIDEO_URL=<http URL of a video file>'
        : _error ?? v?.error ??
            (v == null || !v.initialized
                ? 'Loading…'
                : '${v.videoCodec} ${v.size.width.toInt()}x${v.size.height.toInt()}, ${v.audioCodec}   '
                    '${_t(v.position)} / ${_t(v.duration)}${v.speed != 1.0 ? '   ${v.speed}x' : ''}${v.playing ? '' : '   paused'}${v.completed ? '   ended' : ''}'
                    '${_audioLine(v)}');
    return Scaffold(
      backgroundColor: Colors.transparent,
      body: Focus(
        autofocus: true,
        onKeyEvent: _onKey,
        child: Stack(children: [
          if (c != null) const NativeVideoView(),
          Positioned(
            left: 48,
            bottom: 40,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
              decoration: BoxDecoration(color: const Color(0xAA000000), borderRadius: BorderRadius.circular(12)),
              child: Text(status, style: const TextStyle(color: Colors.white, fontSize: 24)),
            ),
          ),
        ]),
      ),
    );
  }
}
