from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.api import router
from app.core_monitor import start as start_core_monitor
from app.core_monitor import stop as stop_core_monitor
from app.db import init_db
from app.public_monitor import start as start_public_monitor
from app.public_monitor import stop as stop_public_monitor


def configure_logging() -> None:
    import logging
    import sys

    root = logging.getLogger()
    root.setLevel(logging.INFO)
    if not root.handlers:
        handler = logging.StreamHandler(sys.stderr)
        handler.setFormatter(
            logging.Formatter("%(asctime)s %(levelname)s %(name)s %(message)s")
        )
        root.addHandler(handler)


configure_logging()


@asynccontextmanager
async def lifespan(_app: FastAPI):
    start_core_monitor()
    start_public_monitor()
    yield
    stop_core_monitor()
    stop_public_monitor()


def create_app() -> FastAPI:
    init_db()
    app = FastAPI(title="dBmap Operations", version="0.1.0", lifespan=lifespan)
    app.include_router(router)
    return app


app = create_app()
