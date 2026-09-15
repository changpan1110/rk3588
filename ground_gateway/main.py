from __future__ import annotations

import asyncio
import logging
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from .admin_config import (
    PLAYER_CONFIG_PATH,
    PlayerConfigError,
    load_player_config,
    save_player_config,
)
from .config import GatewaySettings
from .gateway_core import (
    ClientSequenceTracker,
    CommandResultCache,
    CommandValidationError,
    ControlCommand,
)
from .mavlink_transport import create_transport


settings = GatewaySettings.from_env()
result_cache = CommandResultCache()
sequence_tracker = ClientSequenceTracker()
command_lock = asyncio.Lock()
transport = create_transport(settings)
logger = logging.getLogger("uvicorn.error")
if settings.debug:
    logger.setLevel(logging.DEBUG)

app = FastAPI(title="RK3588 Ground MAVLink Gateway", version="1.0.0")
admin_directory = Path(__file__).resolve().parent / "admin"
app.mount(
    "/admin/assets",
    StaticFiles(directory=admin_directory),
    name="admin-assets",
)


def make_ack(
    command: ControlCommand | None,
    ok: bool,
    message: str,
    **extra: Any,
) -> dict[str, Any]:
    response: dict[str, Any] = {
        "type": "ack",
        "command_id": command.command_id if command else None,
        "sequence": command.sequence if command else None,
        "ok": ok,
        "message": message,
    }
    response.update(extra)
    return response


@app.get("/health")
async def health() -> dict[str, Any]:
    return {
        "ok": True,
        "dry_run": settings.dry_run,
        "control_debug": settings.debug,
        "mavlink_endpoint": settings.mavlink_endpoint,
        "mavlink_baud": settings.mavlink_baud,
        "target_system": settings.target_system,
        "target_component": settings.target_component,
    }


@app.get("/admin", include_in_schema=False)
async def admin_page() -> FileResponse:
    return FileResponse(
        admin_directory / "index.html",
        headers={"Cache-Control": "no-store"},
    )


@app.get("/admin/", include_in_schema=False)
async def admin_page_slash() -> RedirectResponse:
    return RedirectResponse(url="/admin")


@app.get("/api/admin/config")
async def get_admin_config(request: Request) -> dict[str, Any]:
    try:
        config = load_player_config()
    except PlayerConfigError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return {
        "ok": True,
        "config": config,
        "player_url": _player_url(request, config["web"]["port"]),
        "config_path": str(PLAYER_CONFIG_PATH),
    }


@app.put("/api/admin/config")
async def update_admin_config(
    request: Request, payload: dict[str, Any]
) -> dict[str, Any]:
    try:
        config, restart_fields = save_player_config(payload)
    except PlayerConfigError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except OSError as exc:
        logger.exception("failed to save player configuration")
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    logger.info(
        "player configuration saved path=%s restart_fields=%s",
        PLAYER_CONFIG_PATH,
        restart_fields,
    )
    return {
        "ok": True,
        "config": config,
        "restart_required": bool(restart_fields),
        "restart_fields": restart_fields,
        "player_url": _player_url(request, config["web"]["port"]),
    }


def _player_url(request: Request, port: int) -> str:
    hostname = request.url.hostname or "127.0.0.1"
    if ":" in hostname and not hostname.startswith("["):
        hostname = f"[{hostname}]"
    return f"http://{hostname}:{port}/"


@app.websocket("/ws/control")
async def control_socket(websocket: WebSocket) -> None:
    await websocket.accept()

    try:
        while True:
            payload = await websocket.receive_json()
            await handle_command(websocket, payload)
    except WebSocketDisconnect:
        return


async def handle_command(websocket: WebSocket, payload: dict[str, Any]) -> None:
    command: ControlCommand | None = None

    try:
        if settings.debug:
            logger.debug("control websocket payload=%s", payload)
        command = ControlCommand.from_payload(payload, settings.max_command_age_ms)
        logger.debug(
            "control command received command=%s event=%s value=%s sequence=%s "
            "command_id=%s client_id=%s",
            command.command,
            command.event,
            command.value,
            command.sequence,
            command.command_id,
            command.client_id,
        )

        cached = result_cache.get(command.command_id)
        if cached is not None:
            cached["duplicate"] = True
            logger.debug(
                "control ack cached command=%s sequence=%s command_id=%s ok=%s",
                command.command,
                command.sequence,
                command.command_id,
                cached.get("ok"),
            )
            await websocket.send_json(cached)
            return

        if not sequence_tracker.accept(command.client_id, command.sequence):
            response = make_ack(command, False, "sequence replay rejected", error="sequence_replay")
            result_cache.put(command.command_id, response)
            logger.warning(
                "control sequence replay rejected client_id=%s sequence=%s command_id=%s",
                command.client_id,
                command.sequence,
                command.command_id,
            )
            await websocket.send_json(response)
            return

        async with command_lock:
            result = await asyncio.to_thread(transport.send_and_wait, command)

        response = make_ack(
            command,
            result.accepted,
            result.result_name,
            mav_result=result.result_code,
            attempts=result.attempts,
        )
    except CommandValidationError as exc:
        response = make_ack(command, False, str(exc), error="invalid_command")
    except TimeoutError as exc:
        response = make_ack(command, False, str(exc), error="ack_timeout")
    except Exception as exc:
        logger.exception("control gateway failure")
        response = make_ack(command, False, str(exc), error="gateway_error")

    if command is not None:
        result_cache.put(command.command_id, response)
        log_ack = logger.debug if response.get("ok") else logger.warning
        log_ack(
            "control ack command=%s sequence=%s command_id=%s ok=%s message=%s attempts=%s",
            command.command,
            command.sequence,
            command.command_id,
            response.get("ok"),
            response.get("message"),
            response.get("attempts", 0),
        )
    else:
        logger.warning("control message rejected message=%s", response.get("message"))
    await websocket.send_json(response)
