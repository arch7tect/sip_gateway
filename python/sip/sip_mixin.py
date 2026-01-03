import asyncio
import logging
import os
from abc import ABC, abstractmethod
from typing import List, Optional

import pjsua2 as pj
from aiohttp import web
from aiohttp.web_request import Request

from src.backend.client_app_mixin import ClientAppMixin
from src.backend.client_bot_base import ClientBotSession
from src.integrations.sip.audio_message import AudioMessage
from src.integrations.sip.pjcall import PjCall

logger = logging.getLogger(__name__)

class SipAppMixin(ClientAppMixin, ABC):
    def get_app_name(self) -> str:
        return "sip"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.config = dict()
        self.config["FLAMETREE_CALLBACK_URL"] = os.getenv("FLAMETREE_CALLBACK_URL")
        self.config["FLAMETREE_CALLBACK_PORT"] = int(os.getenv("FLAMETREE_CALLBACK_PORT", "8088"))
        self.app.add_routes([
            web.post("/call", self._make_call),
            web.post("/transfer/{session_id}", self._make_transfer),
        ])
        self.app.middlewares.append(self.bearer_auth_factory())

        self.app.on_startup.append(self.on_startup)
        self.app.on_cleanup.append(self.on_cleanup)

    async def on_startup(self, app):
        logger.info("SIP app startup. [app=%s]", self.get_app_name())

    async def on_cleanup(self, app):
        logger.info("SIP app cleanup. [app=%s]", self.get_app_name())

    async def _make_call(self, request: Request):
        """
        description: Make sip call.
        tags:
        - sip
        produces:
        - application/json
        requestBody:
          content:
            "application/json":
                required: true
                schema:
                    type: object
                    properties:
                      to_uri:
                        title: Phone number to call
                        type: string
                      env_info:
                        title: Environment Info
                        anyOf:
                          - type: object
                            additionalProperties: {}
                          - type: null
        responses:
            "200":
                description: 'successful operation. Returns {"session_id": session_id}'
            "500":
                description: Returns exception text
        security:
            - bearerAuth: []
        """
        session: Optional[PjCall] = None
        try:
            body = await request.json()
            to_uri = body["to_uri"]
            env_info = body.get("env_info", {})
            communication_id = body.get("communication_id", None)
            logger.info("Making outbound call. [to_uri=%s, env_info=%s, communication_id=%s]", to_uri, env_info, communication_id)
            session = await self.start_session(to_uri, "", self.get_app_name(), "", communication_id=communication_id, **env_info)
            greeting = session.metadata.get('initialization_response', {}).get('greeting')
            if greeting:
                message = AudioMessage(session, greeting)
                await message.get_blob()
                session.metadata['greeting_message'] = message
            call_param = pj.CallOpParam(True)
            session.makeCall(to_uri, call_param)
            return web.json_response({'message': 'ok', "session_id": session.session_id}, status=200)
        except Exception as exc:
            message = exc.reason if hasattr(exc, "reason") else str(exc)
            try:
                logger.error("Failed to make call. [error=%s]", message, exc_info=exc)
                if session:
                    await ClientBotSession.update(session, conversation_result={"message": message})
                    await session.close_session("failed")
            except Exception as e:
                logger.error("Error during call cleanup. [error=%s]", message, exc_info=e)
            return web.json_response({'message': message}, status=500)

    async def _make_transfer(self, request: Request):
        """
        description: Transfer a call to another number.
        tags:
        - sip
        produces:
        - application/json
        parameters:
          - name: session_id
            in: path
            required: true
            description: Unique identifier of the active call session
            schema:
              type: string
        requestBody:
          content:
            "application/json":
                schema:
                    type: object
                    required:
                      - to_uri
                    properties:
                      to_uri:
                        title: SIP URI or dtmf message for transfer the call
                        type: string
                        example: "sip:destination@example.com" or "dtmf:*1w5555"
                      transfer_delay:
                        title: Delay in secs after dtmf transfer. Default 1.0
                        type: float
        responses:
            "200":
                description: 'successful operation, returns {"status": "ok", "session_id": session_id, "to_uri": to_uri}'
            "400":
                description: 'Call is not active'
            "404":
                description: 'Call session not found'
            "500":
                description: Returns exception text
        security:
            - bearerAuth: []
        """
        session_id = request.match_info['session_id']
        session: Optional[PjCall] = self.get_session(session_id)
        if not session:
            return web.json_response({"status": "error", "message": "Session not found"}, status=404)
        try:
            if session.getInfo().state != pj.PJSIP_INV_STATE_CONFIRMED:
                return web.json_response({"status": "error", "message": "Call is not active"}, status=400)
            body = await request.json()
            to_uri = body['to_uri']
            session.to_uri = to_uri
            logger.debug("Transfer URI set. [to_uri=%s, session_id=%s]", session.to_uri, session.session_id)
            if 'transfer_delay' in body:
                logger.debug("Transfer delay set. [delay=%s, session_id=%s]", body['transfer_delay'], session.session_id)
                session.transfer_delay = float(body['transfer_delay'])
            return web.json_response({"status": "ok", "message": f"Successfully transferred", "session_id": session.session_id, "to_uri": to_uri}, status=200)
        except Exception as exc:
            message = exc.reason if hasattr(exc, "reason") else str(exc)
            logger.error("Failed to transfer call. [error=%s, session_id=%s]", message, session_id, exc_info=exc)
            try:
                if session:
                    await ClientBotSession.update(session, conversation_result={"message": message})
                    await session.close_session("failed")
            except Exception as e:
                logger.error("Error during transfer cleanup. [error=%s, session_id=%s]", str(e), session_id)
            return web.json_response({"status": "error", "message": message}, status=500)

    def run(self, *args, **kwargs):
        super().run(*args, port=self.config["FLAMETREE_CALLBACK_PORT"], **kwargs)

    def get_protected_paths(self) -> List[str]:
        return ["/call", "/transfer"]

    @abstractmethod
    async def start_session(self, user_id, name, bot_type, conversation_id=None, *argv, **kwargs):
        pass

    @abstractmethod
    def get_session(self, session_id) -> Optional[PjCall]:
        pass
