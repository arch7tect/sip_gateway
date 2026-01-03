import statistics
from collections import deque
from dataclasses import dataclass, field
from typing import Deque, Tuple, List
import logging

__all__ = ["VADCorrectionConfig", "DynamicCorrection"]

logger = logging.getLogger(__name__)

@dataclass
class VADCorrectionConfig:
    """Configuration for foreground speech detection."""

    # Rolling‑window lengths
    score_window: int = 5  # было 3
    prob_window: int = 15  # было 10

    # Hysteresis thresholds
    enter_thres: float = 0.40  # было 0.60
    exit_thres: float = 0.25   # было 0.40

    # Early detection thresholds (more aggressive for first ~5 seconds)
    early_enter_thres: float = 0.30  # было 0.45
    early_phase_frames: int = 200    # было 156 (увеличиваем до ~6.4 сек)
    early_prob_boost: float = 0.20   # было 0.15

    # Feature weights
    w_prob: float = 0.60  # было 0.40
    w_snr: float = 0.15   # было 0.30
    w_var: float = 0.05   # оставляем как есть
    w_energy: float = 0.20 # оставляем как есть

    # Foreground detection parameters
    speech_prob_threshold: float = 0.3  # Minimum prob to consider "speech"
    # min_speech_frames: int = 5  # Minimum frames needed for variance calc
    min_speech_frames: int = 3  # было 5

    transition_threshold: float = 0.4  # Prob range indicating transition

    # Normalisation ranges
    snr_clip: Tuple[float, float] = (0.0, 20.0)
    var_clip: Tuple[float, float] = (0.0, 0.05)

    # Energy profile adaptation
    # noise_alpha: float = 0.01
    noise_alpha: float = 0.02  # было 0.01
    peak_decay: float = 0.05

    # Fast initial adaptation
    # initial_noise_alpha: float = 0.1  # Faster noise adaptation in first frames
    initial_noise_alpha: float = 0.15  # было 0.1
    initial_adapt_frames: int = 50  # Use faster adaptation for first ~1 second

    debug: bool = False
    dbg_fmt: str = field(init=False, repr=False,
                         default="{f:05d}  p={p:.2f}  E={eng:.2f}  SNR={snr:.1f}  VAR={var:.3f}  FG={fg:.3f}  s={score:.2f}  → {state}")


class DynamicCorrection:
    """VAD with foreground/background speech distinction and improved early detection."""

    def __init__(self, cfg: VADCorrectionConfig | None = None):
        self.cfg = cfg or VADCorrectionConfig()

        # Rolling buffers
        self._score_buf: Deque[float] = deque(maxlen=self.cfg.score_window)
        self._prob_buf: Deque[float] = deque(maxlen=self.cfg.prob_window)

        # Energy tracking with better initial estimates
        self._noise_energy: float = 0.01  # Start lower for better sensitivity
        self._peak_energy: float = 0.1
        self._initial_energy_samples: List[float] = []  # Collect samples for better initial estimate

        # State
        self._state: bool = False
        self.frame_index: int = 0

        # Early detection state
        self._in_early_phase: bool = False
        self._early_phase_start_frame: int = -1

    def start_early_detection(self) -> None:
        """Call this when agent finishes speaking and we expect user input."""
        if self._early_phase_start_frame == -1:
            self._in_early_phase = True
            self._early_phase_start_frame = self.frame_index

    @staticmethod
    def _clip_norm(x: float, lo: float, hi: float) -> float:
        """Clip and normalize to [0, 1]."""
        if hi <= lo:
            return 0.0
        x = max(lo, min(x, hi))
        return (x - lo) / (hi - lo)

    def _update_energy_profile(self, eng: float, current_speech_prob: float) -> None:
        """Update adaptive energy profile with improved initial adaptation."""
        cfg = self.cfg

        # Collect initial samples for better noise estimation
        if len(self._initial_energy_samples) < cfg.initial_adapt_frames:
            self._initial_energy_samples.append(eng)
            if len(self._initial_energy_samples) == cfg.initial_adapt_frames:
                # Set initial noise to 10th percentile of first samples
                self._noise_energy = sorted(self._initial_energy_samples)[len(self._initial_energy_samples) // 10]

        # Use faster adaptation during initial phase
        alpha = cfg.initial_noise_alpha if self.frame_index < cfg.initial_adapt_frames else cfg.noise_alpha

        if not self._state and current_speech_prob < 0.3:
            self._noise_energy = (1.0 - alpha) * self._noise_energy + alpha * eng

        if eng > self._peak_energy:
            self._peak_energy = eng
        else:
            self._peak_energy = (1.0 - cfg.peak_decay) * self._peak_energy + cfg.peak_decay * self._noise_energy

        self._peak_energy = max(self._peak_energy, self._noise_energy + 1e-6)

    def _is_transition_period(self) -> bool:
        """Detect speech transition periods."""
        if len(self._prob_buf) < 4:
            return False

        recent = list(self._prob_buf)[-4:]
        prob_range = max(recent) - min(recent)
        return prob_range > self.cfg.transition_threshold

    def _calculate_foreground_variance(self) -> Tuple[float, float]:
        """
        Calculate variance that represents genuine speech dynamics.
        Returns: (raw_variance, foreground_variance)
        """
        if len(self._prob_buf) < 2:
            return 0.0, 0.0

        # Calculate raw variance (for comparison)
        raw_variance = statistics.pvariance(self._prob_buf)

        # Get recent probabilities for foreground analysis
        recent_probs = list(self._prob_buf)

        # Strategy 1: State-conditional variance calculation
        if self._state:  # Currently detecting speech
            # Only use speech-level probabilities for variance
            speech_probs = [p for p in recent_probs if p > self.cfg.speech_prob_threshold]

            if len(speech_probs) >= self.cfg.min_speech_frames:
                # Calculate variance only from speech frames
                foreground_var = statistics.pvariance(speech_probs)

                # Additional check: avoid transition contamination
                if self._is_transition_period():
                    # During transitions, use a more conservative approach
                    # Look at the most recent speech frames only
                    recent_speech = [p for p in recent_probs[-6:] if p > self.cfg.speech_prob_threshold]
                    if len(recent_speech) >= 3:
                        foreground_var = statistics.pvariance(recent_speech)
                    else:
                        foreground_var = 0.0  # Insufficient clean speech data

                return raw_variance, foreground_var
            else:
                # Not enough speech frames for reliable variance
                return raw_variance, 0.0
        else:
            # During silence, no foreground speech activity
            return raw_variance, 0.0

    def _apply_early_detection_boost(self, speech_prob: float) -> float:
        """Apply early detection boost during initial conversation phase."""
        if not self._in_early_phase:
            return speech_prob

        # Boost speech probability during early phase
        boosted_prob = speech_prob + self.cfg.early_prob_boost
        return min(boosted_prob, 1.0)

    def _get_dynamic_threshold(self) -> float:
        """Get threshold based on conversation phase and detection history."""
        cfg = self.cfg

        # Use lower threshold during early phase
        if self._in_early_phase:
            return cfg.early_enter_thres

        # Fallback to standard threshold
        return cfg.enter_thres

    def process_frame(self, speech_prob: float, frame_energy: float) -> bool:
        """Process frame and return speech detection result."""

        # Update energy profile
        self._update_energy_profile(frame_energy, speech_prob)

        # Apply early detection boost if in early phase
        adjusted_prob = self._apply_early_detection_boost(speech_prob)

        # Calculate features
        snr = frame_energy / (self._noise_energy + 1e-6)
        snr_n = self._clip_norm(snr, *self.cfg.snr_clip)

        self._prob_buf.append(adjusted_prob)

        # Calculate foreground-aware variance
        raw_var, foreground_var = self._calculate_foreground_variance()
        var_n = self._clip_norm(raw_var, *self.cfg.var_clip)
        fg_var_n = self._clip_norm(foreground_var, *self.cfg.var_clip)

        # Energy normalization with better handling for early frames
        if self._peak_energy > self._noise_energy:
            eng_n = (frame_energy - self._noise_energy) / (self._peak_energy - self._noise_energy + 1e-6)
        else:
            eng_n = 0.5 if frame_energy > self._noise_energy else 0.0
        eng_n = min(max(eng_n, 0.0), 1.0)

        # Calculate score using foreground variance
        cfg = self.cfg
        score = (
                cfg.w_prob * adjusted_prob +
                cfg.w_snr * snr_n +
                cfg.w_var * fg_var_n +  # Use foreground variance instead of raw
                cfg.w_energy * eng_n
        )

        weight_sum = cfg.w_prob + cfg.w_snr + cfg.w_var + cfg.w_energy
        score /= weight_sum if weight_sum else 1.0

        # Update rolling score and state with dynamic threshold
        self._score_buf.append(score)
        mean_score = sum(self._score_buf) / len(self._score_buf)

        dynamic_enter_threshold = self._get_dynamic_threshold()

        if not self._state and mean_score >= dynamic_enter_threshold:
            self._state = True
        elif self._state and mean_score <= cfg.exit_thres:
            self._state = False

        if self._in_early_phase:
            if self._state:
                self._in_early_phase = False
            elif (self._early_phase_start_frame >= 0 and
                  self.frame_index >= self._early_phase_start_frame + self.cfg.early_phase_frames):
                self._in_early_phase = False

        # Debug output
        if cfg.debug:
            early_indicator = " [EARLY]" if self._in_early_phase else ""
            debug_message = cfg.dbg_fmt.format(
                f=self.frame_index,
                p=speech_prob,  # Show original prob in debug
                eng=eng_n,
                snr=snr_n,
                var=var_n,
                fg=fg_var_n,
                score=mean_score,
                state="SPEECH" if self._state else "SILENCE",
            ) + early_indicator
            logger.debug(debug_message)

        self.frame_index += 1
        return self._state

    @property
    def is_speech(self) -> bool:
        """Current speech detection state."""
        return self._state

