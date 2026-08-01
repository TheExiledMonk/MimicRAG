from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from dataclasses import asdict
import json
import secrets
import time
import uuid
from typing import Any

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field

from .config import RagConfig, load_config
from .runtime import RagRuntime
from .security import PolicyError, RateLimitError


class IngestRequest(BaseModel):
    text: str
    source_uri: str
    tenant_id: str = "default"
    title: str = ""
    metadata: dict[str, Any] = Field(default_factory=dict)
    document_id: str = ""
    background: bool = True


class RetrieveRequest(BaseModel):
    query: str
    tenant_id: str = "default"
    access_scope: str = "public"
    top_k: int = Field(default=10, ge=1, le=100)


class AnswerRequest(RetrieveRequest):
    stream: bool = False
    options: dict[str, Any] = Field(default_factory=dict)


class ChatMessage(BaseModel):
    role: str
    content: str


class OpenAIChatRequest(BaseModel):
    model: str | None = None
    messages: list[ChatMessage]
    stream: bool = False
    temperature: float | None = None
    max_tokens: int | None = None
    tenant_id: str = "default"
    access_scope: str = "public"
    top_k: int = Field(default=10, ge=1, le=100)


def _hit(hit) -> dict[str, Any]:
    return {"chunk": asdict(hit.chunk), "score": hit.score, "vector_rank": hit.vector_rank, "lexical_rank": hit.lexical_rank}


def _model_values(model: BaseModel) -> dict[str, Any]:
    return model.model_dump() if hasattr(model, "model_dump") else model.dict()


def _next_item(iterator):
    try:
        return True, next(iterator)
    except StopIteration:
        return False, None


def create_app(config: RagConfig, runtime: RagRuntime | None = None) -> FastAPI:
    service = runtime or RagRuntime(config)

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        yield
        service.close()

    app = FastAPI(title="MimicRAG", version="0.1.0", lifespan=lifespan)
    app.state.runtime = service
    started = time.time()

    def authorize(request: Request) -> None:
        expected = config.server.resolved_api_key()
        supplied = request.headers.get("authorization", "")
        if supplied.lower().startswith("bearer "):
            supplied = supplied[7:]
        if expected and not secrets.compare_digest(supplied, expected):
            raise HTTPException(401, "invalid API key")
        identity = supplied or (request.client.host if request.client else "local")
        try:
            service.rate_limiter.check(identity)
        except RateLimitError as exc:
            raise HTTPException(429, str(exc)) from exc

    async def call(action):
        try:
            return await asyncio.to_thread(action)
        except PolicyError as exc:
            raise HTTPException(413, str(exc)) from exc
        except ValueError as exc:
            raise HTTPException(400, str(exc)) from exc

    @app.get("/health")
    async def health():
        return {"status": "ok", "uptime_seconds": time.time() - started}

    @app.get("/ready")
    async def ready():
        return {"ready": True, "pending_jobs": service.jobs.pending(), "embedding_model_key": service.indexer.model_key}

    @app.post("/v1/documents")
    async def ingest(body: IngestRequest, request: Request):
        authorize(request)
        result, job = await call(lambda: service.ingest(**_model_values(body)))
        return {**asdict(result), "job_id": job.job_id if job else None}

    @app.get("/v1/jobs/{job_id}")
    async def job(job_id: str, request: Request):
        authorize(request)
        value = service.jobs.get(job_id)
        if value is None:
            raise HTTPException(404, "job not found")
        return service.jobs.snapshot(value)

    @app.post("/v1/retrieve")
    async def retrieve(body: RetrieveRequest, request: Request):
        authorize(request)
        hits, trace_id = await call(lambda: service.retrieve(body.query, body.tenant_id, body.access_scope, body.top_k))
        return {"hits": [_hit(hit) for hit in hits], "trace_id": trace_id}

    def answer_payload(result):
        return {"answer": result.answer, "citations": [asdict(item) for item in result.context.citations], "trace_id": result.trace_id, "hits": [_hit(hit) for hit in result.hits]}

    async def sse_answer(body: AnswerRequest, request: Request, openai: bool = False, conversation: list[dict[str, str]] | None = None):
        iterator = service.answer_stream(body.query, body.tenant_id, body.access_scope, body.top_k, body.options, conversation)
        completion_id = "chatcmpl-" + uuid.uuid4().hex
        while True:
            if await request.is_disconnected():
                iterator.close()
                return
            present, item = await asyncio.to_thread(_next_item, iterator)
            if not present:
                break
            token, _, trace_id = item
            if openai:
                payload = {"id": completion_id, "object": "chat.completion.chunk", "choices": [{"index": 0, "delta": {"content": token}, "finish_reason": None}]}
            else:
                payload = {"type": "token", "token": token, "trace_id": trace_id}
            yield "data: " + json.dumps(payload) + "\n\n"
        yield "data: [DONE]\n\n"

    @app.post("/v1/answers")
    async def answer(body: AnswerRequest, request: Request):
        authorize(request)
        if body.stream:
            return StreamingResponse(sse_answer(body, request), media_type="text/event-stream")
        result = await call(lambda: service.answer(body.query, body.tenant_id, body.access_scope, body.top_k, body.options))
        return answer_payload(result)

    @app.post("/v1/chat/completions")
    async def chat_completions(body: OpenAIChatRequest, request: Request):
        authorize(request)
        query = next((message.content for message in reversed(body.messages) if message.role == "user"), "")
        if not query:
            raise HTTPException(400, "a user message is required")
        options = {}
        if body.temperature is not None:
            options["temperature"] = body.temperature
        if body.max_tokens is not None:
            options["max_tokens"] = body.max_tokens
        answer_body = AnswerRequest(query=query, tenant_id=body.tenant_id, access_scope=body.access_scope, top_k=body.top_k, stream=body.stream, options=options)
        conversation = [_model_values(message) for message in body.messages]
        if body.stream:
            return StreamingResponse(sse_answer(answer_body, request, openai=True, conversation=conversation), media_type="text/event-stream")
        result = await call(lambda: service.answer(query, body.tenant_id, body.access_scope, body.top_k, options, conversation))
        return {"id": "chatcmpl-" + uuid.uuid4().hex, "object": "chat.completion", "created": int(time.time()), "model": config.chat.model, "choices": [{"index": 0, "message": {"role": "assistant", "content": result.answer}, "finish_reason": "stop"}], "mimicrag": {"trace_id": result.trace_id, "citations": [asdict(item) for item in result.context.citations]}}

    @app.get("/v1/traces/{trace_id}")
    async def trace(trace_id: str, request: Request):
        authorize(request)
        value = service.traces.get(trace_id)
        if value is None:
            raise HTTPException(404, "trace not found")
        return asdict(value)

    return app


def app_from_environment() -> FastAPI:
    import os
    path = os.environ.get("MIMICRAG_CONFIG", "mimicrag.json")
    return create_app(load_config(path))
