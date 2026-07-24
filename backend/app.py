"""LAN simulator for the ESP32 robot inspection panel.

Replace RobotAdapter with a real robot HTTP adapter without changing the panel API.
"""
from __future__ import annotations

import asyncio
import os
import time
import uuid
from dataclasses import asdict, dataclass
from typing import Literal

from fastapi import FastAPI, Header, HTTPException
from pydantic import BaseModel, Field

TaskKind = Literal["navigate", "delivery", "inspection"]
TaskStatus = Literal[
    "queued",
    "navigating",
    "arrived",
    "awaiting_load",
    "awaiting_destination",
    "delivering",
    "awaiting_unload",
    "running",
    "completed",
    "failed",
    "cancelled",
]
NavigationPurpose = Literal["delivery_pickup", "inspection"]

TERMINAL_STATUSES = {"completed", "failed", "cancelled"}
SIM_STEP_SECONDS = float(os.getenv("ROBOT_SIM_STEP_SECONDS", "1"))
DELIVERY_UNLOAD_TIMEOUT_SECONDS = float(os.getenv("DELIVERY_UNLOAD_TIMEOUT_SECONDS", "120"))


class Point(BaseModel):
    id: str
    name: str


class InspectionOption(BaseModel):
    id: str
    name: str


class NavigateRequest(BaseModel):
    event_id: str = Field(min_length=1, max_length=96)
    device_id: str = Field(min_length=1)
    site_id: str = Field(min_length=1)
    point_id: str = Field(min_length=1)
    purpose: NavigationPurpose = "inspection"
    replace_task_id: str | None = None


class TaskActionRequest(BaseModel):
    event_id: str = Field(min_length=1, max_length=96)
    device_id: str = Field(min_length=1)


class DeliveryRequest(BaseModel):
    event_id: str = Field(min_length=1, max_length=96)
    device_id: str = Field(min_length=1)
    pickup_task_id: str = Field(min_length=1)
    destination_point_id: str = Field(min_length=1)


class InspectionRequest(BaseModel):
    event_id: str = Field(min_length=1, max_length=96)
    device_id: str = Field(min_length=1)
    arrival_task_id: str = Field(min_length=1)
    inspection_id: str = Field(min_length=1)


@dataclass
class Task:
    id: str
    event_id: str
    kind: TaskKind
    status: TaskStatus
    message: str
    device_id: str
    point_id: str
    created_at: float
    updated_at: float
    purpose: NavigationPurpose | None = None
    parent_task_id: str | None = None
    source_point_id: str | None = None
    destination_point_id: str | None = None
    unload_deadline: float | None = None


@dataclass
class RobotState:
    current_point_id: str = "lobby"
    active_task_id: str | None = None
    updated_at: float = 0.0


POINTS = [
    Point(id="lobby", name="大厅"),
    Point(id="warehouse", name="仓库"),
    Point(id="line-1", name="一号产线"),
    Point(id="line-2", name="二号产线"),
    Point(id="line-3", name="三号产线"),
    Point(id="storage", name="原料库"),
    Point(id="finish", name="成品库"),
    Point(id="office-a", name="A 办公区"),
    Point(id="office-b", name="B 办公区"),
    Point(id="cafeteria", name="食堂"),
    Point(id="guard", name="门卫"),
    Point(id="parking", name="停车场"),
    Point(id="boiler", name="锅炉房"),
    Point(id="sewage", name="污水处理站"),
    Point(id="lab", name="实验室"),
]
POINT_BY_ID = {point.id: point for point in POINTS}
INSPECTIONS = [InspectionOption(id="general", name="常规巡检")]
TASKS: dict[str, Task] = {}
EVENTS: dict[str, str] = {}
RUNNERS: dict[str, asyncio.Task[None]] = {}
ROBOT = RobotState(updated_at=time.time())
STATE_LOCK = asyncio.Lock()
API_TOKEN = os.getenv("ROBOT_PANEL_API_TOKEN", "")
app = FastAPI(title="智巡精灵 v1.1", version="1.1.0")


def authorize(token: str | None) -> None:
    if API_TOKEN and token != API_TOKEN:
        raise HTTPException(401, "invalid API token")


def response(task: Task) -> dict:
    payload = asdict(task)
    payload["unload_remaining_seconds"] = (
        max(0, int(task.unload_deadline - time.time() + 0.999)) if task.unload_deadline else None
    )
    return payload


def point_response(point_id: str | None) -> dict | None:
    point = POINT_BY_ID.get(point_id or "")
    return point.model_dump() if point else None


def new_task(
    event_id: str,
    kind: TaskKind,
    device_id: str,
    point_id: str,
    message: str,
    **kwargs: object,
) -> Task:
    now = time.time()
    task = Task(
        id=str(uuid.uuid4()),
        event_id=event_id,
        kind=kind,
        status="queued",
        message=message,
        device_id=device_id,
        point_id=point_id,
        created_at=now,
        updated_at=now,
        **kwargs,
    )
    TASKS[task.id] = task
    EVENTS[event_id] = task.id
    return task


def start_runner(task_id: str, coroutine: object) -> None:
    runner = asyncio.create_task(coroutine)  # type: ignore[arg-type]
    RUNNERS[task_id] = runner

    def remove_finished(finished: asyncio.Task[None]) -> None:
        if RUNNERS.get(task_id) is finished:
            RUNNERS.pop(task_id, None)

    runner.add_done_callback(remove_finished)


def cancel_active_locked(task_id: str) -> None:
    task = TASKS.get(task_id)
    if not task or task.status in TERMINAL_STATUSES:
        return
    runner = RUNNERS.pop(task_id, None)
    if runner:
        runner.cancel()
    task.status = "cancelled"
    task.message = "任务已被新任务替换"
    task.updated_at = time.time()
    if ROBOT.active_task_id == task_id:
        ROBOT.active_task_id = None
        ROBOT.updated_at = task.updated_at


async def set_task_state(
    task_id: str,
    status: TaskStatus,
    message: str,
    *,
    arrive: bool = False,
    release: bool = False,
    unload_deadline: float | None = None,
) -> bool:
    async with STATE_LOCK:
        task = TASKS.get(task_id)
        if not task or task.status in TERMINAL_STATUSES or ROBOT.active_task_id != task_id:
            return False
        now = time.time()
        task.status = status
        task.message = message
        task.updated_at = now
        task.unload_deadline = unload_deadline
        if arrive:
            ROBOT.current_point_id = task.point_id
        if release:
            ROBOT.active_task_id = None
        ROBOT.updated_at = now
        return True


class RobotAdapter:
    """Simulator boundary. A real adapter would issue robot HTTP commands here."""

    async def navigate(self, task_id: str) -> None:
        try:
            await asyncio.sleep(SIM_STEP_SECONDS)
            if not await set_task_state(task_id, "navigating", "机器人正在前往点位"):
                return
            await asyncio.sleep(SIM_STEP_SECONDS * 3)
            task = TASKS[task_id]
            if task.purpose == "delivery_pickup":
                await set_task_state(task_id, "awaiting_load", "机器人已到达，等待装货", arrive=True)
            else:
                await set_task_state(task_id, "arrived", "机器人已到达巡检点", arrive=True)
        except asyncio.CancelledError:
            return

    async def deliver(self, task_id: str) -> None:
        try:
            await asyncio.sleep(SIM_STEP_SECONDS)
            if not await set_task_state(task_id, "delivering", "正在送往目的地"):
                return
            await asyncio.sleep(SIM_STEP_SECONDS * 3)
            deadline = time.time() + DELIVERY_UNLOAD_TIMEOUT_SECONDS
            if not await set_task_state(
                task_id,
                "awaiting_unload",
                "已到达，等待卸货确认",
                arrive=True,
                unload_deadline=deadline,
            ):
                return
            await asyncio.sleep(DELIVERY_UNLOAD_TIMEOUT_SECONDS)
            await set_task_state(task_id, "completed", "卸货等待超时，送货已完成", release=True)
        except asyncio.CancelledError:
            return

    async def inspect(self, task_id: str) -> None:
        try:
            await asyncio.sleep(SIM_STEP_SECONDS)
            if not await set_task_state(task_id, "running", "正在执行常规巡检"):
                return
            await asyncio.sleep(SIM_STEP_SECONDS * 3)
            await set_task_state(task_id, "completed", "巡检已完成", release=True)
        except asyncio.CancelledError:
            return


robot = RobotAdapter()


def robot_phase(task: Task | None) -> str:
    if not task:
        return "idle"
    if task.kind == "navigate":
        if task.purpose == "delivery_pickup":
            return {
                "queued": "going_to_pickup",
                "navigating": "going_to_pickup",
                "awaiting_load": "awaiting_load",
                "awaiting_destination": "awaiting_destination",
            }.get(task.status, task.status)
        return "awaiting_inspection" if task.status == "arrived" else "going_to_inspection"
    if task.kind == "delivery":
        return "awaiting_unload" if task.status == "awaiting_unload" else "delivering"
    return "inspecting"


async def reset_state() -> None:
    """Reset simulator state for tests."""
    runners = list(RUNNERS.values())
    RUNNERS.clear()
    for runner in runners:
        runner.cancel()
    if runners:
        await asyncio.gather(*runners, return_exceptions=True)
    async with STATE_LOCK:
        TASKS.clear()
        EVENTS.clear()
        ROBOT.current_point_id = "lobby"
        ROBOT.active_task_id = None
        ROBOT.updated_at = time.time()


@app.get("/health")
def health() -> dict:
    return {"ok": True}


@app.get("/api/robot/status")
async def robot_status(x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    async with STATE_LOCK:
        active = TASKS.get(ROBOT.active_task_id or "")
        target_id = active.point_id if active else None
        return {
            "state": "busy" if active else "idle",
            "current_point": point_response(ROBOT.current_point_id),
            "target_point": point_response(target_id),
            "phase": robot_phase(active),
            "message": active.message if active else "机器狗空闲",
            "active_task": response(active) if active else None,
            "unload_deadline": active.unload_deadline if active else None,
            "unload_remaining_seconds": (
                max(0, int(active.unload_deadline - time.time() + 0.999))
                if active and active.unload_deadline
                else None
            ),
            "updated_at": ROBOT.updated_at,
        }


@app.get("/api/panels/{device_id}/config")
def panel_config(device_id: str, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    return {
        "device_id": device_id,
        "site_id": "site-a",
        "site_name": "主入口",
        "points": [point.model_dump() for point in POINTS],
    }


@app.get("/api/points/{point_id}/inspections")
def inspections(point_id: str, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    if point_id not in POINT_BY_ID:
        raise HTTPException(404, "point not found")
    return {"point_id": point_id, "inspections": [item.model_dump() for item in INSPECTIONS]}


@app.post("/api/navigation-tasks", status_code=201)
async def create_navigation(request: NavigateRequest, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    if request.point_id not in POINT_BY_ID:
        raise HTTPException(422, "unknown point")
    async with STATE_LOCK:
        if request.event_id in EVENTS:
            return response(TASKS[EVENTS[request.event_id]])
        active = TASKS.get(ROBOT.active_task_id or "")
        if active:
            if request.replace_task_id != active.id:
                raise HTTPException(409, {"message": "robot busy", "active_task": response(active)})
            cancel_active_locked(active.id)
        elif request.replace_task_id:
            raise HTTPException(409, "replacement task is no longer active")
        task = new_task(
            request.event_id,
            "navigate",
            request.device_id,
            request.point_id,
            "导航任务已排队",
            purpose=request.purpose,
        )
        ROBOT.active_task_id = task.id
        ROBOT.updated_at = task.updated_at
        start_runner(task.id, robot.navigate(task.id))
        return response(task)


@app.post("/api/navigation-tasks/{task_id}/load-complete")
async def load_complete(
    task_id: str,
    request: TaskActionRequest,
    x_api_token: str | None = Header(default=None),
) -> dict:
    authorize(x_api_token)
    async with STATE_LOCK:
        if request.event_id in EVENTS:
            return response(TASKS[EVENTS[request.event_id]])
        task = TASKS.get(task_id)
        if not task or task.kind != "navigate" or task.purpose != "delivery_pickup":
            raise HTTPException(404, "pickup task not found")
        if ROBOT.active_task_id != task.id or task.status != "awaiting_load":
            raise HTTPException(409, "robot is not awaiting load for this task")
        task.status = "awaiting_destination"
        task.message = "装货完成，等待选择送货点"
        task.updated_at = time.time()
        ROBOT.updated_at = task.updated_at
        EVENTS[request.event_id] = task.id
        return response(task)


@app.post("/api/delivery-tasks", status_code=201)
async def create_delivery(request: DeliveryRequest, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    if request.destination_point_id not in POINT_BY_ID:
        raise HTTPException(422, "unknown destination point")
    async with STATE_LOCK:
        if request.event_id in EVENTS:
            return response(TASKS[EVENTS[request.event_id]])
        pickup = TASKS.get(request.pickup_task_id)
        if not pickup or pickup.kind != "navigate" or pickup.purpose != "delivery_pickup":
            raise HTTPException(404, "pickup task not found")
        if ROBOT.active_task_id != pickup.id or pickup.status != "awaiting_destination":
            raise HTTPException(409, "pickup task is not awaiting destination")
        if pickup.point_id == request.destination_point_id:
            raise HTTPException(422, "destination must differ from pickup point")
        pickup.status = "completed"
        pickup.message = "装货完成，开始送货"
        pickup.updated_at = time.time()
        task = new_task(
            request.event_id,
            "delivery",
            request.device_id,
            request.destination_point_id,
            "送货任务已排队",
            parent_task_id=pickup.id,
            source_point_id=pickup.point_id,
            destination_point_id=request.destination_point_id,
        )
        ROBOT.active_task_id = task.id
        ROBOT.updated_at = task.updated_at
        start_runner(task.id, robot.deliver(task.id))
        return response(task)


@app.post("/api/delivery-tasks/{task_id}/unload-complete")
async def unload_complete(
    task_id: str,
    request: TaskActionRequest,
    x_api_token: str | None = Header(default=None),
) -> dict:
    authorize(x_api_token)
    async with STATE_LOCK:
        if request.event_id in EVENTS:
            return response(TASKS[EVENTS[request.event_id]])
        task = TASKS.get(task_id)
        if not task or task.kind != "delivery":
            raise HTTPException(404, "delivery task not found")
        if ROBOT.active_task_id != task.id or task.status != "awaiting_unload":
            raise HTTPException(409, "delivery is not awaiting unload")
        runner = RUNNERS.pop(task.id, None)
        if runner:
            runner.cancel()
        task.status = "completed"
        task.message = "卸货完成，送货任务已完成"
        task.unload_deadline = None
        task.updated_at = time.time()
        ROBOT.active_task_id = None
        ROBOT.updated_at = task.updated_at
        EVENTS[request.event_id] = task.id
        return response(task)


@app.post("/api/inspection-tasks", status_code=201)
async def create_inspection(request: InspectionRequest, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    if request.inspection_id not in {item.id for item in INSPECTIONS}:
        raise HTTPException(422, "unknown inspection")
    async with STATE_LOCK:
        if request.event_id in EVENTS:
            return response(TASKS[EVENTS[request.event_id]])
        arrival = TASKS.get(request.arrival_task_id)
        if not arrival or arrival.kind != "navigate" or arrival.purpose != "inspection":
            raise HTTPException(404, "arrival task not found")
        if ROBOT.active_task_id != arrival.id or arrival.status != "arrived":
            raise HTTPException(409, "robot has not arrived for this inspection")
        arrival.status = "completed"
        arrival.message = "已到达并开始巡检"
        arrival.updated_at = time.time()
        task = new_task(
            request.event_id,
            "inspection",
            request.device_id,
            arrival.point_id,
            "巡检任务已排队",
            parent_task_id=arrival.id,
        )
        ROBOT.active_task_id = task.id
        ROBOT.updated_at = task.updated_at
        start_runner(task.id, robot.inspect(task.id))
        return response(task)


@app.get("/api/tasks/{task_id}")
def get_task(task_id: str, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    task = TASKS.get(task_id)
    if not task:
        raise HTTPException(404, "task not found")
    return response(task)
