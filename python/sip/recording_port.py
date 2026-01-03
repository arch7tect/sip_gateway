import logging

import pjsua2 as pj

from src.integrations.sip.async_callback import AsyncCallbackJob
from src.integrations.sip.wav_writer import AsyncWavWriter

logger = logging.getLogger(__name__)

class RecordingPort(pj.AudioMediaPort):
    def __init__(self):
        super().__init__()
        self.wav_file = None

    def __del__(self):
        self.close_recorder()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close_recorder()

    def create_recorder(self, name):
        fmt = pj.MediaFormatAudio()
        fmt.type = pj.PJMEDIA_TYPE_AUDIO
        fmt.clockRate = 16000
        fmt.channelCount = 1
        fmt.bitsPerSample = 16
        fmt.frameTimeUsec = 60000
        self.createPort(f"{name}", fmt)
        self.wav_file = AsyncWavWriter(name, channels=1, sampwidth=2, framerate=16000)
        AsyncCallbackJob(self.wav_file.open).submit()

    def close_recorder(self):
        if self.wav_file:
            AsyncCallbackJob(self.wav_file.close).submit()
            self.wav_file = None

    def onFrameRequested(self, frame):
        frame.type = pj.PJMEDIA_FRAME_TYPE_AUDIO

    def onFrameReceived(self, frame):
        byte_data = bytes(frame.buf)
        if byte_data:
            AsyncCallbackJob(self.wav_file.write_chunk, byte_data).submit()
