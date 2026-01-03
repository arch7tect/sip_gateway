import asyncio
import logging
from asyncio import Task
from enum import Enum
from typing import Dict, Coroutine

logger = logging.getLogger(__name__)

class TaskName(Enum):
    START = "START"
    COMMIT = "COMMIT"
    # ROLLBACK = "ROLLBACK"


class TaskManager(Dict[TaskName, Task]):

    async def await_and_delete(self, name: TaskName):
        if name in self:
            logger.debug("Awaiting task. [task_name=%s]", name.value)
            await self.pop(name)
            return True
        return False

    def cancel_and_delete(self, name: TaskName):
        if name in self:
            logger.debug("Cancelling task. [task_name=%s]", name.value)
            self.pop(name).cancel()
            return True
        return False

    def create_task(self, name: TaskName, coro: Coroutine):
        logger.debug("Creating task. [task_name=%s, coroutine=%s]", name.value, coro)
        self[name] = asyncio.create_task(coro)

    def cancel_all_tasks(self):
        tasks = list(self.values())
        self.clear()
        for task in tasks:
            task.cancel()

