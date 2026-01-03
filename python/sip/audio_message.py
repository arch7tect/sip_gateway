import asyncio
import logging
import time

logger = logging.getLogger(__name__)

class AudioMessage:
    def __init__(self, call: 'PjCall', text: str):
        self.text = text
        self.task = asyncio.create_task(synthesize(text, call))
        self.blob = None

    async def get_blob(self):
        if self.blob is None:
            self.blob = await self.task
        return self.blob


async def synthesize(text: str, call: 'PjCall'):
    start_time = time.perf_counter()
    response = await call.session_synthesize(text)
    elapsed = time.perf_counter() - start_time
    if call.start_response_generation != 0.0:
        call.app.client_response_time.observe({"method": "synthesize"}, elapsed)
        logger.info("Synthesize finished. [text=%s, elapsed_sec=%s, session_id=%s]", text, elapsed, call.session_id)
    return response

