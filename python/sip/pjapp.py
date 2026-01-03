import asyncio
import logging
import os
import signal
import urllib.request
from pathlib import Path
from typing import Dict, Optional

import pjsua2 as pj
from aiohttp import web
from aiohttp_swagger import setup_swagger
from aioprometheus import Registry, Counter, Histogram, Summary, render

from src.backend.client_bot_base import ClientBotBase
from src.integrations.sip.pjaccount import PjAccount
from src.integrations.sip.pjcall import PjCall
from src.integrations.sip.sip_config import sip_config
from src.integrations.sip.sip_mixin import SipAppMixin
from src.integrations.sip.vad.processor6 import VADModel

logger = logging.getLogger(__name__)

class PjApp(ClientBotBase, SipAppMixin):
    def __init__(self, base_url: str, *args, **kwargs):
        ClientBotBase.__init__(self, base_url)
        SipAppMixin.__init__(self, *args, **kwargs)
        self.app.add_routes([
            web.get('/health', self.health),
        ])
        self.acc: PjAccount | None = None
        self.ep_cfg: pj.EpConfig | None = None
        self.quiting = False
        self.ep: pj.Endpoint | None = None
        if not Path(sip_config.vad_model_path).is_file():
            urllib.request.urlretrieve(sip_config.vad_model_url, sip_config.vad_model_path)
        self.vad_engine = VADModel(model_path=Path(sip_config.vad_model_path), sampling_rate=sip_config.vad_sampling_rate)

        self.prometheus_registry = Registry()
        self.client_request_counter: Counter = Counter(
            "client_requests_total",
            "Total number of client requests"
        )
        self.client_response_summary: Summary = Summary(
            "client_response_summary",
            "Time elapsed for response"
        )
        self.client_response_time: Histogram = Histogram(
            "response_time_milliseconds",
            "Response time in milliseconds"
        )
        self.prometheus_registry.register(self.client_request_counter)
        self.prometheus_registry.register(self.client_response_time)
        self.prometheus_registry.register(self.client_response_summary)
        self.app.add_routes([
            web.get('/metrics', self.metrics),
        ])

    async def metrics(self, request: web.Request):
        accept_header = request.headers.get("Accept", "")
        accepts = [ct.strip() for ct in accept_header.split(",")] if accept_header else []
        content, http_headers = render(self.prometheus_registry, accepts)
        return web.Response(body=content, headers=http_headers)

    async def on_startup(self, app):
        logger.info("SIP app startup. [app=%s, base_url=%s]", self.get_app_name(), self.get_base_url())

    def create_session(self, user_id, name, bot_type, conversation_id: Optional[str]=None, *args, **kwargs):
        return PjCall(self, user_id, name, conversation_id, kwargs.pop('call_id', pj.PJSUA_INVALID_ID))

    async def ws_message(self, session: 'PjCall', message: Dict[str, str]):
        await session.handle_ws_message(message)

    def handle_events(self) -> int:
        try:
            return self.ep.libHandleEvents(int(sip_config.events_delay * 1000))
        except pj.Error as e:
            logger.error("PJSIP error handling events. [reason=%s, status=%s]", e.reason, e.status)
            return 0
        except Exception as e:
            logger.error("Error handling PJSIP events. [error_type=%s, error=%s]", e.__class__.__name__, str(e), exc_info=True)
            return 0

    async def quit(self):
        logger.info("Quitting SIP app. [app=%s]", self.get_app_name())
        self.quiting = True

    async def run(self):
        consecutive_empty_cycles = 0
        while not self.quiting:
            try:
                events_processed = self.handle_events()
                if events_processed == 0:
                    consecutive_empty_cycles += 1
                    if consecutive_empty_cycles > 10:
                        sleep_time = min(sip_config.async_delay * 2, 0.1)
                    else:
                        sleep_time = sip_config.async_delay
                else:
                    consecutive_empty_cycles = 0
                    sleep_time = 0.001
                await asyncio.sleep(sleep_time)
                await self.idle()

            except Exception as e:
                logger.error("Error in main loop. [error=%s]", str(e))
                await asyncio.sleep(0.1)

    async def idle(self):
        pass

    async def destroy(self):
        # self.executor.shutdown(True, cancel_futures=True)
        await self.close()
        self.acc.shutdown()
        self.acc = None
        self.ep.libDestroy()
        self.ep = None

    async def init(self):
        self.ep = pj.Endpoint()
        self.ep.libCreate()
        self.ep_cfg = pj.EpConfig()
        self.ep_cfg.uaConfig.threadCnt = 1
        if sip_config.ua_zero_thread_cnt:
            self.ep_cfg.uaConfig.threadCnt = 0
        self.ep_cfg.uaConfig.mainThreadOnly = sip_config.ua_main_thread_only
        # self.ep_cfg.uaConfig.mainThreadOnly = False
        self.ep_cfg.uaConfig.maxCalls = 32
        self.ep_cfg.medConfig.threadCnt = 1
        self.ep_cfg.medConfig.hasIoqueue = True
        self.ep_cfg.medConfig.srtpUse = pj.PJMEDIA_SRTP_OPTIONAL
        # self.ep_cfg.medConfig.srtpSecureSignaling = 0
        self.ep_cfg.medConfig.noVad = sip_config.ec_no_vad
        self.ep_cfg.medConfig.ecTailLen = sip_config.ec_tail_len
        self.ep_cfg.medConfig.ecOptions = (
            pj.PJMEDIA_ECHO_WEBRTC_AEC3
            | pj.PJMEDIA_ECHO_USE_GAIN_CONTROLLER
            | pj.PJMEDIA_ECHO_USE_NOISE_SUPPRESSOR
            # | pj.PJMEDIA_ECHO_AGGRESSIVENESS_AGGRESSIVE
        )
        self.ep_cfg.medConfig.sndAutoCloseTime = -1
        # TODO: recompile sip with support for srtp: ./configure --enable-ssl --with-ssl=/path/to/openssl --enable-srtp
        # Temporarily disabled SRTP to test if it's causing the 488 error
        self.ep_cfg.medConfig.enableSrtp = False
        # self.ep_cfg.medConfig.srtpUse = pj.PJMEDIA_SRTP_OPTIONAL
        # self.ep_cfg.medConfig.srtpKeying = pj.PJMEDIA_SRTP_KEYING_SDES
        # self.ep_cfg.medConfig.srtpSecureSignaling = 0
        self.ep_cfg.logConfig.level = sip_config.pjsip_log_level
        self.ep_cfg.logConfig.filename = sip_config.log_filename
        if sip_config.sip_stun_servers:
            stun_servers = pj.StringVector()
            for stun_server in sip_config.sip_stun_server:
                stun_servers.append(stun_server)
            self.ep_cfg.uaConfig.stunServer = stun_servers
        self.ep.libInit(self.ep_cfg)
        for codec, priority in sip_config.codecs_priority.items():
            self.ep.codecSetPriority(codec, priority)

        for codec in self.ep.codecEnum2():
            logger.info("Supported codec. [codec_id=%s, priority=%s]", codec.codecId, codec.priority)
        if sip_config.sip_null_device:
            self.ep.audDevManager().setNullDev()
        sip_tp_config = pj.TransportConfig()
        sip_tp_config.port = sip_config.sip_port
        self.ep.transportCreate(pj.PJSIP_TRANSPORT_UDP, sip_tp_config)
        if sip_config.sip_use_tcp:
            self.ep.transportCreate(pj.PJSIP_TRANSPORT_TCP, sip_tp_config)
        self.ep.libStart()
        acfg = pj.AccountConfig()
        # Explicitly disable SRTP at account level to avoid negotiation issues
        acfg.mediaConfig.srtpUse = pj.PJMEDIA_SRTP_DISABLED
        acfg.mediaConfig.srtpSecureSignaling = 0
        if sip_config.sip_caller_id:
            acfg.idUri = f'"{sip_config.sip_caller_id}" <sip:{sip_config.sip_user}@{sip_config.sip_domain}>'
        else:
            acfg.idUri = f"sip:{sip_config.sip_user}@{sip_config.sip_domain}"
        acfg.regConfig.registrarUri = (
            f"sip:{sip_config.sip_domain}{';transport=tcp' if sip_config.sip_use_tcp else ''}"
        )
        cred = pj.AuthCredInfo("digest", "*", sip_config.sip_login, 0, sip_config.sip_password)
        acfg.sipConfig.authCreds.append(cred)
        if sip_config.sip_proxy_servers:
            proxy_servers = pj.StringVector()
            for proxy_server in sip_config.sip_proxy_servers:
                proxy_servers.append(proxy_server)
            acfg.sipConfig.proxies = proxy_servers
        acfg.natConfig.iceEnabled = sip_config.sip_use_ice
        # acfg.mediaConfig.noVad = config.ec_no_vad
        # acfg.mediaConfig.ecTailLen = config.ec_tail_len
        # acfg.mediaConfig.ecOptions = (pj.PJMEDIA_ECHO_SPEEX | pj.PJMEDIA_ECHO_USE_GAIN_CONTROLLER
        #                                    | pj.PJMEDIA_ECHO_USE_NOISE_SUPPRESSOR)
        # Create the account
        self.acc = PjAccount(self)
        self.acc.create(acfg)
        await self.open()

async def main():
    from src.integrations.sip.async_callback import set_main_loop

    # Set the main event loop for async callbacks
    set_main_loop(asyncio.get_event_loop())

    backend_url = os.environ["BACKEND_URL"]
    sip_app = PjApp(backend_url)
    signal.signal(signal.SIGINT, lambda _0, _1: asyncio.create_task(sip_app.quit()))
    signal.signal(signal.SIGTERM, lambda _0, _1: asyncio.create_task(sip_app.quit()))
    signal.signal(signal.SIGABRT, lambda _0, _1: asyncio.create_task(sip_app.quit()))
    signal.signal(signal.SIGQUIT, lambda _0, _1: asyncio.create_task(sip_app.quit()))
    try:
        await sip_app.init()
        rewrite_root = os.getenv("REWRITE_ROOT", "true").lower() == "true"
        api_base_url = sip_app.get_base_url() if rewrite_root else '/'
        setup_swagger(sip_app.app, ui_version=3, swagger_url="/docs", api_base_url=api_base_url,
                      security_definitions={"bearerAuth": {"type": "http", "scheme": "bearer"}})
        runner = web.AppRunner(sip_app.app)
        await runner.setup()
        site = web.TCPSite(runner, port=sip_config.sip_rest_api_port)
        await site.start()
        await sip_app.run()
    except Exception as e:
        logger.error("Fatal error running SIP app. [error=%s]", e, exc_info=e)
    finally:
        await sip_app.destroy()
        asyncio.get_event_loop().stop()

if __name__ == "__main__":
    asyncio.run(main())
