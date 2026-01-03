import logging
import os
from collections import deque
from pathlib import Path
from typing import Optional, Callable

import numpy as np
import onnxruntime

from src.integrations.sip.sip_config import sip_config
from src.integrations.sip.vad.vad_correction2 import VADCorrectionConfig, DynamicCorrection

logger = logging.getLogger(__name__)
logging.getLogger("onnxruntime").setLevel(logging.WARNING)


class VADModel:
    def __init__(
            self,
            model_path: Path,
            sampling_rate: int = 16000
    ):
        """Initialize the VAD engine."""
        self.sampling_rate = sampling_rate

        self.session = self._load_model(model_path)
        self.input_names = [input.name for input in self.session.get_inputs()]
        self.output_names = [output.name for output in self.session.get_outputs()]

    def initialize_state(self):
        silence = np.zeros((1, 512), dtype=np.float32)  # silence chunk
        inputs = {'input': silence}
        if 'sr' in self.input_names:
            inputs['sr'] = np.array(self.sampling_rate, dtype=np.int64)
        default_state = None
        if 'state' in self.input_names:
            default_state = np.zeros((2, 1, 128), dtype=np.float32)
            inputs['state'] = default_state

        try:
            outputs = self.session.run(self.output_names, inputs)
            output_dict = dict(zip(self.output_names, outputs))
            initialized_state = output_dict.get('stateN', default_state)

            logger.debug("Model state initialized. [sampling_rate=%s]", self.sampling_rate)
            return initialized_state

        except Exception as e:
            logger.warning("Failed to initialize model state. [error=%s]", e)
            return default_state

    @staticmethod
    def _load_model(model_path: Path) -> onnxruntime.InferenceSession:
        if not model_path.exists():
            raise FileNotFoundError("Model file not found: %s" % model_path)

        try:
            # Configure session for best performance
            session_options = onnxruntime.SessionOptions()
            session_options.graph_optimization_level = onnxruntime.GraphOptimizationLevel.ORT_ENABLE_ALL

            # Set optimal threads based on system
            import multiprocessing
            cpu_count = max(1, multiprocessing.cpu_count())
            session_options.intra_op_num_threads = min(4, cpu_count)
            session_options.inter_op_num_threads = 1
            session_options.enable_mem_pattern = False

            # Load model with available providers
            providers = onnxruntime.get_available_providers()
            logger.info("Available ONNX providers. [providers=%s]", providers)

            session = onnxruntime.InferenceSession(str(model_path), session_options, providers=providers)

            # Verify model inputs
            input_names = [input.name for input in session.get_inputs()]
            if 'input' not in input_names:
                raise ValueError("Model missing required 'input' node. Available: %s" % input_names)

            logger.info("Model loaded with inputs. [inputs=%s]", input_names)
            logger.info("Model outputs. [outputs=%s]", [output.name for output in session.get_outputs()])

            return session
        except Exception as e:
            logger.error("Error loading model. [error=%s]", e)
            raise

    def get_speech_prob(self, audio_chunk: np.ndarray, state: Optional[np.ndarray] = None) -> tuple:
        # Handle silent or empty chunks
        if len(audio_chunk) == 0 or np.abs(audio_chunk).max() == 0:
            return 0.0, state

        # Normalize audio to range [-1, 1]
        max_amp = np.abs(audio_chunk).max()
        if max_amp > 1.0 or max_amp < 0.01:
            audio_chunk /= max_amp

        inputs = {'input': audio_chunk.reshape(1, -1)}

        if 'sr' in self.input_names:
            inputs['sr'] = np.array(self.sampling_rate, dtype=np.int64)

        if 'state' in self.input_names:
            inputs['state'] = state if state is not None else np.zeros((2, 1, 128), dtype=np.float32)

        try:
            outputs = self.session.run(self.output_names, inputs)
            output_dict = dict(zip(self.output_names, outputs))

            prob = float(output_dict['output'][0])
            new_state = output_dict.get('stateN', None)

            return prob, new_state

        except Exception as e:
            logger.error("Inference error. [error=%s]", e)
            return 0.0, state


class StreamingVADProcessor:
    def __init__(
            self,
            vad_engine: VADModel,
            threshold: float = 0.8,
            min_speech_duration_ms: int = 150,
            min_silence_duration_ms: int = 300,
            speech_pad_ms: int = 500,
            short_pause_ms: int = 0,
            long_pause_ms: int = 0,
            user_silence_duration_ms: int = 30000,
            window_size_samples: int = 512,
            speech_prob_window: int = 3,
            on_speech_start: Optional[Callable[[np.ndarray, float, float], None]] = None,
            on_speech_end: Optional[Callable[[np.ndarray, float, float], None]] = None,
            on_short_pause: Optional[Callable[[np.ndarray, float, float], None]] = None,
            on_long_pause: Optional[Callable[[np.ndarray, float, float], None]] = None,
            on_user_salience_timeout: Optional[Callable[[float], None]] = None
    ):
        # Engine and parameters
        self.vad_engine = vad_engine
        self.threshold = threshold
        self.window_size_samples = window_size_samples
        self.sampling_rate = vad_engine.sampling_rate
        self.speech_prob_window = max(1, speech_prob_window)  # Ensure at least 1

        # Convert time parameters to samples
        self.min_speech_samples = self.sampling_rate * min_speech_duration_ms // 1000
        self.min_silence_samples = self.sampling_rate * min_silence_duration_ms // 1000
        self.speech_pad_samples = self.sampling_rate * speech_pad_ms // 1000
        self.short_pause_samples = self.min_silence_samples + self.sampling_rate * short_pause_ms // 1000
        self.long_pause_samples = self.short_pause_samples + self.sampling_rate * long_pause_ms // 1000
        self.user_silence_duration_samples = self.sampling_rate * user_silence_duration_ms // 1000

        # Buffers
        self.buffer = np.array([], dtype=np.float32)
        self.speech_buffer = np.array([], dtype=np.float32)
        self.silence_buffer = np.array([], dtype=np.float32)
        self.silence_pad_buffer = np.array([], dtype=np.float32)
        self.max_silence_buffer_duration_ms = max(speech_pad_ms * 2, min_silence_duration_ms)
        self.max_silence_samples = self.sampling_rate * self.max_silence_buffer_duration_ms // 1000
        self.prob_history = deque(maxlen=self.speech_prob_window)

        # Detection state
        self.current_sample = 0
        self.active_speech = False
        self.active_long_speech = False
        self.short_pause_fired = False
        self.long_pause_suspended = False
        self.speech_start = 0
        self.user_silence_start = 0
        self.user_silence_timeout_fired = False

        # Store callbacks
        self.on_speech_start = on_speech_start
        self.on_speech_end = on_speech_end
        self.on_short_pause = on_short_pause
        self.on_long_pause = on_long_pause
        self.on_user_salience_timeout = on_user_salience_timeout

        # Initialize ONNX session state for consistent inference
        self.state = self.vad_engine.initialize_state()

        self.use_dynamic_corrections = os.getenv("VAD_USE_DYNAMIC_CORRECTIONS", "true").lower() == "true"
        cfg = VADCorrectionConfig()
        cfg.debug = sip_config.vad_correction_debug
        cfg.enter_thres = sip_config.vad_correction_enter_thres
        cfg.exit_thres = sip_config.vad_correction_exit_thres
        self.dc = DynamicCorrection(cfg)

    def times_sec(self, audio_chunk: np.ndarray) -> dict:
        return {
            "start": (self.current_sample - len(audio_chunk)) / self.sampling_rate,
            "duration": len(audio_chunk) / self.sampling_rate,
        }

    def current_time_sec(self):
        return self.current_sample / self.sampling_rate

    def _get_smoothed_prob(self, audio_chunk: np.ndarray) -> float:
        new_prob, self.state = self.vad_engine.get_speech_prob(audio_chunk.copy(), self.state)
        self.prob_history.append(new_prob)

        if len(self.prob_history) > 1:
            weights = list(range(1, len(self.prob_history) + 1))
            weighted_sum = sum(p * w for p, w in zip(self.prob_history, weights))
            return weighted_sum / sum(weights)

        return new_prob

    def process_audio(self, audio_chunk: np.ndarray) -> None:
        if audio_chunk.dtype != np.float32:
            raise ValueError(f"Audio chunk must be of type float32. Got {audio_chunk.dtype}")

        self.buffer = np.concatenate([self.buffer, audio_chunk])
        while len(self.buffer) >= self.window_size_samples:
            chunk = self.buffer[:self.window_size_samples]
            self.buffer = self.buffer[self.window_size_samples:]
            self._process_audio_window(chunk)

    def finalize(self):
        if len(self.speech_buffer) >= self.min_speech_samples:
            self._fire_long_pause()

    def _process_audio_window(self, window: np.ndarray) -> None:
        # Get speech probability using original method
        speech_prob = self._get_smoothed_prob(window)
        is_basic_speech = speech_prob > self.threshold
        if self.use_dynamic_corrections:
            frame_energy = float(np.sqrt(np.mean(np.square(window))))
            is_speech_frame = self.dc.process_frame(speech_prob, frame_energy)
        else:
            is_speech_frame = is_basic_speech

        self.current_sample += len(window)

        if self.active_long_speech:
            self.speech_buffer = np.concatenate([self.speech_buffer, window])
            if is_speech_frame:
                if len(self.silence_buffer) > 0:
                    self.silence_buffer = np.array([], dtype=np.float32)
            else:
                self._grow_silence_buffer(window)
        else:
            if is_speech_frame:
                self.speech_buffer = np.concatenate([self.speech_buffer, window])
            else:
                if len(self.speech_buffer) > 0:
                    self._grow_silence_buffer(self.speech_buffer)
                    self.speech_buffer = np.array([], dtype=np.float32)
                self._grow_silence_buffer(window)

        if is_speech_frame:
            if not self.active_speech:
                self.speech_start = self.current_sample - len(window)
                if len(self.speech_buffer) >= self.min_speech_samples:
                    self._fire_speech_start()
        else: # silence frame
            if self.active_speech:
                if len(self.silence_buffer) >= self.min_silence_samples:
                    self._fire_speech_end()
            else:
                if not self.user_silence_timeout_fired and self.current_sample - self.user_silence_start > self.user_silence_duration_samples:
                    self._fire_user_silence_timeout()
            if self.active_long_speech:
                if not self.short_pause_fired and len(self.silence_buffer) >= self.short_pause_samples:
                    self._fire_short_pause()
                if not self.long_pause_suspended and len(self.silence_buffer) >= self.long_pause_samples:
                    self._fire_long_pause()

    def start_user_silence(self) -> None:
        self.user_silence_start = self.current_sample
        self.user_silence_timeout_fired = False
        self.dc.start_early_detection()
        current_time_ms = self.current_sample / self.sampling_rate * 1000
        logger.debug("User salience period started. [time_sec=%.2f]", current_time_ms / 1000)

    def reset_user_salience(self) -> None:
        """Disable user salience timeout (prevents it from firing)"""
        self.user_silence_start = 0
        self.user_silence_timeout_fired = True

    def cancel_user_salience(self) -> None:
        """Cancel user salience timeout (when user speaks during timeout period)"""
        self.user_silence_start = 0
        # Don't change user_silence_timeout_fired - let it keep its state
        logger.debug("User salience timeout cancelled. [time_sec=%.2f]", self.current_time_sec())

    def _grow_silence_buffer(self, window):
        self.silence_buffer = np.concatenate([self.silence_buffer, window])
        if len(self.silence_buffer) > self.max_silence_samples:
            self.silence_buffer = self.silence_buffer[-self.max_silence_samples:]

    def _fire_speech_end(self):
        self.active_speech = False
        if not self.active_long_speech:
            self.speech_buffer = np.array([], dtype=np.float32)
        self.short_pause_fired = False
        self.user_silence_start = self.current_sample - len(self.silence_buffer)
        self.user_silence_timeout_fired = False
        start_offset = self.speech_start - self.current_sample
        end_offset = -len(self.silence_buffer)
        buffer = self.speech_buffer[start_offset:end_offset]
        try:
            self.handle_speech_end(buffer, **self.times_sec(buffer))
        except Exception as e:
            logger.error("Error firing speech end. [error=%s]", e)

    def _fire_speech_start(self):
        self.active_speech = True
        if not self.active_long_speech:
            self.active_long_speech = True
            start_padding = min(self.speech_pad_samples, len(self.silence_buffer))
            self.silence_pad_buffer = self._apply_fade(self.silence_buffer[-start_padding:])
        self.silence_buffer = np.array([], dtype=np.float32)
        try:
            self.handle_speech_start(self.silence_pad_buffer, **self.times_sec(self.silence_pad_buffer))
        except Exception as e:
            logger.error("Error firing speech end. [error=%s]", e)

    def _fire_short_pause(self):
        silence_length = len(self.silence_buffer)
        silence_postfix = self._apply_fade(self.silence_buffer, False)
        buffer = np.concatenate([self.silence_pad_buffer, self.speech_buffer[:-silence_length], silence_postfix])
        try:
            self.handle_short_pause(buffer, **self.times_sec(buffer))
        except Exception as e:
            logger.error("Error firing short pause. [error=%s]", e)
        self.short_pause_fired = True

    def _fire_long_pause(self):
        silence_length = len(self.silence_buffer)
        silence_postfix = self._apply_fade(self.silence_buffer, False)
        buffer = np.concatenate([self.silence_pad_buffer, self.speech_buffer[:-silence_length], silence_postfix])
        try:
            self.handle_long_pause(buffer, **self.times_sec(buffer))
        except Exception as e:
            logger.error("Error firing long pause. [error=%s]", e)
        self.short_pause_fired = False
        self.active_long_speech = False
        # self.silence_start = self.current_sample - len(self.silence_buffer)
        self.speech_buffer = np.array([], dtype=np.float32)

    def _fire_user_silence_timeout(self):
        try:
            self.handle_user_silence_timeout(self.current_time_sec())
        except Exception as e:
            logger.error("Error firing user silence timeout. [error=%s]", e)
        self.user_silence_timeout_fired = True

    @staticmethod
    def _apply_fade(audio: np.ndarray,
                    fade_in: bool = True, curve: str = 'cosine') -> np.ndarray:
        fade_length = len(audio)
        if fade_length <= 1:
            return audio
        result = audio.copy()

        # Create fade curve based on selected type
        if curve == 'linear':
            fade_curve = np.linspace(0.0, 1.0, fade_length)
        elif curve == 'cosine':
            fade_curve = np.sin(np.linspace(0, np.pi / 2, fade_length))
        elif curve == 'exponential':
            fade_curve = np.exp(np.linspace(-4, 0, fade_length)) - np.exp(-4)
            fade_curve = fade_curve / fade_curve[-1]  # Normalize to 0-1 range
        elif curve == 'log':
            fade_curve = np.log(np.linspace(0.1, 1, fade_length) * 9 + 1)
            fade_curve = fade_curve / fade_curve[-1]  # Normalize to 0-1 range
        else:
            raise ValueError(f"Wrong curve {curve}")

        if not fade_in:
            fade_curve = fade_curve[::-1]
        result[:fade_length] *= fade_curve
        return result

    def handle_speech_start(self, silence_pad_buffer: np.ndarray, start: float, duration: float):
        logger.debug("Speech start detected. [current_time_sec=%.2f, start_sec=%.2f, duration_sec=%.2f]",
                     self.current_time_sec(), start, duration)
        if self.on_speech_start is not None:
            self.on_speech_start(silence_pad_buffer, start, duration)

    def handle_speech_end(self, speech_buffer: np.ndarray, start: float, duration: float):
        logger.debug("Speech end detected. [current_time_sec=%.2f, start_sec=%.2f, duration_sec=%.2f]",
                     self.current_time_sec(), start, duration)
        if self.on_speech_end is not None:
            self.on_speech_end(speech_buffer, start, duration)

    def handle_short_pause(self, speech_buffer: np.ndarray, start: float, duration: float):
        logger.debug("Short pause detected. [current_time_sec=%.2f, start_sec=%.2f, duration_sec=%.2f]",
                     self.current_time_sec(), start, duration)
        if self.on_short_pause is not None:
            self.on_short_pause(speech_buffer, start, duration)

    def handle_long_pause(self, speech_buffer: np.ndarray, start: float, duration: float):
        logger.debug("Long pause detected. [current_time_sec=%.2f, start_sec=%.2f, duration_sec=%.2f]",
                     self.current_time_sec(), start, duration)
        if self.on_long_pause is not None:
            self.on_long_pause(speech_buffer, start, duration)

    def handle_user_silence_timeout(self, current_time: float):
        logger.debug("User silence timeout. [time_sec=%.2f]", current_time)
        if self.on_user_salience_timeout is not None:
            self.on_user_salience_timeout(current_time)

    def track_empty_transcription(self):
        pass
