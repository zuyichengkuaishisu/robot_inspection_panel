import asyncio
import unittest

from fastapi import HTTPException

import app as backend


class RobotWorkflowTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.original_step = backend.SIM_STEP_SECONDS
        self.original_timeout = backend.DELIVERY_UNLOAD_TIMEOUT_SECONDS
        backend.SIM_STEP_SECONDS = 0.01
        backend.DELIVERY_UNLOAD_TIMEOUT_SECONDS = 0.05
        await backend.reset_state()

    async def asyncTearDown(self) -> None:
        await backend.reset_state()
        backend.SIM_STEP_SECONDS = self.original_step
        backend.DELIVERY_UNLOAD_TIMEOUT_SECONDS = self.original_timeout

    async def wait_for_status(self, task_id: str, status: str, timeout: float = 0.3) -> dict:
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            task = backend.get_task(task_id, None)
            if task["status"] == status:
                return task
            await asyncio.sleep(0.005)
        self.fail(f"task {task_id} did not reach {status}")

    async def create_navigation(
        self,
        event_id: str,
        point_id: str,
        purpose: str,
        replace_task_id: str | None = None,
    ) -> dict:
        return await backend.create_navigation(
            backend.NavigateRequest(
                event_id=event_id,
                device_id="panel-001",
                site_id="site-a",
                point_id=point_id,
                purpose=purpose,
                replace_task_id=replace_task_id,
            ),
            None,
        )

    async def test_initial_status_is_idle_in_lobby(self) -> None:
        status = await backend.robot_status(None)
        self.assertEqual(status["state"], "idle")
        self.assertEqual(status["current_point"]["id"], "lobby")
        self.assertIsNone(status["active_task"])

    async def test_delivery_manual_completion(self) -> None:
        pickup = await self.create_navigation("pickup-1", "warehouse", "delivery_pickup")
        await self.wait_for_status(pickup["id"], "awaiting_load")
        status = await backend.robot_status(None)
        self.assertEqual(status["current_point"]["id"], "warehouse")
        self.assertEqual(status["phase"], "awaiting_load")

        loaded = await backend.load_complete(
            pickup["id"], backend.TaskActionRequest(event_id="loaded-1", device_id="panel-001"), None
        )
        self.assertEqual(loaded["status"], "awaiting_destination")

        delivery = await backend.create_delivery(
            backend.DeliveryRequest(
                event_id="delivery-1",
                device_id="panel-001",
                pickup_task_id=pickup["id"],
                destination_point_id="finish",
            ),
            None,
        )
        await self.wait_for_status(delivery["id"], "awaiting_unload")
        completed = await backend.unload_complete(
            delivery["id"], backend.TaskActionRequest(event_id="unload-1", device_id="panel-001"), None
        )
        self.assertEqual(completed["status"], "completed")
        status = await backend.robot_status(None)
        self.assertEqual(status["state"], "idle")
        self.assertEqual(status["current_point"]["id"], "finish")

    async def test_delivery_auto_completes_after_timeout(self) -> None:
        pickup = await self.create_navigation("pickup-2", "warehouse", "delivery_pickup")
        await self.wait_for_status(pickup["id"], "awaiting_load")
        await backend.load_complete(
            pickup["id"], backend.TaskActionRequest(event_id="loaded-2", device_id="panel-001"), None
        )
        delivery = await backend.create_delivery(
            backend.DeliveryRequest(
                event_id="delivery-2",
                device_id="panel-001",
                pickup_task_id=pickup["id"],
                destination_point_id="finish",
            ),
            None,
        )
        await self.wait_for_status(delivery["id"], "awaiting_unload")
        await self.wait_for_status(delivery["id"], "completed")
        self.assertEqual((await backend.robot_status(None))["state"], "idle")

    async def test_inspection_flow_and_idempotency(self) -> None:
        arrival = await self.create_navigation("inspect-nav", "line-1", "inspection")
        duplicate = await self.create_navigation("inspect-nav", "line-1", "inspection")
        self.assertEqual(arrival["id"], duplicate["id"])
        await self.wait_for_status(arrival["id"], "arrived")
        inspection = await backend.create_inspection(
            backend.InspectionRequest(
                event_id="inspect-run",
                device_id="panel-001",
                arrival_task_id=arrival["id"],
                inspection_id="general",
            ),
            None,
        )
        await self.wait_for_status(inspection["id"], "completed")
        self.assertEqual((await backend.robot_status(None))["state"], "idle")

    async def test_busy_rejects_and_matching_replacement_cancels_old_runner(self) -> None:
        old = await self.create_navigation("old", "warehouse", "delivery_pickup")
        with self.assertRaises(HTTPException) as busy:
            await self.create_navigation("blocked", "line-1", "inspection")
        self.assertEqual(busy.exception.status_code, 409)

        new = await self.create_navigation("new", "line-2", "inspection", old["id"])
        self.assertEqual(backend.get_task(old["id"], None)["status"], "cancelled")
        await self.wait_for_status(new["id"], "arrived")
        await asyncio.sleep(0.05)
        self.assertEqual((await backend.robot_status(None))["active_task"]["id"], new["id"])
        self.assertEqual((await backend.robot_status(None))["current_point"]["id"], "line-2")

    async def test_stale_replacement_and_same_destination_are_rejected(self) -> None:
        pickup = await self.create_navigation("pickup-3", "warehouse", "delivery_pickup")
        with self.assertRaises(HTTPException) as stale:
            await self.create_navigation("stale", "line-1", "inspection", "not-active")
        self.assertEqual(stale.exception.status_code, 409)

        await self.wait_for_status(pickup["id"], "awaiting_load")
        await backend.load_complete(
            pickup["id"], backend.TaskActionRequest(event_id="loaded-3", device_id="panel-001"), None
        )
        with self.assertRaises(HTTPException) as same_point:
            await backend.create_delivery(
                backend.DeliveryRequest(
                    event_id="delivery-3",
                    device_id="panel-001",
                    pickup_task_id=pickup["id"],
                    destination_point_id="warehouse",
                ),
                None,
            )
        self.assertEqual(same_point.exception.status_code, 422)


if __name__ == "__main__":
    unittest.main()
