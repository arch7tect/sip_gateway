import asyncio
import io
import logging
import os
import re
import struct
import time
from enum import Enum
from pathlib import Path
from typing import Optional, Dict, cast
from typing import TYPE_CHECKING

import aiohttp
import numpy as np
import pjsua2 as pj
import scipy
import aiofiles
from numpy import ndarray
from pjsua2 import CallInfo
from scipy.io import wavfile
from uuid_extensions import uuid7

from src.backend.client_bot_base import ClientBotSession
from src.integrations.sip.audio_message import AudioMessage
from src.integrations.sip.task_manager import TaskManager, TaskName
from src.integrations.sip.vad.processor6 import StreamingVADProcessor

if TYPE_CHECKING:
    from src.integrations.sip.pjapp import PjApp
from src.integrations.sip.async_callback import AsyncCallbackJob
from src.integrations.sip.audio_media_player import SmartPlayer
from src.integrations.sip.recording_port import RecordingPort
from src.integrations.sip.sip_config import sip_config

logger = logging.getLogger(__name__)
logging.getLogger("src.integrations.sip.vad.processor").setLevel(logging.INFO)


END_OF_CONVERSATION = object()
END_OF_STREAM = object()


class AudioMediaPort(pj.AudioMediaPort):
    def __init__(self, call):
        pj.AudioMediaPort.__init__(self)
        self.call: PjCall = call

    def onFrameRequested(self, frame):
        frame.type = pj.PJMEDIA_FRAME_TYPE_AUDIO

    def onFrameReceived(self, frame):
        byte_data = bytes(frame.buf)
        if byte_data:
            AsyncCallbackJob(self.call.process, byte_data).submit()


class CallState(Enum):
    WAIT_FOR_USER = "WAIT_FOR_USER"
    SPECULATIVE_GENERATE = "SPECULATIVE_GENERATE"
    COMMIT_GENERATE = "COMMIT_GENERATE"
    FINISHED = "FINISHED"
    HANGED_UP = "HANGED_UP"


class PjCall(pj.Call, ClientBotSession):
    def __init__(self, app: 'PjApp', user_id: str, name: str, conversation_id: Optional[str] = None,
                 call_id=pj.PJSUA_INVALID_ID):
        pj.Call.__init__(self, app.acc, call_id)
        if call_id != pj.PJSUA_INVALID_ID:
            ci = self.getInfo()
            uri_info = self.parse_sip_uri(ci.remoteUri)
            user_id = user_id or uri_info["uri"]
            name = name or uri_info["name"] or uri_info["user"]
            conversation_id = conversation_id or ci.callIdString
        ClientBotSession.__init__(self, app, user_id, name, app.get_app_name(), conversation_id)
        self.med_port: AudioMediaPort | None = None
        self.aud_med: pj.AudioMedia | None = None
        self.smart_player: SmartPlayer | None = None
        self.app: PjApp = app
        self.wav_recorder: RecordingPort | None = None

        self.processor = self._create_processor()
        
        self.state = CallState.WAIT_FOR_USER
        self.start_time = time.perf_counter()
        self.start_response_generation: float = 0.0
        self.start_reply_generation: float = 0.0
        self.message_queue: asyncio.Queue[AudioMessage] = asyncio.Queue()
        self.is_playing = False
        self.unstable_speech_result: Optional[str] = None
        self.tasks: TaskManager = TaskManager()
        self.to_uri: Optional[str] = None
        self.transfer_delay = 1.0
        self.xfer_started = False
        self._media_refs = []
        self.status: Optional[str] = None  # session close status

    @staticmethod
    def parse_sip_uri(uri):
        result = {"name": "", "user": "", "domain": "", "params": {}, "uri": uri}

        if not uri:
            return result

        # Extract display name if present
        if "<" in uri and ">" in uri:
            result["name"] = uri.split("<")[0].strip().strip('"')
            uri = uri[uri.find("<") + 1:uri.find(">")]
            result["uri"] = uri

        # Remove sip: prefix
        if uri.lower().startswith(("sip:", "sips:")):
            uri = uri[uri.find(":") + 1:]

        # Extract params
        if ";" in uri:
            uri_part, params_part = uri.split(";", 1)
            result["params"] = {p.split("=")[0]: p.split("=")[1] if "=" in p else True for p in params_part.split(";")}
        else:
            uri_part = uri

        # Extract user and domain
        if "@" in uri_part:
            result["user"], result["domain"] = uri_part.split("@", 1)
        else:
            result["domain"] = uri_part

        return result

    def _create_processor(self) -> StreamingVADProcessor:
        processor = StreamingVADProcessor(
            vad_engine=self.app.vad_engine,
            threshold = sip_config.vad_threshold,
            min_speech_duration_ms = sip_config.vad_min_speech_duration_ms,
            min_silence_duration_ms = sip_config.vad_min_silence_duration_ms,
            speech_pad_ms = sip_config.vad_speech_pad_ms,
            short_pause_ms = sip_config.short_pause_offset_ms,
            long_pause_ms = sip_config.long_pause_offset_ms,
            user_silence_duration_ms = sip_config.user_silence_timeout_ms,
            speech_prob_window = sip_config.vad_speech_prob_window,
            on_speech_start = self._on_user_speech_start,
            on_speech_end = self._on_user_speech_stop,
            on_short_pause = self._on_short_pause,
            on_long_pause = self._on_long_pause,
            on_user_salience_timeout = self._on_user_salience_timeout,
        )
        return processor
    
    def unstable_speech_result_is_the_same(self, unstable_speech_result) -> bool:
        if self.unstable_speech_result is None:
            return False
        normalize = lambda s: re.sub(r'\s+', ' ', s.lower()).strip()
        return normalize(self.unstable_speech_result) == normalize(unstable_speech_result)

    def send_bye_with_tag(self, tag: str):
        prm = pj.CallOpParam()
        opt = pj.SipTxOption()
        hdr = pj.SipHeader()
        hdr.hName = "X-App-Bye-Tag"
        hdr.hValue = tag
        hdrs = pj.SipHeaderVector()
        hdrs.push_back(hdr)
        opt.headers = hdrs
        prm.txOption = opt
        logger.info("Sending BYE. [tag=%s, session_id=%s]", tag, self.session_id)
        self.hangup(prm)

    def onCallState(self, prm):
        try:
            ci: CallInfo = self.getInfo()
            logger.debug("onCallState. [call_id=%s, uri=%s, state=%s, state_text=%s, session_id=%s]",
                         ci.callIdString, ci.remoteUri, ci.state, ci.stateText, self.session_id)
            if ci.state == pj.PJSIP_INV_STATE_DISCONNECTED:
                status = self.status if self.status else \
                    "declined" if ci.lastStatusCode == pj.PJSIP_SC_DECLINE else \
                    "busy" if ci.lastStatusCode == pj.PJSIP_SC_BUSY_HERE else \
                    "canceled" if ci.lastStatusCode == pj.PJSIP_SC_REQUEST_TERMINATED else \
                    "noanswer" if ci.lastStatusCode in (
                        pj.PJSIP_SC_TEMPORARILY_UNAVAILABLE, pj.PJSIP_SC_REQUEST_TIMEOUT) else \
                    "not_found" if ci.lastStatusCode == pj.PJSIP_SC_NOT_FOUND else \
                    "network_error" if ci.lastStatusCode in (
                                pj.PJSIP_SC_SERVICE_UNAVAILABLE, pj.PJSIP_SC_SERVER_TIMEOUT) else \
                    "completed" if ci.lastStatusCode == pj.PJSIP_SC_OK else \
                    "unknown"
                logger.debug("Call disconnected. [status=%s, last_status_code=%s, last_reason=%s, session_id=%s]", status, ci.lastStatusCode, ci.lastReason, self.session_id)
                self.close_call()
                AsyncCallbackJob(self.close_session, status).submit()
            elif ci.state == pj.PJSIP_INV_STATE_CONFIRMED:
                self.open_call()
                codec_name = self.getStreamInfo(0).codecName
                fmt = self.aud_med.getPortInfo().format
                logger.debug("Call connected. [codec=%s, clock_rate=%s, channel_count=%s, bits_per_sample=%s, frame_time_usec=%s, session_id=%s]",
                    codec_name, fmt.clockRate, fmt.channelCount, fmt.bitsPerSample, fmt.frameTimeUsec, self.session_id)
                greeting_message = self.metadata.get('greeting_message')
                if greeting_message:
                    AsyncCallbackJob(self.play_voice_response, greeting_message, sip_config.greeting_delay_sec).submit()
        except Exception as e:
            logger.error("Exception in onCallState. [error_type=%s, error=%s, session_id=%s]",
                        e.__class__.__name__, str(e), self.session_id, exc_info=True)

    def onCallMediaState(self, prm):
        logger.debug("onCallMediaState. [session_id=%s]", self.session_id)

    def open_call(self):
        self.aud_med = self.getAudioMedia(-1)
        self.med_port = self.create_input_port()
        self.aud_med.startTransmit(self.med_port)
        self.wav_recorder = self.create_recorder()
        self.smart_player = SmartPlayer(self.aud_med, self.wav_recorder, self.on_stop_play, session_id=self.session_id)
        self.aud_med.startTransmit(self.wav_recorder)
        self._media_refs = [self.aud_med, self.med_port, self.wav_recorder, self.smart_player]

    def close_call(self):
        if not self._media_refs:
            logger.debug("close_call: already closed. [session_id=%s]", self.session_id)
            return

        if self.smart_player:
            self.smart_player.interrupt()
        if self.aud_med and self.wav_recorder:
            try:
                self.aud_med.stopTransmit(self.wav_recorder)
            except pj.Error as e:
                logger.warning("aud_med.stopTransmit(wav_recorder) failed. [reason=%s, session_id=%s]", e.reason, self.session_id)
        if self.aud_med and self.med_port:
            try:
                self.aud_med.stopTransmit(self.med_port)
            except pj.Error as e:
                logger.warning("aud_med.stopTransmit(med_port) failed. [reason=%s, session_id=%s]", e.reason, self.session_id)

        self._media_refs.clear()

        self.aud_med = None
        self.med_port = None
        self.wav_recorder = None
        if self.smart_player:
            self.smart_player.interrupt()
            self.smart_player = None

    def create_recorder(self):
        wav_dir = Path(sip_config.sip_audio_dir)
        wav_dir.mkdir(parents=True, exist_ok=True)
        wav_name = self.session_id
        # wav_name = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
        filename = f"{wav_dir / wav_name}.wav"
        wav_recorder = RecordingPort()
        # wav_recorder = pj.AudioMediaRecorder()
        wav_recorder.create_recorder(filename)
        return wav_recorder

    async def on_stop_play(self):
        if self.state == CallState.FINISHED:
            logger.debug("on_stop_play: state is FINISHED - checking for remaining speech. [session_id=%s]", self.session_id)
            await self.hangup_if_no_active_speech()
        else:
            self.processor.start_user_silence()

    def create_input_port(self):
        fmt = pj.MediaFormatAudio()
        fmt.type = pj.PJMEDIA_TYPE_AUDIO
        fmt.clockRate = 16000
        fmt.channelCount = 1
        fmt.bitsPerSample = 16
        fmt.frameTimeUsec = sip_config.frame_time_usec
        port = AudioMediaPort(self)
        port.createPort("port/input", fmt)
        return port

    async def play_voice_response(self, message: AudioMessage, delay=0):
        blob = await message.get_blob()
        if self.start_response_generation != 0.0:
            elapsed = time.perf_counter() - self.start_response_generation
            self.app.client_response_time.observe({"method": "play_queue"}, elapsed)
            self.app.client_response_summary.observe({"method": "play_queue"}, elapsed)
            logger.debug("Response ready. [elapsed_sec=%s, session_id=%s]", elapsed, self.session_id)
            self.start_response_generation = 0.0
        filename = f"{sip_config.tmp_audio_dir}/tts-{uuid7()}.wav"
        Path(filename).parent.mkdir(parents=True, exist_ok=True)
        async with aiofiles.open(filename, 'wb') as f:
            await f.write(blob)
        if len(blob) < 364:
            logger.info("Audio too short. [blob_len=%s, session_id=%s]", len(blob), self.session_id)
            return
        if delay:
            await asyncio.sleep(delay)
        if self.smart_player:
            self.smart_player.put_to_queue(filename, True)
            self.smart_player.play()
            self.processor.reset_user_salience() # ???
            logger.debug("WAV blob sent to player. [blob_len=%s, session_id=%s]", len(blob), self.session_id)

    @staticmethod
    def remove_emojis(text):
        emoji_pattern = re.compile("["
                                   u"\U0001F600-\U0001F64F"  # emoticons
                                   u"\U0001F300-\U0001F5FF"  # symbols & pictographs
                                   u"\U0001F680-\U0001F6FF"  # transport & map symbols
                                   u"\U0001F700-\U0001F77F"  # alchemical symbols
                                   u"\U0001F780-\U0001F7FF"  # Geometric Shapes
                                   u"\U0001F800-\U0001F8FF"  # Supplemental Arrows-C
                                   u"\U0001F900-\U0001F9FF"  # Supplemental Symbols and Pictographs
                                   u"\U0001FA00-\U0001FA6F"  # Chess Symbols
                                   u"\U0001FA70-\U0001FAFF"  # Symbols and Pictographs Extended-A
                                   u"\U00002702-\U000027B0"  # Dingbats
                                   u"\U000024C2-\U0001F251"
                                   "]+", flags=re.UNICODE)
        return emoji_pattern.sub(r'', text)

    @staticmethod
    def _convert_to_np_float32(data):
        int16_data = [
            struct.unpack("<h", bytes(data[i: i + 2]))[0]
            for i in range(0, len(data), 2)
        ]
        audio_float32 = np.array(int16_data, np.float32)
        audio_float32 *= 1 / 32768
        return audio_float32

    @staticmethod
    async def local_transcribe(audio: ndarray) -> dict:
        if audio.size == 0:
            return {}
        async with aiohttp.ClientSession() as session:
            audio_bytes = io.BytesIO()
            audio_bytes.name = "audio.wav"
            scipy.io.wavfile.write(audio_bytes, 16000, audio)
            form_data = aiohttp.FormData()
            form_data.add_field('file', audio_bytes, filename='file.wav', content_type='audio/wav')
            form_data.add_field('lang', sip_config.local_stt_lang, content_type='text/plain')
            async with session.post(sip_config.local_stt_url, data=form_data) as response:
                if response.status == 200:
                    data = await response.json()
                    return data if isinstance(data, str) else '' if not isinstance(data, dict) else data.get("text")
                else:
                    raise Exception(f"Unable to post audio to server. Status code: {response.status},"
                                    f" audio.size: {audio.size}")

    async def _transcribe(self, audio: ndarray) -> dict:
        logger.debug("Transcribing audio. [time_sec=%.6f, duration_sec=%.2f, session_id=%s]",
                     self.get_current_time(), len(audio)/self.processor.sampling_rate, self.session_id)
        result = (await self.local_transcribe(audio)) if sip_config.use_local_stt else \
            (await self.transcribe_nd(audio))
        if not result:
            self.processor.track_empty_transcription()
        return result

    async def _rollback_and_speculative_generate(self, buf: ndarray):
        """Rollback any existing speculation and start a new one"""
        # First, rollback old speculation if it exists
        await self._rollback_start_task()
        # Now create the task and start new speculation
        self.tasks.create_task(TaskName.START, self._speculative_generate(buf))

    async def _speculative_generate(self, buf: ndarray):
        if buf.size == 0:
            logger.debug("Empty buffer. [time_sec=%.6f, session_id=%s]", self.get_current_time(), self.session_id)
            return

        self.set_state(CallState.SPECULATIVE_GENERATE)
        start_time = time.perf_counter()
        unstable_speech_result = await self._transcribe(buf)
        elapsed = time.perf_counter() - start_time
        self.app.client_response_time.observe({"method": "transcribe"}, elapsed)
        logger.info("Transcription completed. [text=%s, elapsed_sec=%s, session_id=%s]", unstable_speech_result, elapsed, self.session_id)

        if self.state != CallState.SPECULATIVE_GENERATE:
            logger.debug("Speculation superseded, discarding result. [time_sec=%.6f, state=%s, session_id=%s]",
                         self.get_current_time(), self.state.name, self.session_id)
            return

        if self.state == CallState.FINISHED:
            await self.hangup_if_no_active_speech()
            return

        if not unstable_speech_result:
            logger.debug("Empty transcription. [time_sec=%.6f, session_id=%s]", self.get_current_time(), self.session_id)
            return

        logger.debug("Transcription result received. [time_sec=%.6f, text=%s, session_id=%s]",
                     self.get_current_time(), unstable_speech_result, self.session_id)
        if self.unstable_speech_result_is_the_same(unstable_speech_result):
            logger.debug("Unstable speech result unchanged. [time_sec=%.6f, session_id=%s]",
                         self.get_current_time(), self.session_id)
            return

        await self._start_generate(unstable_speech_result)

    def is_active_ai_speech(self):
        return self.is_playing or self.is_player_active() or (not self.message_queue.empty() and self.ai_can_speak())

    def ai_can_speak(self):
        return self.state in (CallState.WAIT_FOR_USER, CallState.COMMIT_GENERATE, CallState.FINISHED,)

    async def hangup_if_no_active_speech(self):
        if not self.is_active_ai_speech():
            logger.debug("Queue empty and player inactive. [time_sec=%.6f, session_id=%s]",
                         self.get_current_time(), self.session_id)
            await self.soft_hangup()

    def is_player_active(self):
        return self.smart_player and self.smart_player.is_active()

    async def _start_generate(self, speech_result):
        logger.debug("Starting response generation (queue cleared). [time_sec=%.6f, text=%s, session_id=%s]",
                     self.get_current_time(), speech_result, self.session_id)
        self.clear_message_queue()
        if self.smart_player:
            self.smart_player.interrupt()
        self.unstable_speech_result = speech_result
        self.start_reply_generation = time.perf_counter()
        response = await self.start(speech_result)
        return response

    async def _commit_generate(self, buf):
        if self.state in (CallState.HANGED_UP, CallState.FINISHED):
            return
        speech_result = None
        if TaskName.START in self.tasks:
            logger.debug("Awaiting speculative transcription. [time_sec=%.6f, session_id=%s]",
                         self.get_current_time(), self.session_id)
            await self.tasks.await_and_delete(TaskName.START)

        if self.state == CallState.SPECULATIVE_GENERATE:
            logger.debug("Speculation transcription completed. [time_sec=%.6f, text=%s, session_id=%s]",
                         self.get_current_time(), self.unstable_speech_result, self.session_id)
            speech_result = self.unstable_speech_result
        else:
            logger.debug("No speculative generation task found. [time_sec=%.6f, session_id=%s]",
                         self.get_current_time(), self.session_id)
            speech_result = await self._transcribe(buf)
            await self._start_generate(speech_result)
            await self.tasks.await_and_delete(TaskName.START)

        logger.debug("Final transcription result. [time_sec=%.6f, text=%s, session_id=%s]",
                     self.get_current_time(), speech_result, self.session_id)
        if speech_result:
            logger.debug("Commit generation started. [time_sec=%.6f, text=%s, session_id=%s]",
                         self.get_current_time(), speech_result, self.session_id)
            try:
                self.set_state(CallState.COMMIT_GENERATE)
                self.start_user_speech = 0
                self.processor.long_pause_suspended = True

                play_task = asyncio.create_task(self.play_message_queue())
                result = cast(Dict, await self.commit())
                await play_task
                response: str = result.get("response")
                logger.debug("Generation response received. [time_sec=%.6f, response=%s, session_id=%s]",
                             self.get_current_time(), response, self.session_id)
                if not self.is_streaming:
                    self.message_queue.put_nowait(AudioMessage(self, response))
                self.set_state(CallState.WAIT_FOR_USER)
                await self.play_message_queue()
                if result.get("metadata", {}).get("SESSION_ENDS"):
                    logger.debug("Received SESSION_ENDS. [time_sec=%.6f, session_id=%s]",
                                 self.get_current_time(), self.session_id)
                    if self.state not in (CallState.HANGED_UP, CallState.FINISHED):
                        await self.hangup_if_no_active_speech()
                        self.set_state(CallState.FINISHED)
            except BaseException as e:
                logger.error("Commit generation failed. [time_sec=%.6f, error_type=%s, error=%s, session_id=%s]",
                             self.get_current_time(), e.__class__.__name__, str(e), self.session_id)
                self.set_state(CallState.WAIT_FOR_USER)
            finally:
                self.processor.long_pause_suspended = False
                self.unstable_speech_result = None
                _ = self.tasks.pop(TaskName.COMMIT, None)

    async def play_message_queue(self, and_text: str = None):
        if and_text:
            self.message_queue.put_nowait(AudioMessage(self, and_text))
        if not self.is_playing:
            self.is_playing = True
            try:
                if self.smart_player:
                    while not self.message_queue.empty():
                        message = self.message_queue.get_nowait()
                        await self.play_voice_response(message)
            finally:
                self.is_playing = False

    def get_current_time(self):
        current_time = time.perf_counter() - self.start_time
        return current_time

    async def handle_ws_message(self, message: Dict[str, str]):
        if message["type"] == "message":
            if self.start_reply_generation != 0.0:
                elapsed = time.perf_counter() - self.start_reply_generation
                self.app.client_response_time.observe({"method": "generate"}, elapsed)
                logger.info("Generation completed. [elapsed_sec=%s, session_id=%s]", elapsed, self.session_id)
            self.start_reply_generation = 0.0
            text = self.remove_emojis(message["message"])
            logger.debug("WS message received. [time_sec=%.6f, text=%s, session_id=%s]",
                         self.get_current_time(), text, self.session_id)
            if not text:
                return
            if self.state in (
                    CallState.COMMIT_GENERATE, CallState.WAIT_FOR_USER,
                    CallState.FINISHED) and self.start_user_speech == 0:
                logger.debug("Play queue and message. [time_sec=%.6f, session_id=%s]",
                             self.get_current_time(), self.session_id)
                await self.play_message_queue(text)
            elif self.state in (CallState.SPECULATIVE_GENERATE,):
                logger.debug("Save to queue (SPECULATIVE_GENERATE). [time_sec=%.6f, session_id=%s]",
                             self.get_current_time(), self.session_id)
                self.message_queue.put_nowait(AudioMessage(self, text))
            else:
                if self.start_user_speech == 0:
                    logger.debug("Save to queue. [time_sec=%.6f, session_id=%s]",
                                 self.get_current_time(), self.session_id)
                    self.message_queue.put_nowait(AudioMessage(self, text))
                else:
                    logger.debug("Discarded message, user speaking. [time_sec=%.6f, session_id=%s]",
                                 self.get_current_time(), self.session_id)
        elif message["type"] == "eos":
            logger.debug("End of stream received. [time_sec=%.6f, session_id=%s]", self.get_current_time(), self.session_id)
            # await self.tasks.await_and_delete(TaskName.COMMIT) ???
            if self.state == CallState.FINISHED:
                logger.debug("Current state FINISHED. [time_sec=%.6f, session_id=%s]", self.get_current_time(), self.session_id)
                await self.play_message_queue()
                await self.hangup_if_no_active_speech()
            elif self.state in (CallState.COMMIT_GENERATE, CallState.WAIT_FOR_USER):
                logger.debug("Play message queue. [time_sec=%.6f, session_id=%s]", self.get_current_time(), self.session_id)
                await self.play_message_queue()
            else:
                logger.debug("Unhandled state on EOS. [time_sec=%.6f, state=%s, session_id=%s]",
                             self.get_current_time(), self.state, self.session_id)
                # self.set_state(CallState.WAIT_FOR_USER)
                # await self.play_message_queue()
        elif message["type"] == "eoc":
            logger.debug("End Of Conversation received. [time_sec=%.6f, session_id=%s]",
                         self.get_current_time(), self.session_id)
            if os.getenv("SIP_EARLY_EOC", "false").lower() == "true" and self.state != CallState.SPECULATIVE_GENERATE:
                self.set_state(CallState.FINISHED)
                await self.play_message_queue()
                await self.hangup_if_no_active_speech()

    async def soft_hangup(self, pause=0.3):
        await asyncio.sleep(pause)
        logger.debug("Soft hangup initiated. [time_sec=%.6f, session_id=%s]", self.get_current_time(), self.session_id)
        if self.state != CallState.HANGED_UP:
            if self.to_uri:
                if not self.xfer_started:
                    self.xfer_started = True
                    logger.debug("Transferring call. [time_sec=%.6f, to_uri=%s, session_id=%s]",
                                 self.get_current_time(), self.to_uri, self.session_id)
                    self.status = "transferred"
                    if self.to_uri.startswith("dtmf:"):
                        self.dialDtmf(self.to_uri[5:])
                        await asyncio.sleep(self.transfer_delay)
                        if self.state != CallState.HANGED_UP:
                            logger.debug("Closing after transfer. [session_id=%s]", self.session_id)
                            # self.hangup(pj.CallOpParam())
                            self.send_bye_with_tag("transferred")
                            self.state = CallState.HANGED_UP
                    else:
                        self.xfer(self.to_uri, pj.CallOpParam(True))
            else:
                # self.hangup(pj.CallOpParam())
                self.send_bye_with_tag("soft_hangup")
                self.state = CallState.HANGED_UP

    def onCallTransferStatus(self, prm):
        logger.debug("Transfer status. [status_code=%s, reason=%s, final_notify=%s, session_id=%s]", prm.statusCode, prm.reason, prm.finalNotify, self.session_id)
        if prm.finalNotify:
            if 200 <= prm.statusCode < 300:
                self.send_bye_with_tag("final_notify")
                # self.hangup(pj.CallOpParam())
                self.state = CallState.HANGED_UP
            prm.cont = False

    def set_state(self, state: CallState):
        if self.state not in (CallState.HANGED_UP, CallState.FINISHED):
            logger.debug("Call state change. [time_sec=%.6f, from=%s, to=%s, session_id=%s]",
                         self.get_current_time(), self.state.name, state.name, self.session_id)
            self.state = state

    def clear_message_queue(self):
        if not self.is_playing:
            while not self.message_queue.empty():
                self.message_queue.get_nowait().task.cancel()

    async def process(self, data: bytearray):
        if not data or self.state == CallState.FINISHED:
            return
        if not sip_config.interruptions_are_allowed and (self.is_active_ai_speech() or TaskName.COMMIT in self.tasks):
            return
        float32_data = self._convert_to_np_float32(data)
        self.processor.process_audio(float32_data)

    def _on_short_pause(self, speech_buffer: np.ndarray, start: float, duration: float):
        """Callback for short pause detection - used for speculation"""
        # This is triggered when a short pause is detected, suitable for speculative generation
        if speech_buffer is None or len(speech_buffer) == 0:
            return

        logger.debug("Short pause detected. [time_sec=%.6f, start_sec=%.2f, duration_sec=%.2f, buffer_len=%s, session_id=%s]",
                     self.get_current_time(), start, duration, len(speech_buffer), self.session_id)

        # Check if there's already a COMMIT task running - if so, skip speculation
        if TaskName.COMMIT in self.tasks:
            logger.debug("Skipping speculation due to active commit task. [time_sec=%.6f, session_id=%s]",
                         self.get_current_time(), self.session_id)
            return

        if duration < 2.5:  # Skip speculation on short utterances
            logger.debug("Speech too short, waiting for long pause. [time_sec=%.6f, duration_sec=%.2f, session_id=%s]",
                         self.get_current_time(), duration, self.session_id)
            return

        # Clear message queue and start new speculation (with rollback if needed)
        self.clear_message_queue()
        self.start_response_generation = time.perf_counter()
        AsyncCallbackJob(self._rollback_and_speculative_generate, speech_buffer).submit()

    def _on_long_pause(self,  speech_buffer: np.ndarray, start: float, duration: float):
        """Callback for long pause detection - used for commit"""
        if speech_buffer is None or len(speech_buffer) == 0:
            return

        logger.debug("Long pause detected. [time_sec=%.6f, start_sec=%.2f, duration_sec=%.2f, buffer_len=%s, session_id=%s]",
                     self.get_current_time(), start, duration, len(speech_buffer), self.session_id)

        if sip_config.record_audio_parts:
            wav_dir = Path(sip_config.sip_audio_dir, self.session_id)
            wav_dir.mkdir(parents=True, exist_ok=True)
            wav_name = wav_dir/f"part-{uuid7()}.wav"
            audio_int16 = (speech_buffer * 32767).astype(np.int16)
            wavfile.write(str(wav_name), 16000, audio_int16)

        # Start commit generation
        self.tasks.create_task(TaskName.COMMIT, self._commit_generate(speech_buffer))


    async def _rollback_start_task(self):
        """Gracefully rollback the START task via the backend API"""
        if TaskName.START in self.tasks:
            logger.debug("Rolling back START task via /rollback API. [time_sec=%.6f, session_id=%s]",
                         self.get_current_time(), self.session_id)
            # Remove from local task manager first to avoid double-cancellation
            _ = self.tasks.pop(TaskName.START, None)
            try:
                # Let the server handle the cancellation and rollback
                await self.rollback()
                logger.debug("Rollback completed successfully. [time_sec=%.6f, session_id=%s]",
                             self.get_current_time(), self.session_id)
            except Exception as e:
                logger.warning("Rollback failed. [time_sec=%.6f, error=%s, session_id=%s]",
                               self.get_current_time(), e, self.session_id)

    def _on_user_speech_start(self, silence_pad_buffer: np.ndarray, start: float, duration: float):
        """Handle the start of user speech"""
        current_time = self.get_current_time()
        logger.debug("User speech started. [time_sec=%.6f, speech_start_sec=%.2f, session_id=%s]",
                     current_time, start + duration, self.session_id)
        self.start_user_speech = current_time
        # Cancel user salience timeout when user speaks
        self.processor.cancel_user_salience()
        # If the system is playing something, interrupt it
        if self.smart_player:
            self.smart_player.interrupt()
        self.clear_message_queue()
        # Gracefully rollback speculative generation instead of just cancelling
        AsyncCallbackJob(self._rollback_start_task).submit()

    def _on_user_speech_stop(self, speech_buffer: np.ndarray, start: float, duration: float):
        """Handle the end of user speech (before pause detection)"""
        current_time = self.get_current_time()
        logger.debug("User speech ended. [time_sec=%.6f, speech_end_sec=%.2f, duration_sec=%.2f, session_id=%s]",
                     current_time, start + duration, duration, self.session_id)

    def _on_user_salience_timeout(self, current_time: float):
        """Handle user salience timeout (user not speaking after bot finishes)"""
        logger.debug("User salience timeout, finishing. [time_sec=%.6f, timeout_sec=%.2f, session_id=%s]",
                     self.get_current_time(), current_time, self.session_id)

        # Set the call state to finished
        self.set_state(CallState.FINISHED)

        # Initiate a soft hangup
        # self.status = "user_timeout"  # commented until backend supports this status
        AsyncCallbackJob(self.hangup_if_no_active_speech).submit()

    async def close_session(self, status: Optional[str]=None):
        try:
            self.tasks.cancel_and_delete(TaskName.START)
            await self.tasks.await_and_delete(TaskName.COMMIT)
        except Exception as e:
            logger.error("Commit failed. [session_id=%s]", self.session_id, exc_info=e)
        await self.app.close_session(self.session_id, status=status)
