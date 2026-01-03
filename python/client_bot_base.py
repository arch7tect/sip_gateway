import asyncio
import io
import json
import logging
import os
import sys
from asyncio import Task
from typing import Any, Awaitable, Dict, List, Literal, Optional, cast

import aiohttp
import scipy
from aiohttp import (
    ClientConnectionError,
    ClientSession,
    ClientWebSocketResponse,
    FormData,
    web,
)
from aiohttp.web_request import Request
from dotenv import load_dotenv
from numpy import ndarray

from src.backend.channel.channel_base import Attachment
from src.backend.health_base import HealthBotBase
from src.backend.health_models import HealthCheck, HealthEnum

logger = logging.getLogger(__name__)


class ClientBotSession(object):
    def __init__(self, bot_base, user_id: str, name: str, bot_type: str, conversation_id: Optional[str] = None):
        self.closed = False
        self.bot_base: ClientBotBase = bot_base
        self.user_id = user_id
        self.name = name
        self.bot_type = bot_type
        self.conversation_id = conversation_id
        self.session_id: Optional[str] = None
        self.metadata: Dict[str, Any] = {}
        self.ws: Optional[ClientWebSocketResponse] = None
        self.ws_task: Optional[Task] = None
        self.session_type: str = os.environ.get("SESSION_TYPE", "inbound")
        self.is_streaming = (self.session_type not in ["inbound", "outbound"] and
                             os.getenv("IS_STREAMING", "true").lower() == "true")
        logger.debug("New session created. [session_type=%s, is_streaming=%s]", self.session_type, self.is_streaming)

    async def request(self, method: str, url: str, raise_error=True, **kwargs):
        return await self.bot_base.request(method, url, raise_error=raise_error, **kwargs)

    async def run(self, message, **kwargs) -> dict:
        json = {"message": message, "kwargs": kwargs}
        response = await self.request("POST", "/session/{id}/run".format(id=self.session_id), json=json)
        return response

    async def run_v2(self, message, attachments) -> dict:
        form = FormData()
        form.add_field("message", message)
        if attachments:
            for attachment in attachments:
                form.add_field("attachments", value=attachment.file, filename=attachment.filename,
                               content_type=attachment.content_type)
        response = await self.request("POST", f"/session/{self.session_id}/run_v2", headers={"Content-Type": None}, data=form)
        return response

    async def logs_v2(self, message) -> dict:
        form = FormData()
        form.add_field("message", message)
        form.add_field("role", "OPERATOR")
        logger.debug("logs_v2 message queued. [session_id=%s, message=%s]", self.session_id, message)
        response = await self.request("POST", f"/session/{self.session_id}/logs_v2", headers={"Content-Type": None}, data=form)
        return response

    async def start(self, message, **kwargs) -> dict:
        json = {"message": message, "kwargs": kwargs}
        response = await self.request("POST", "/session/{id}/start".format(id=self.session_id), json=json)
        return response

    async def commit(self) -> dict:
        json = {}
        response = await self.request("POST", "/session/{id}/commit".format(id=self.session_id), json=json)
        return response

    async def rollback(self) -> dict:
        json = {}
        response = await self.request("POST", "/session/{id}/rollback".format(id=self.session_id), json=json)
        return response

    async def command(self, command: str, *args):
        json = {"command": command, "args": list(args)}
        response = await self.request("POST", "/session/{id}/command".format(id=self.session_id), json=json)
        self.metadata["/" + command] = response.get("metadata")
        return response

    async def update(self, **kwargs):
        response = await self.request("PUT", "/session/{id}".format(id=self.session_id), json=kwargs)
        conversation_id = kwargs.get("conversation_id")
        if conversation_id:
            self.conversation_id = conversation_id
        return response

    async def open(self, *argv, **kwargs) -> dict:
        response = cast(Dict, await self.request("POST", "/session",
                                      json={"user_id": self.user_id, "name": self.name, "type": self.bot_type, "conversation_id": self.conversation_id,
                                            "args": argv, "kwargs": kwargs}))
        self.session_id = response["session"]["session_id"]
        self.ws_task = asyncio.create_task(self.ws_connect(self.session_id))
        logger.debug("Session opened. [session_id=%s]", self.session_id)
        return response

    async def open_v2(self, *argv, attachments: List[Attachment]=None, **kwargs) -> dict:
        form = FormData()
        json_data = json.dumps({"user_id": self.user_id, "name": self.name, "type": self.bot_type, "conversation_id": self.conversation_id,
                                            "communication_id": kwargs.pop("communication_id", None), "args": argv, "kwargs": kwargs})
        form.add_field("body", json_data)
        if attachments:
            for attachment in attachments:
                form.add_field("attachments", value=attachment.file, filename=attachment.filename,
                               content_type=attachment.content_type)
        response = cast(Dict, await self.request("POST", "/session_v2", headers={"Content-Type": None}, data=form))
        self.session_id = response["session"]["session_id"]
        self.ws_task = asyncio.create_task(self.ws_connect(self.session_id))
        logger.debug("Session opened. [session_id=%s]", self.session_id)
        return response

    async def close(self, status: Optional[str]=None):
        await self.detach()
        params = {"status": status} if status else {}
        await self.request("DELETE", f"/session/{self.session_id}", params=params)
        logger.debug("Session closed. [session_id=%s, params=%s]", self.session_id, params)

    async def detach(self):
        if self.ws:
            try:
                await self.ws.close()
            except Exception as e:
                logger.warning("Failed to close WebSocket. [session_id=%s, error=%s]", self.session_id, e)
            self.ws = None
        if self.ws_task:
            self.ws_task.cancel()
            self.ws_task = None
        self.closed = True

    async def waiting_message(self) -> dict:
        response = await self.request("POST", "/waiting_message",
                                      json={"session_id": self.session_id})
        return response

    async def stop_message(self) -> dict:
        response = await self.request("POST", "/stop_message",
                                      json={"session_id": self.session_id})
        return response

    async def get_message(self, message_name: str) -> dict:
        response = await self.request("POST", "/get_message",
                                      json={"message_name": message_name,
                                            "session_id": self.session_id})
        return response

    async def synthesize(self, text: str, content_type: str = "wav"):
        response = await self.request("GET", "/synthesize", params={"text": text, "format": content_type}, response_format="binary")
        return response

    async def session_synthesize(self, text: str, content_type: str = "wav"):
        response = await self.request("GET", f"/session/{self.session_id}/synthesize", params={"text": text, "format": content_type}, response_format="binary")
        return response

    async def transcribe(self, audio: bytes, content_type: str = "wav") -> dict:
        response = await self.request("POST", "/transcribe", data=audio,
                                      headers={'Content-Type': content_type})
        return response

    async def transcribe_nd(self, audio: ndarray, content_type: str = "wav") -> dict:
        memory_file = io.BytesIO()
        memory_file.name = "memory_file.wav"
        scipy.io.wavfile.write(memory_file, 16000, audio)
        # await Path(os.getcwd(), memory_file.name).write_bytes(memory_file.getvalue())
        response = await self.request("POST", "/transcribe", data=memory_file.getvalue(),
                                      headers={'Content-Type': content_type})
        return response

    async def __aenter__(self):
        return self

    async def __aexit__(self, __exc_type, __exc_value, __traceback):
        if not self.closed:
            await self.close()

    async def ws_connect(self, session_id):
        while not self.closed:
            logger.info("Connecting WebSocket. [session_id=%s]", session_id)
            async with self.bot_base.session.ws_connect(f'/ws/{session_id}'.format(session_id=session_id), ssl=False) as ws:
                self.ws = ws
                async for msg in ws:
                    try:
                        if msg.type == aiohttp.WSMsgType.ERROR:
                            logger.error("WebSocket error. [session_id=%s, error=%s]", session_id, self.ws.exception())
                        else:
                            body = msg.json()
                            if body["type"] == "timeout":
                                await self.bot_base.on_timeout(self.session_id)
                            elif body["type"] == "close":
                                await self.bot_base.on_close(self.session_id)
                            else:
                                await self.bot_base.ws_message(self, body)
                    except Exception as e:
                        logger.error("WebSocket error. [session_id=%s, error=%s]", session_id, e, exc_info=e)
                logger.info("WebSocket disconnected. [session_id=%s]", session_id)
                self.ws = None
            await asyncio.sleep(5)

    async def ws_send_json(self, json_data):
        if self.ws is not None:
            await self.ws.send_json(json_data)


class ClientBotBase(HealthBotBase):
    def __init__(self, base_url: str):
        self.base_url = base_url
        self.session: Optional[ClientSession] = None
        self.sessions: Dict[str, ClientBotSession] = dict()
        self.conversation_id_to_session_id = dict()
        self.session_id_to_conversation_id = dict()
        self.show_waiting_messages = os.getenv("SHOW_WAITING_MESSAGES", "false").lower() == "true"
        self.request_timeout = aiohttp.ClientTimeout(
            total=float(os.getenv("BACKEND_REQUEST_TIMEOUT", "60")),
            connect=float(os.getenv("BACKEND_CONNECT_TIMEOUT", "60")),
            sock_read=float(os.getenv("BACKEND_SOCK_READ_TIMEOUT", "60"))
        )


    async def request(self, method: str, url: str, raise_error=True, **kwargs):
        response_format = cast(Literal["json", "text", "binary"], kwargs.pop("response_format", "json"))
        return await self._request(method, url, raise_error, response_format=response_format, **kwargs)

    async def request_binary(self, method: str, url: str, raise_error=True, **kwargs):
        return await self._request(method, url, raise_error, response_format="binary", **kwargs)

    async def _request(self, method: str, url: str, raise_error=True, response_format: Literal["json", "text", "binary"]= "json", **kwargs):
        authorization_token = os.getenv("AUTHORIZATION_TOKEN")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
            'Authorization': f'Bearer {authorization_token}'
        }
        headers.update(kwargs.pop('headers', {}))
        headers = {k: v for k, v in headers.items() if v}
        # if 'timeout' not in kwargs:
        #     kwargs['timeout'] = self.request_timeout
        async with self.session.request(method, url, headers=headers, **kwargs) as resp:
            if resp.status < 200 or resp.status >= 300:
                try:
                    message = (await resp.json())["message"]
                except Exception:
                    message = await resp.text()
                logger.error("API error. [method=%s, url=%s, status=%s, message=%s]", method, url, resp.status, message)
                if raise_error:
                    if resp.status == 403:
                        raise PermissionError(message)
                    raise RuntimeError(message)
            if response_format == "json":
                return await resp.json()
            if response_format == "text":
                return await resp.text()
            else:
                return await resp.read()

    async def get_bot_back_health_check(self) -> HealthCheck:
        name = "BOT_BACK"
        try:
            _ = await self.request("GET", "/capabilities", raise_error=True)
            return HealthCheck(name=name, status=HealthEnum.UP)
        except ClientConnectionError:
            return HealthCheck(name=name, status=HealthEnum.DOWN)
        except Exception as e:
            logger.error("Health check failed. [check=%s, error=%s]", name, e, exc_info=e)
            return HealthCheck(name=name, status=HealthEnum.UNKNOWN)

    def get_health_checks_coros(self) -> List[Awaitable[HealthCheck]]:
        return [self.get_bot_back_health_check()]

    async def open(self):
        timeout = aiohttp.ClientTimeout(connect=5)
        self.session = ClientSession(self.base_url, timeout=timeout)

    async def close(self):
        for session in self.sessions.values():
            await session.close()
        if self.session is not None:
            await self.session.close()
            self.session = None

    async def __aenter__(self):
        await self.open()
        return self

    async def __aexit__(self, __exc_type, __exc_value, __traceback):
        await self.close()

    async def health(self, _: Request):
        response = await self.get_health_check()
        return web.Response(text=response.model_dump_json(exclude_defaults=True), status=200 if response.status == HealthEnum.UP else 503)

    async def start_session(self, user_id, name, bot_type, *argv, timeout: Optional[float] = None,
                            conversation_id: Optional[str] = None,  **kwargs):
        session = self.create_session(user_id, name, bot_type, conversation_id, **kwargs)
        kwargs['timeout'] = timeout
        initialization_response = await session.open_v2(*argv, **kwargs)
        session.metadata['initialization_response'] = initialization_response
        self.sessions[session.session_id] = session
        if conversation_id:
            self.set_session_to_conversation(conversation_id, session.session_id)
        return session

    def set_session_to_conversation(self, conversation_id, session_id):
        self.conversation_id_to_session_id[conversation_id] = session_id
        self.session_id_to_conversation_id[session_id] = conversation_id

    def create_session(self, user_id, name, bot_type, conversation_id: Optional[str] = None,  **kwargs):
        return ClientBotSession(self, user_id, name, bot_type, conversation_id)

    async def close_session(self, session_id, status: Optional[str]=None):
        logger.debug("Closing session. [session_id=%s, status=%s]", session_id, status)
        session = self.clear_conversation_id(session_id)
        await session.close(status=status)

    def clear_conversation_id(self, session_id):
        session = self.sessions.pop(session_id, None)
        if session is not None:
            conversation_id = self.session_id_to_conversation_id.get(session_id)
            if conversation_id:
                del self.session_id_to_conversation_id[session_id]
                if conversation_id in self.conversation_id_to_session_id:
                    del self.conversation_id_to_session_id[conversation_id]
        return session

    async def ws_message(self, session, message):
        logger.debug("WebSocket message received. [session_id=%s, message=%s]", session.session_id, message)

    def get_session(self, session_id) -> ClientBotSession:
        return self.sessions.get(session_id)

    async def on_timeout(self, session_id):
        logger.debug("Session timeout event. [session_id=%s]", session_id)

    async def on_close(self, session_id):
        logger.debug("Session close event. [session_id=%s]", session_id)
        session = self.clear_conversation_id(session_id)
        await session.detach()
        return session


async def main(base_url):
    async with ClientBotBase(base_url) as client:
        async with await client.start_session("arch7tect", "Oleg Orlov", "web") as session:
            response = await session.command("thoughts", "on")
            logger.info("Thoughts response. [response=%s]", response)
            await asyncio.sleep(0.5)
            response = await session.command("plugins")
            logger.info("Plugins response. [response=%s]", response)
            response = await session.run("hello world")
            logger.info("Run response. [response=%s]", response)


if __name__ == "__main__":
    from src.logger import LoggerType, setup_logger

    load_dotenv(override=True)
    setup_logger(log_type=LoggerType.CLIENT, log_name=os.getenv("LOG_NAME", __name__))
    
    asyncio.run(main("http://127.0.0.1:8000/"))
    
