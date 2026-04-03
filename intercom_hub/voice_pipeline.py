"""
Voice Pipeline — Wyoming protocol client for HA Assist integration.

Manages per-device voice assist sessions:
1. ESP32 detects wake word, sends MQTT start event
2. Hub opens Wyoming connection to HA's Assist pipeline
3. Hub forwards decoded PCM from ESP32 (PRIORITY_VOICE_ASSIST packets) to Wyoming
4. Wyoming returns STT transcript + TTS audio
5. Hub sends TTS audio as raw PCM back to ESP32 via unicast UDP
"""

import asyncio
import logging
import time
from typing import Optional, Dict, Callable

log = logging.getLogger("voice_pipeline")

# Wyoming imports (same package used for TTS in intercom_hub.py)
try:
    from wyoming.audio import AudioChunk, AudioStart, AudioStop
    from wyoming.asr import Transcribe, Transcript
    from wyoming.event import async_read_event, async_write_event
    HAS_WYOMING = True
except ImportError:
    HAS_WYOMING = False
    log.warning("wyoming package not available — voice pipeline disabled")

# HA Whisper add-on defaults (can be overridden via env vars)
import os
WHISPER_HOST = os.environ.get('WHISPER_HOST', '') or 'core-whisper'
_whisper_port = os.environ.get('WHISPER_PORT', '') or '10300'
WHISPER_PORT = int(_whisper_port) if _whisper_port.isdigit() else 10300

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2  # 16-bit


class VoiceSession:
    """A single voice assist session for one ESP32 device."""

    def __init__(self, device_id: str, room: str, source_ip: str,
                 send_mqtt_done: Callable):
        self.device_id = device_id
        self.room = room
        self.source_ip = source_ip
        self.send_mqtt_done = send_mqtt_done
        self.audio_queue: asyncio.Queue = asyncio.Queue(maxsize=200)
        self.active = True
        self.created_at = time.time()
        self._task: Optional[asyncio.Task] = None
        self._chunks_sent = 0

    async def start(self):
        """Start the Wyoming session."""
        self._task = asyncio.create_task(self._run())

    async def _run(self):
        """Run the full STT cycle via Wyoming."""
        try:
            log.info(f"[{self.room}] Opening Wyoming STT session ({WHISPER_HOST}:{WHISPER_PORT})")
            reader, writer = await asyncio.open_connection(WHISPER_HOST, WHISPER_PORT)

            # Send audio start event (Transcribe comes AFTER AudioStop per Wyoming protocol)
            audio_start = AudioStart(
                rate=SAMPLE_RATE,
                width=SAMPLE_WIDTH,
                channels=CHANNELS,
            )
            await async_write_event(audio_start.event(), writer)

            # Stream audio chunks until session ends
            while self.active:
                try:
                    pcm_data = await asyncio.wait_for(
                        self.audio_queue.get(), timeout=1.0
                    )
                    if pcm_data is None:
                        break  # Sentinel: stop signal

                    chunk = AudioChunk(
                        rate=SAMPLE_RATE,
                        width=SAMPLE_WIDTH,
                        channels=CHANNELS,
                        audio=pcm_data,
                    )
                    await async_write_event(chunk.event(), writer)
                    self._chunks_sent += 1
                    if self._chunks_sent == 1:
                        log.info(f"[{self.room}] First audio chunk sent to Wyoming ({len(pcm_data)} bytes)")
                except asyncio.TimeoutError:
                    if not self.active:
                        break
                    continue

            # Send audio stop, then request transcription (Wyoming protocol order)
            log.info(f"[{self.room}] Sending AudioStop after {self._chunks_sent} chunks")
            await async_write_event(AudioStop().event(), writer)
            transcribe = Transcribe(language="en")
            await async_write_event(transcribe.event(), writer)

            # Wait for transcript
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

            if not self.active:
                log.info(f"[{self.room}] Session cancelled — no TTS")
                return

            if transcript_text:
                log.info(f"[{self.room}] Transcript: '{transcript_text}'")
            else:
                log.warning(f"[{self.room}] Empty transcript")

            # Signal ESP32 that voice assist is complete
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
        """Feed decoded PCM audio from ESP32 into the session.
        Called from receive_thread (non-asyncio), so must use thread-safe method."""
        if self.active:
            loop.call_soon_threadsafe(self._put_audio, pcm_data)

    def _put_audio(self, data):
        """Thread-safe audio queue insertion (called via loop.call_soon_threadsafe)."""
        try:
            self.audio_queue.put_nowait(data)
        except asyncio.QueueFull:
            pass

    def stop(self, loop: asyncio.AbstractEventLoop):
        """Stop audio streaming (normal end — silence timeout)."""
        self.active = False
        loop.call_soon_threadsafe(self._put_audio, None)  # Sentinel

    def cancel(self):
        """Cancel session immediately (PTT pressed)."""
        self.active = False
        if self._task:
            self._task.cancel()


class VoicePipelineManager:
    """Manages voice assist sessions for all ESP32 devices."""

    def __init__(self, send_mqtt_done: Callable,
                 event_loop: asyncio.AbstractEventLoop):
        self.sessions: Dict[str, VoiceSession] = {}
        self.send_mqtt_done = send_mqtt_done
        self.event_loop = event_loop
        self.enabled = HAS_WYOMING

    def on_voice_assist_start(self, device_id: str, room: str, source_ip: str):
        """Handle voice_assist start event from ESP32."""
        if not self.enabled:
            log.warning("Voice pipeline not available (missing wyoming package)")
            return

        # Cancel existing session for this device
        if device_id in self.sessions:
            self.sessions[device_id].cancel()

        session = VoiceSession(
            device_id=device_id,
            room=room,
            source_ip=source_ip,
            send_mqtt_done=self.send_mqtt_done,
        )
        self.sessions[device_id] = session
        asyncio.run_coroutine_threadsafe(session.start(), self.event_loop)
        log.info(f"Voice session started for {room} ({device_id})")

    def on_voice_assist_stop(self, device_id: str):
        """Handle voice_assist stop event (silence timeout)."""
        if device_id in self.sessions:
            self.sessions[device_id].stop(self.event_loop)
            log.info(f"Voice session stopped for {device_id}")

    def on_voice_assist_cancel(self, device_id: str):
        """Handle voice_assist cancel event (PTT pressed)."""
        if device_id in self.sessions:
            self.sessions[device_id].cancel()
            del self.sessions[device_id]
            log.info(f"Voice session cancelled for {device_id}")

    def feed_audio(self, device_id: str, pcm_data: bytes):
        """Feed decoded PCM to the appropriate session."""
        if device_id in self.sessions:
            self.sessions[device_id].feed_audio(pcm_data, self.event_loop)

    def cleanup_stale(self, max_age_s: float = 60.0):
        """Remove sessions older than max_age_s."""
        now = time.time()
        stale = [did for did, s in self.sessions.items()
                 if now - s.created_at > max_age_s]
        for did in stale:
            self.sessions[did].cancel()
            del self.sessions[did]
            log.warning(f"Cleaned up stale voice session: {did}")
