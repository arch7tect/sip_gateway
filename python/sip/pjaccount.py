import logging
import os
from typing import TYPE_CHECKING

import pjsua2 as pj

from src.integrations.sip.async_callback import AsyncCallbackJob
from src.integrations.sip.audio_message import AudioMessage

if TYPE_CHECKING:
    from src.integrations.sip.pjapp import PjApp
    from src.integrations.sip.pjcall import PjCall

from src.integrations.sip.sip_config import sip_config

logger = logging.getLogger(__name__)

class PjAccount(pj.Account):
    def __init__(self, app: 'PjApp'):
        pj.Account.__init__(self)
        self.app: PjApp = app
        os.makedirs(sip_config.tmp_audio_dir, exist_ok=True)

    def onRegState(self, prm):
        try:
            logger.info("SIP registration state. [status=%s, reason=%s]", prm.code, prm.reason)
            if prm.code // 100 == 5:
                logger.error("SIP registration server error. [status=%s, reason=%s]", prm.code, prm.reason)
            elif prm.code == 408:
                logger.warning("SIP registration timeout. [status=%s, reason=%s]", prm.code, prm.reason)
            elif prm.code == 200:
                ps = pj.PresenceStatus()
                ps.status = pj.PJSUA_BUDDY_STATUS_ONLINE
                ps.note = "Ready to answer"
                self.setOnlineStatus(ps)
                logger.info("SIP registration successful.")
            elif prm.code != 0:
                logger.warning("SIP registration failed. [status=%s, reason=%s]", prm.code, prm.reason)
        except Exception as e:
            logger.error("Exception in onRegState. [error_type=%s, error=%s]",
                        e.__class__.__name__, str(e), exc_info=True)

    def onIncomingCall(self, iprm):
        try:
            logger.info("Incoming call. [call_id=%s]", iprm.callId)
            call: PjCall = self.app.create_session("", "", self.app.get_app_name(), call_id=iprm.callId)
            prm = pj.CallOpParam(True)
            prm.statusCode = pj.PJSIP_SC_RINGING
            call.answer(prm)
            AsyncCallbackJob(self.on_incoming_call, call).submit()
        except Exception as e:
            logger.error("Exception in onIncomingCall. [error_type=%s, error=%s, call_id=%s]",
                        e.__class__.__name__, str(e), iprm.callId, exc_info=True)

    async def on_incoming_call(self, call: 'PjCall'):
        initialization_response = await call.open()
        call.metadata['initialization_response'] = initialization_response
        self.app.sessions[call.session_id] = call
        greeting = call.metadata.get('initialization_response', {}).get('greeting')
        if greeting:
            message = AudioMessage(call, greeting)
            await message.get_blob()
            call.metadata['greeting_message'] = message
        prm = pj.CallOpParam(True)
        prm.statusCode = pj.PJSIP_SC_OK
        call.answer(prm)

