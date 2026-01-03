import asyncio
import logging

import pjsua2 as pj

logger = logging.getLogger(__name__)

# Global reference to the main event loop, set during initialization
_main_loop = None

def set_main_loop(loop):
    """Set the main event loop to use for scheduling callbacks."""
    global _main_loop
    _main_loop = loop

class AsyncCallbackJob:
    def __init__(self, cb, *args, **kwargs):
        self.cb = cb
        self.args = args
        self.kwargs = kwargs

    async def _safe_execute(self):
        try:
            await self.cb(*self.args, **self.kwargs)
        except BaseException as e:
            message = e.reason if hasattr(e, 'reason') else str(e)
            logger.error("Async callback execution failed. [callback=%s, error=%s]", getattr(self.cb, "__name__", type(self.cb).__name__), message, exc_info=e)

    def submit(self):
        # Schedule the async task directly on the event loop instead of using PJSIP's pending job system
        # This avoids thread registration issues when called from PJSIP's internal media threads
        try:
            if _main_loop is None:
                logger.error("Main event loop not set. Call set_main_loop() during initialization. [callback=%s]", getattr(self.cb, "__name__", type(self.cb).__name__))
                return
            # Use call_soon_threadsafe to safely schedule from any thread
            _main_loop.call_soon_threadsafe(lambda: asyncio.create_task(self._safe_execute()))
        except Exception as e:
            logger.error("Failed to schedule async callback. [callback=%s, error=%s]", getattr(self.cb, "__name__", type(self.cb).__name__), str(e))
