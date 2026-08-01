from __future__ import annotations

from dataclasses import asdict, dataclass, field
from queue import Empty, Queue
import threading
import time
import uuid
from typing import Callable, Any


@dataclass
class Job:
    job_id: str
    kind: str
    status: str = "queued"
    created_at_ms: int = field(default_factory=lambda: int(time.time() * 1000))
    started_at_ms: int = 0
    finished_at_ms: int = 0
    result: Any = None
    error: str = ""


class JobRunner:
    def __init__(self, workers: int = 1) -> None:
        self._queue: Queue[tuple[Job, Callable[[], Any]] | None] = Queue()
        self._jobs: dict[str, Job] = {}
        self._lock = threading.Lock()
        self._threads = [threading.Thread(target=self._work, name=f"mimicrag-job-{i}", daemon=True) for i in range(max(1, workers))]
        for thread in self._threads:
            thread.start()

    def submit(self, kind: str, action: Callable[[], Any]) -> Job:
        job = Job(uuid.uuid4().hex, kind)
        with self._lock:
            self._jobs[job.job_id] = job
        self._queue.put((job, action))
        return job

    def get(self, job_id: str) -> Job | None:
        with self._lock:
            return self._jobs.get(job_id)

    def snapshot(self, job: Job) -> dict[str, Any]:
        with self._lock:
            return asdict(job)

    def pending(self) -> int:
        return self._queue.qsize()

    def close(self) -> None:
        for _ in self._threads:
            self._queue.put(None)
        for thread in self._threads:
            thread.join(timeout=2.0)

    def _work(self) -> None:
        while True:
            item = self._queue.get()
            if item is None:
                self._queue.task_done()
                return
            job, action = item
            try:
                job.status = "running"
                job.started_at_ms = int(time.time() * 1000)
                value = action()
                job.result = asdict(value) if hasattr(value, "__dataclass_fields__") else value
                job.status = "complete"
            except Exception as exc:
                job.error = str(exc)
                job.status = "failed"
            finally:
                job.finished_at_ms = int(time.time() * 1000)
                self._queue.task_done()
