"""LAN simulator for the ESP32 robot inspection panel.

Replace RobotAdapter with a real robot HTTP adapter without changing the panel API.
"""
from __future__ import annotations

import asyncio
import os
import time
import uuid
from dataclasses import dataclass, asdict
from typing import Literal

from fastapi import FastAPI, Header, HTTPException
from pydantic import BaseModel, Field

TaskKind = Literal["navigate", "inspection"]
TaskStatus = Literal["queued", "navigating", "arrived", "running", "completed", "failed"]


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


POINTS = [Point(id="lobby", name="大厅"), Point(id="warehouse", name="仓库"), Point(id="line-1", name="一号产线")]
INSPECTIONS = [InspectionOption(id="general", name="常规巡检")]
TASKS: dict[str, Task] = {}
EVENTS: dict[str, str] = {}
API_TOKEN = os.getenv("ROBOT_PANEL_API_TOKEN", "")
app = FastAPI(title="智巡精灵 v1.0", version="1.0.0")


def authorize(token: str | None) -> None:
    if API_TOKEN and token != API_TOKEN:
        raise HTTPException(401, "invalid API token")


def response(task: Task) -> dict:
    return asdict(task)


class RobotAdapter:
    """Simulator boundary. A real adapter would issue robot HTTP commands here."""

    async def navigate(self, task_id: str) -> None:
        await asyncio.sleep(1)
        update(task_id, "navigating", "机器人正在前往点位")
        await asyncio.sleep(3)
        update(task_id, "arrived", "机器人已到达")

    async def inspect(self, task_id: str) -> None:
        await asyncio.sleep(1)
        update(task_id, "running", "正在执行常规巡检")
        await asyncio.sleep(3)
        update(task_id, "completed", "巡检已完成")


robot = RobotAdapter()


def update(task_id: str, status: TaskStatus, message: str) -> None:
    task = TASKS.get(task_id)
    if task and task.status not in {"failed", "completed"}:
        task.status, task.message, task.updated_at = status, message, time.time()


def create(event_id: str, kind: TaskKind, device_id: str, point_id: str, message: str) -> tuple[Task, bool]:
    if event_id in EVENTS:
        return TASKS[EVENTS[event_id]], False
    now = time.time()
    task = Task(str(uuid.uuid4()), event_id, kind, "queued", message, device_id, point_id, now, now)
    TASKS[task.id] = task
    EVENTS[event_id] = task.id
    return task, True


@app.get("/health")
def health() -> dict:
    return {"ok": True}


@app.get("/api/panels/{device_id}/config")
def panel_config(device_id: str, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    return {"device_id": device_id, "site_id": "site-a", "site_name": "主入口", "points": [p.model_dump() for p in POINTS]}


@app.get("/api/points/{point_id}/inspections")
def inspections(point_id: str, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    if point_id not in {p.id for p in POINTS}:
        raise HTTPException(404, "point not found")
    return {"point_id": point_id, "inspections": [item.model_dump() for item in INSPECTIONS]}


@app.post("/api/navigation-tasks", status_code=201)
async def create_navigation(request: NavigateRequest, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    if request.point_id not in {p.id for p in POINTS}:
        raise HTTPException(422, "unknown point")
    task, is_new = create(request.event_id, "navigate", request.device_id, request.point_id, "导航任务已排队")
    if is_new:
        asyncio.create_task(robot.navigate(task.id))
    return response(task)


@app.post("/api/inspection-tasks", status_code=201)
async def create_inspection(request: InspectionRequest, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    arrival = TASKS.get(request.arrival_task_id)
    if not arrival or arrival.kind != "navigate":
        raise HTTPException(404, "arrival task not found")
    if arrival.status != "arrived":
        raise HTTPException(409, "robot has not arrived")
    if request.inspection_id not in {item.id for item in INSPECTIONS}:
        raise HTTPException(422, "unknown inspection")
    task, is_new = create(request.event_id, "inspection", request.device_id, arrival.point_id, "巡检任务已排队")
    if is_new:
        asyncio.create_task(robot.inspect(task.id))
    return response(task)


@app.get("/api/tasks/{task_id}")
def get_task(task_id: str, x_api_token: str | None = Header(default=None)) -> dict:
    authorize(x_api_token)
    task = TASKS.get(task_id)
    if not task:
        raise HTTPException(404, "task not found")
    return response(task)
