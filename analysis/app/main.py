from fastapi import FastAPI

from app.api import router
from app.db import init_db


def create_app() -> FastAPI:
    init_db()
    app = FastAPI(title="dBmap analysis", version="0.1.0")
    app.include_router(router)
    return app


app = create_app()
