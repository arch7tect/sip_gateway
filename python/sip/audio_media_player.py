import logging
import os
from collections import deque
from contextlib import suppress
from typing import Any, Optional

import pjsua2 as pj

from src.integrations.sip.async_callback import AsyncCallbackJob

logger = logging.getLogger(__name__)


class AudioMediaPlayer(pj.AudioMediaPlayer):
    def __init__(self, smart_player: "SmartPlayer"):
        super().__init__()
        self.smart_player = smart_player

    def onEof2(self):
        # Run asynchronously to avoid blocking PJSIP thread
        AsyncCallbackJob(self.smart_player.on_eof).submit()


class AudioFile:
    __slots__ = ("filename", "discard")
    def __init__(self, filename: str, discard: bool):
        self.filename = filename
        self.discard = discard


class SmartPlayer:
    def __init__(self, aud_med: pj.AudioMedia, wav_recorder: Any, on_stop_cb=None, session_id: Optional[str] = None):
        self.current_audio: Optional[AudioFile] = None
        self.deque = deque()
        self.aud_med: Optional[pj.AudioMedia] = aud_med
        self.player: Optional[AudioMediaPlayer] = None
        self.wav_recorder: Optional[pj.AudioMediaRecorder] = wav_recorder
        self.on_stop_cb = on_stop_cb
        self._tearing_down = False  # prevents double-destroy
        self.session_id = session_id

    def is_active(self) -> bool:
        return self.current_audio is not None or bool(self.deque)

    def put_to_queue(self, filename: str, discard: bool = False):
        self.deque.append(AudioFile(filename, discard))

    def play(self):
        if not self.current_audio and self.deque:
            self._play_next()

    def interrupt(self):
        logger.debug("Interrupting queue. [session_id=%s]", self.session_id)
        self._tearing_down = True
        try:
            if self.player:
                self.destroy_player()
            self._discard_current()
            while self.deque:
                audio = self.deque.popleft()
                if audio.discard:
                    with suppress(FileNotFoundError):
                        os.remove(audio.filename)
        finally:
            self._tearing_down = False

    def destroy_player(self):
        pl = self.player
        if not pl:
            return
        self.player = None
        if self.aud_med is not None:
            with suppress(pj.Error):
                pl.stopTransmit(self.aud_med)
        if self.wav_recorder is not None:
            with suppress(pj.Error):
                pl.stopTransmit(self.wav_recorder)

    def _play_next(self):
        assert self.deque
        self.current_audio = self.deque.popleft()
        af = self.current_audio

        if self._tearing_down or self.aud_med is None or self.wav_recorder is None:
            logger.debug("Skip _play_next during teardown or missing media. [session_id=%s]", self.session_id)
            return

        try:
            self.player = AudioMediaPlayer(self)
            self.player.createPlayer(af.filename, pj.PJMEDIA_FILE_NO_LOOP)
            self.player.startTransmit(self.wav_recorder)
            self.player.startTransmit(self.aud_med)
        except pj.Error as e:
            logger.debug("Player start failed during teardown. [error=%s, session_id=%s]", getattr(e, "reason", e), self.session_id)
            self.player = None
            self._discard_current()
            if self.deque:
                self._play_next()

    async def on_eof(self):
        ca = self.current_audio
        logger.debug("on_eof. [filename=%s, session_id=%s]", getattr(ca, "filename", None), self.session_id)
        self.destroy_player()
        self._discard_current()
        if self.deque and not self._tearing_down:
            self._play_next()
        elif self.on_stop_cb and not self._tearing_down:
            # on_stop_cb may hangup; let it run after we fully released media
            await self.on_stop_cb()

    def _discard_current(self):
        if not self.current_audio:
            return
        if self.current_audio.discard:
            with suppress(FileNotFoundError):
                logger.debug("Discarding audio file. [filename=%s, session_id=%s]", self.current_audio.filename, self.session_id)
                os.remove(self.current_audio.filename)
        self.current_audio = None
