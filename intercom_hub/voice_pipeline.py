"""
Voice Pipeline — Wyoming protocol client for HA Assist integration.

Manages per-device voice assist sessions:
1. ESP32 detects wake word, sends MQTT start event
2. Hub collects candidates for 500ms, picks loudest (closest to speaker)
3. Hub opens Wyoming connection to HA's Assist pipeline
4. Hub forwards PCM from winning ESP32 (PRIORITY_VOICE_ASSIST packets) to Wyoming
5. Wyoming returns STT transcript + TTS audio
6. Hub sends TTS audio as raw PCM back to ESP32 via unicast UDP
"""

import asyncio
import logging
import math
import struct
import time
from typing import Optional, Dict, List, Callable, Tuple

log = logging.getLogger("voice_pipeline")

# Wyoming imports
try:
    from wyoming.audio import AudioChunk, AudioStart, AudioStop
    from wyoming.asr import Transcribe, Transcript
    from wyoming.event import async_read_event, async_write_event
    HAS_WYOMING = True
except ImportError:
    HAS_WYOMING = False
    log.warning("wyoming package not available — voice pipeline disabled")

# HA Whisper add-on defaults
import os
WHISPER_HOST = os.environ.get('WHISPER_HOST', '') or 'core-whisper'
_whisper_port = os.environ.get('WHISPER_PORT', '') or '10300'
WHISPER_PORT = int(_whisper_port) if _whisper_port.isdigit() else 10300

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2  # 16-bit


def compute_rms(pcm_bytes: bytes) -> float:
    """Compute RMS of 16-bit signed LE PCM data."""
    if len(pcm_bytes) < 2:
        return 0.0
    n_samples = len(pcm_bytes) // 2
    samples = struct.unpack(f'<{n_samples}h', pcm_bytes[:n_samples * 2])
    sum_sq = sum(s * s for s in samples)
    return math.sqrt(sum_sq / n_samples)


class VoiceSession:
    """A single voice assist session for one ESP32 device."""

    def __init__(self, device_id: str, room: str, source_ip: str,
                 send_mqtt_done: Callable,
                 process_response: Optional[Callable] = None,
                 initial_audio: Optional[List[bytes]] = None):
        self.device_id = device_id
        self.room = room
        self.source_ip = source_ip
        self.send_mqtt_done = send_mqtt_done
        self.process_response = process_response
        self.audio_queue: asyncio.Queue = asyncio.Queue(maxsize=200)
        self.active = True
        self._cancelled = False
        self.created_at = time.time()
        self._task: Optional[asyncio.Task] = None
        self._chunks_sent = 0
        self._initial_audio = initial_audio or []

    async def start(self):
        """Start the Wyoming session."""
        self._task = asyncio.create_task(self._run())

    async def _run(self):
        """Run the full STT cycle via Wyoming."""
        try:
            log.info(f"[{self.room}] Opening Wyoming STT session ({WHISPER_HOST}:{WHISPER_PORT})")
            reader, writer = await asyncio.open_connection(WHISPER_HOST, WHISPER_PORT)

            audio_start = AudioStart(
                rate=SAMPLE_RATE,
                width=SAMPLE_WIDTH,
                channels=CHANNELS,
            )
            await async_write_event(audio_start.event(), writer)

            # Feed buffered audio from selection window first
            for pcm_data in self._initial_audio:
                chunk = AudioChunk(
                    rate=SAMPLE_RATE,
                    width=SAMPLE_WIDTH,
                    channels=CHANNELS,
                    audio=pcm_data,
                )
                await async_write_event(chunk.event(), writer)
                self._chunks_sent += 1

            if self._initial_audio:
                log.info(f"[{self.room}] Fed {len(self._initial_audio)} buffered chunks to Wyoming")
            self._initial_audio = []  # Free memory

            # Stream live audio chunks until session ends
            while self.active:
                try:
                    pcm_data = await asyncio.wait_for(
                        self.audio_queue.get(), timeout=1.0
                    )
                    if pcm_data is None:
                        break

                    chunk = AudioChunk(
                        rate=SAMPLE_RATE,
                        width=SAMPLE_WIDTH,
                        channels=CHANNELS,
                        audio=pcm_data,
                    )
                    await async_write_event(chunk.event(), writer)
                    self._chunks_sent += 1
                    if self._chunks_sent == len(self._initial_audio) + 1:
                        log.info(f"[{self.room}] First live audio chunk sent to Wyoming ({len(pcm_data)} bytes)")
                except asyncio.TimeoutError:
                    if not self.active:
                        break
                    continue

            log.info(f"[{self.room}] Sending AudioStop after {self._chunks_sent} chunks")
            await async_write_event(AudioStop().event(), writer)
            transcribe = Transcribe(language="en")
            await async_write_event(transcribe.event(), writer)

            transcript_text = ""
            while True:
                event = await asyncio.wait_for(
                    async_read_event(reader), timeout=30.0
                )
                if event is None:
                    break

                if Transcript.is_type(event.type):
                    transcript = Transcript.from_event(event)
                    transcript_text = transcript.text
                    log.info(f"[{self.room}] STT transcript: '{transcript_text}'")
                    break

            writer.close()
            await writer.wait_closed()

            if self._cancelled:
                log.info(f"[{self.room}] Session cancelled by user — no TTS")
                self.send_mqtt_done(self.device_id)
                return

            if transcript_text and transcript_text.strip():
                log.info(f"[{self.room}] Transcript: '{transcript_text}'")
                if self.process_response:
                    try:
                        self.process_response(transcript_text.strip(), self.source_ip, self.room)
                    except Exception as e:
                        log.error(f"[{self.room}] Response processing error: {e}")
            else:
                log.warning(f"[{self.room}] Empty transcript — no action")

            self.send_mqtt_done(self.device_id)

        except ConnectionRefusedError:
            log.error(f"[{self.room}] Wyoming STT connection refused — is Whisper running at {WHISPER_HOST}:{WHISPER_PORT}?")
            self.send_mqtt_done(self.device_id)
        except asyncio.TimeoutError:
            log.error(f"[{self.room}] Wyoming session timed out")
            self.send_mqtt_done(self.device_id)
        except Exception as e:
            log.error(f"[{self.room}] Voice session error: {e}")
            self.send_mqtt_done(self.device_id)

    def feed_audio(self, pcm_data: bytes, loop: asyncio.AbstractEventLoop):
        """Feed PCM audio into the session (thread-safe)."""
        if self.active:
            loop.call_soon_threadsafe(self._put_audio, pcm_data)

    def _put_audio(self, data):
        try:
            self.audio_queue.put_nowait(data)
        except asyncio.QueueFull:
            pass

    def stop(self, loop: asyncio.AbstractEventLoop):
        """Stop audio streaming (normal end — silence timeout)."""
        self.active = False
        loop.call_soon_threadsafe(self._put_audio, None)

    def cancel(self):
        """Cancel session immediately (PTT pressed)."""
        self.active = False
        self._cancelled = True
        if self._task:
            self._task.cancel()


class _Candidate:
    """A wake word candidate during the selection window."""
    def __init__(self, device_id: str, room: str, source_ip: str):
        self.device_id = device_id
        self.room = room
        self.source_ip = source_ip
        self.audio_chunks: List[bytes] = []
        self.rms_sum: float = 0.0
        self.rms_count: int = 0

    def feed(self, pcm_data: bytes):
        self.audio_chunks.append(pcm_data)
        rms = compute_rms(pcm_data)
        self.rms_sum += rms
        self.rms_count += 1

    @property
    def avg_rms(self) -> float:
        return self.rms_sum / self.rms_count if self.rms_count > 0 else 0.0


class VoicePipelineManager:
    """Manages voice assist sessions with loudness-based device selection.

    When multiple devices detect the wake word simultaneously, audio is
    buffered for a short selection window. The device with the highest
    average RMS (closest to the speaker) wins the session.
    """

    SELECTION_WINDOW_S = 0.5  # 500ms to collect candidates

    def __init__(self, send_mqtt_done: Callable,
                 event_loop: asyncio.AbstractEventLoop,
                 process_response: Optional[Callable] = None):
        self.sessions: Dict[str, VoiceSession] = {}
        self.send_mqtt_done = send_mqtt_done
        self.process_response = process_response
        self.event_loop = event_loop
        self.enabled = HAS_WYOMING

        # Selection state
        self._candidates: Dict[str, _Candidate] = {}
        self._selection_timer: Optional[asyncio.TimerHandle] = None
        self._selecting = False

        # Active session lock (prevents new selections while session is running)
        self._active_session_id: Optional[str] = None

    def on_voice_assist_start(self, device_id: str, room: str, source_ip: str):
        """Handle wake word detection from an ESP32."""
        if not self.enabled:
            log.warning("Voice pipeline not available (missing wyoming package)")
            return

        # Reject if a session is already running
        if self._active_session_id:
            log.info(f"Voice dedup: ignoring {room} — session already active")
            self.send_mqtt_done(device_id)
            return

        # Add to candidates
        self._candidates[device_id] = _Candidate(device_id, room, source_ip)
        log.info(f"Voice candidate: {room} ({device_id}) — {len(self._candidates)} candidate(s)")

        # Start selection timer on first candidate
        if not self._selecting:
            self._selecting = True
            self.event_loop.call_soon_threadsafe(self._schedule_selection)

    def _schedule_selection(self):
        """Schedule the selection callback after the window expires."""
        if self._selection_timer:
            self._selection_timer.cancel()
        self._selection_timer = self.event_loop.call_later(
            self.SELECTION_WINDOW_S, self._select_winner
        )

    def _select_winner(self):
        """Pick the loudest candidate and start a real session."""
        self._selecting = False
        self._selection_timer = None

        if not self._candidates:
            return

        # Find candidate with highest average RMS
        winner = max(self._candidates.values(), key=lambda c: c.avg_rms)

        log.info(f"Voice selection: {len(self._candidates)} candidate(s)")
        for c in self._candidates.values():
            marker = " ← WINNER" if c.device_id == winner.device_id else ""
            log.info(f"  {c.room}: avg_rms={c.avg_rms:.0f}, chunks={c.rms_count}{marker}")

        # Dismiss losers
        for c in self._candidates.values():
            if c.device_id != winner.device_id:
                log.info(f"Voice dedup: dismissing {c.room} (rms={c.avg_rms:.0f})")
                self.send_mqtt_done(c.device_id)

        # Start real session for winner with buffered audio
        self._active_session_id = winner.device_id
        session = VoiceSession(
            device_id=winner.device_id,
            room=winner.room,
            source_ip=winner.source_ip,
            send_mqtt_done=self._on_session_done,
            process_response=self.process_response,
            initial_audio=winner.audio_chunks,
        )
        self.sessions[winner.device_id] = session
        asyncio.run_coroutine_threadsafe(session.start(), self.event_loop)
        log.info(f"Voice session started for {winner.room} ({winner.device_id})")

        self._candidates.clear()

    def _on_session_done(self, device_id: str):
        """Called when a session completes — clears active lock."""
        self._active_session_id = None
        if device_id in self.sessions:
            del self.sessions[device_id]
        self.send_mqtt_done(device_id)

    def on_voice_assist_stop(self, device_id: str):
        """Handle voice_assist stop event (silence timeout)."""
        if device_id in self.sessions:
            self.sessions[device_id].stop(self.event_loop)
            log.info(f"Voice session stopped for {device_id}")
        elif device_id in self._candidates:
            # Stopped during selection window — remove candidate
            del self._candidates[device_id]
            log.info(f"Voice candidate removed (stopped): {device_id}")

    def on_voice_assist_cancel(self, device_id: str):
        """Handle voice_assist cancel event (PTT pressed)."""
        if device_id in self.sessions:
            self.sessions[device_id].cancel()
            del self.sessions[device_id]
            self._active_session_id = None
            log.info(f"Voice session cancelled for {device_id}")
        elif device_id in self._candidates:
            del self._candidates[device_id]
            log.info(f"Voice candidate cancelled: {device_id}")

    def feed_audio(self, device_id: str, pcm_data: bytes):
        """Feed PCM to active session or candidate buffer."""
        if device_id in self.sessions:
            self.sessions[device_id].feed_audio(pcm_data, self.event_loop)
        elif device_id in self._candidates:
            self._candidates[device_id].feed(pcm_data)

    def cleanup_stale(self, max_age_s: float = 60.0):
        """Remove sessions older than max_age_s."""
        now = time.time()
        stale = [did for did, s in self.sessions.items()
                 if now - s.created_at > max_age_s]
        for did in stale:
            self.sessions[did].cancel()
            del self.sessions[did]
            log.warning(f"Cleaned up stale voice session: {did}")
