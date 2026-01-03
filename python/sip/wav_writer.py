import asyncio
import logging
import os
import struct
from typing import List

import aiofiles

logger = logging.getLogger(__name__)


class AsyncWavWriter:
    """
    Asynchronous WAV file writer that enables writing audio data chunks incrementally.

    This class handles proper WAV header creation and updating, supporting various
    audio configurations.
    """

    # WAV format constants
    PCM_FORMAT = 1

    # Header offsets
    RIFF_SIZE_OFFSET = 4
    DATA_SIZE_OFFSET = 40
    HEADER_SIZE = 44  # Total size of the WAV header

    def __init__(self, filename: str, channels: int, sampwidth: int, framerate: int):
        """
        Initialize the WAV writer.

        Args:
            filename: Output WAV file path
            channels: Number of audio channels (1=mono, 2=stereo)
            sampwidth: Sample width in bytes (1, 2, or 4)
            framerate: Audio sample rate in Hz

        Raises:
            ValueError: If invalid audio parameters are provided
        """
        # Validate parameters
        if channels <= 0:
            raise ValueError("Number of channels must be positive")
        if sampwidth not in (1, 2, 4):
            raise ValueError("Sample width must be 1, 2, or 4 bytes")
        if framerate <= 0:
            raise ValueError("Frame rate must be positive")

        self.filename = filename
        self.channels = channels
        self.sampwidth = sampwidth
        self.framerate = framerate
        self.data_size = 0
        self._file = None
        self._data_written = False

        # Calculate derived values
        self.byte_rate = self.framerate * self.channels * self.sampwidth
        self.block_align = self.channels * self.sampwidth
        self.bits_per_sample = self.sampwidth * 8

    async def __aenter__(self):
        """Async context manager entry."""
        await self.open()
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """Async context manager exit."""
        await self.close()

    async def open(self):
        """Open the WAV file and write initial header."""
        if self._file is None:
            logger.debug("Opening WAV file for writing. [filename=%s]", self.filename)
            self._file = await aiofiles.open(self.filename, 'wb')
            await self._write_wav_header()

    async def write_chunk(self, chunk: bytes):
        """
        Write an audio data chunk to the WAV file.

        Args:
            chunk: Audio data bytes to write
        """
        if not chunk:
            logger.warning("Attempt to write empty chunk. [filename=%s]", self.filename)
            return

        if self._file is None:
            raise ValueError("Cannot write to file before opening")

        # Track current position to verify writes
        current_pos = await self._file.tell()
        expected_pos = current_pos + len(chunk)

        # Write the chunk data
        await self._file.write(chunk)
        self.data_size += len(chunk)
        self._data_written = True

        # Verify position after write
        new_pos = await self._file.tell()
        if new_pos != expected_pos:
            logger.warning(
                "File position mismatch after write. [expected=%s, actual=%s, filename=%s]",
                expected_pos, new_pos, self.filename
            )

    async def write_chunks(self, chunks: List[bytes]):
        """
        Write multiple audio data chunks efficiently.

        Args:
            chunks: List of audio data byte chunks
        """
        if self._file is None:
            raise ValueError("Cannot write to file before opening")

        for chunk in chunks:
            await self.write_chunk(chunk)

    async def close(self):
        if self._file:
            logger.debug("Flushing file. [filename=%s]", self.filename)
            await self._file.flush()

            if self.data_size > 0:
                # Correct header safely in place
                async with aiofiles.open(self.filename, 'r+b') as f:
                    riff_chunk_size = 36 + self.data_size
                    await f.seek(self.RIFF_SIZE_OFFSET)
                    await f.write(struct.pack('<I', riff_chunk_size))
                    await f.seek(self.DATA_SIZE_OFFSET)
                    await f.write(struct.pack('<I', self.data_size))
                    await f.flush()

            logger.debug("Closing file. [filename=%s]", self.filename)
            await self._file.close()
            self._file = None

    async def _write_wav_header(self):
        """Write the initial WAV header with placeholder sizes."""
        # Initial size will be updated later
        riff_chunk_size = 36  # Placeholder, will be updated on close
        fmt_chunk_size = 16  # Size of the format subchunk for PCM

        header = (
                b'RIFF' +
                struct.pack('<I', riff_chunk_size) +
                b'WAVE' +
                b'fmt ' +
                struct.pack('<IHHIIHH',
                            fmt_chunk_size,  # Subchunk1Size (PCM)
                            self.PCM_FORMAT,  # AudioFormat (1=PCM)
                            self.channels,  # NumChannels
                            self.framerate,  # SampleRate
                            self.byte_rate,  # ByteRate
                            self.block_align,  # BlockAlign
                            self.bits_per_sample  # BitsPerSample
                            ) +
                b'data' +
                struct.pack('<I', 0)  # Data size placeholder
        )
        await self._file.write(header)
        await self._file.flush()  # Ensure header is written

# Example usage with context manager:
async def main():
    # Configure basic logging
    logging.basicConfig(
        level=logging.DEBUG,
        format='%(asctime)s %(name)s %(levelname)s %(message)s'
    )

    # Using the writer as a context manager ensures proper cleanup
    async with AsyncWavWriter('output.wav', channels=2, sampwidth=2, framerate=44100) as writer:
        # Simulate incoming chunks asynchronously
        chunks = [b'\x10' * 1024 * 10, b'\x30' * 2048 * 10, b'\x60' * 4096 * 100]
        for i, chunk in enumerate(chunks):
            await asyncio.sleep(0.1)  # Simulate delay in receiving chunks
            await writer.write_chunk(chunk)
            logger.debug("Wrote chunk. [chunk=%s/%s, size=%s]", i + 1, len(chunks), len(chunk))

    # Verify the file was created with the correct size
    file_size = os.path.getsize('output.wav')
    expected_size = 44 + sum(len(chunk) for chunk in chunks)
    logger.info("File created. [size=%s, expected=%s]", file_size, expected_size)


if __name__ == '__main__':
    asyncio.run(main())