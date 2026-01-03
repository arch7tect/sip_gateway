import json
import logging
import os
from datetime import datetime
from pathlib import Path
from typing import Dict

import dotenv

logger = logging.getLogger(__name__)


class Config(object):
    def __init__(self):
        dotenv.load_dotenv(override=True)
        self.sip_user = os.environ.get("SIP_USER", "user")
        self.sip_login = os.environ.get("SIP_LOGIN", self.sip_user)
        self.sip_domain = os.environ.get("SIP_DOMAIN", "sip.linphone.org")
        self.sip_password = os.environ.get("SIP_PASSWORD", "password")
        self.sip_caller_id = os.environ.get("SIP_CALLER_ID", None)
        self.sip_null_device = os.environ.get("SIP_NULL_DEVICE", "True").lower() == "true"
        self.tmp_audio_dir = os.environ.get("SIP_AUDIO_TMP_DIR", str(Path(os.environ.get("SIP_AUDIO_DIR", os.getcwd()))/"tmp"))
        self.sip_audio_dir = os.environ.get("SIP_AUDIO_WAV_DIR", str(Path(os.environ.get("SIP_AUDIO_DIR", os.getcwd()))/"wav"))
        self.sip_port = int(os.environ.get("SIP_PORT", "5060"))
        self.sip_use_tcp = os.environ.get("SIP_USE_TCP", "True").lower() == "true"
        self.sip_use_ice = os.environ.get("SIP_USE_ICE", "False").lower() == "true"
        self.sip_stun_servers = [s.strip() for s in os.environ.get("SIP_STUN_SERVERS", "").split(",") if
                                 not s.isspace() and s != ""]
        self.sip_proxy_servers = [s.strip() for s in os.environ.get("SIP_PROXY_SERVERS", "").split(",") if
                                  not s.isspace() and s != ""]
        self.events_delay = float(os.environ.get("EVENTS_DELAY", "0.010"))  # sec.
        self.async_delay = float(os.environ.get("ASYNC_DELAY", "0.005"))  # sec.
        self.frame_time_usec = int(os.environ.get("FRAME_TIME_USEC", "60000"))
        self.vad_model_path = str(Path(os.environ.get("VAD_MODEL_PATH", os.getcwd()))/"silero_vad.onnx")
        self.vad_model_url = os.environ.get("VAD_MODEL_URL", "https://huggingface.co/onnx-community/silero-vad/resolve/main/onnx/model.onnx")
        self.ua_zero_thread_cnt = os.environ.get("UA_ZERO_THREAD_CNT", "True").lower() == "true"
        self.ua_main_thread_only = os.environ.get("UA_MAIN_THREAD_ONLY", "True").lower() == "true"
        self.ec_tail_len = int(os.environ.get("EC_TAIL_LEN", "200"))
        self.ec_no_vad = os.environ.get("EC_NO_VAD", "False").lower() == "true"
        self.log_level = os.environ.get("LOG_LEVEL", "INFO")
        log_filename = os.environ.get("LOG_FILENAME", "")
        if log_filename:
            base_name = Path(log_filename).stem
            extension = Path(log_filename).suffix
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            self.log_filename = f"{base_name}_{timestamp}{extension}"
        if self.log_filename:
            logs_dir = os.environ.get("LOGS_DIR")
            if logs_dir:
                self.log_filename = os.path.join(logs_dir, self.log_filename)
            logger.info("PJSIP log file configured. [path=%s]", self.log_filename)
        self.pjsip_log_level = int(os.environ.get("PJSIP_LOG_LEVEL", 1))

        self.vad_sampling_rate = int(os.environ.get("VAD_SAMPLING_RATE", 16000))
        self.vad_threshold = float(os.environ.get("VAD_THRESHOLD", 0.65))
        self.vad_min_speech_duration_ms = int(os.environ.get("VAD_MIN_SPEECH_DURATION_MS", 150))
        self.vad_min_silence_duration_ms = int(os.environ.get("VAD_MIN_SILENCE_DURATION_MS", 300))
        self.vad_speech_pad_ms = int(os.environ.get("VAD_SPEECH_PAD_MS", 700))
        self.vad_speech_prob_window = int(os.environ.get("VAD_SPEECH_PROB_WINDOW", 3))
        self.vad_correction_debug = os.getenv("VAD_CORRECTION_DEBUG", "false").lower() == "true"
        self.vad_correction_enter_thres = float(os.getenv("VAD_CORRECTION_ENTER_THRESHOLD", 0.6))
        self.vad_correction_exit_thres = float(os.getenv("VAD_CORRECTION_EXIT_THRESHOLD", 0.4))

        self.short_pause_offset_ms = int(os.environ.get("SHORT_PAUSE_OFFSET_MS", 200))
        self.long_pause_offset_ms = int(os.environ.get("LONG_PAUSE_OFFSET_MS", 850))
        self.user_silence_timeout_ms = int(os.environ.get("USER_SILENCE_TIMEOUT_MS", 60000))

        self.call_connection_timeout = int(os.environ.get("CALL_CONNECTION_TIMEOUT", "10"))
        self.sip_rest_api_port = int(os.environ.get("SIP_REST_API_PORT", "8000"))
        self.base_path = str(Path(__file__).parent)

        self.use_local_stt = bool(os.environ.get("USE_LOCAL_STT", "false").lower() == "true")
        self.local_stt_url = str(os.environ.get("LOCAL_STT_URL", ""))
        self.local_stt_lang = str(os.environ.get("LOCAL_STT_LANG", "en"))

        self.greeting_delay_sec = float(os.environ.get("GREETING_DELAY_SEC", "0.0"))
        self.codecs_priority: Dict[str, int] = json.loads(os.environ.get("CODECS_PRIORITY",
                                                                         '{"opus/48000":254,"G722/16000":253}'))
        self.interruptions_are_allowed = os.environ.get("INTERRUPTIONS_ARE_ALLOWED", "true").lower() == "true"
        self.record_audio_parts = os.environ.get("RECORD_AUDIO_PARTS", "false").lower() == "true"


sip_config = Config()
