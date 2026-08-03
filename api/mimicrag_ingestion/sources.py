from __future__ import annotations

import shutil
import subprocess
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator


@dataclass
class SourceObject:
    uri: str
    data: bytes
    media_type: str = ""
    etag: str = ""
    modified: str = ""
    metadata: dict[str, Any] | None = None


class DirectorySource:
    def __init__(self, path: str | Path, *, recursive: bool = True, include_hidden: bool = False):
        self.path = Path(path).resolve(); self.recursive = recursive; self.include_hidden = include_hidden

    def objects(self) -> Iterator[SourceObject]:
        iterator = self.path.rglob("*") if self.recursive else self.path.glob("*")
        for path in sorted(iterator):
            if not path.is_file() or (not self.include_hidden and any(part.startswith(".") for part in path.relative_to(self.path).parts)):
                continue
            stat = path.stat()
            yield SourceObject(path.as_uri(), path.read_bytes(), modified=str(stat.st_mtime_ns), metadata={"relative_path": str(path.relative_to(self.path))})


class SitemapSource:
    def __init__(self, url: str, *, headers: dict[str, str] | None = None, bearer_token: str = "", maximum_urls: int = 10000):
        self.url = url; self.headers = dict(headers or {}); self.maximum_urls = maximum_urls
        if bearer_token: self.headers["Authorization"] = "Bearer " + bearer_token

    def _get(self, url: str) -> tuple[bytes, dict[str, str]]:
        request = urllib.request.Request(url, headers={**self.headers, "User-Agent": "MimicRAG-Ingestion/1.4"})
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read(), dict(response.headers.items())

    def objects(self) -> Iterator[SourceObject]:
        pending = [self.url]; pages: list[str] = []
        while pending and len(pages) < self.maximum_urls:
            url = pending.pop(0); payload, _ = self._get(url); root = ET.fromstring(payload)
            locations = [(element.text or "").strip() for element in root.iter() if element.tag.endswith("loc")]
            if root.tag.endswith("sitemapindex"): pending.extend(locations)
            else: pages.extend(locations)
        for url in pages[: self.maximum_urls]:
            payload, headers = self._get(url)
            yield SourceObject(url, payload, headers.get("Content-Type", ""), headers.get("ETag", ""), headers.get("Last-Modified", ""))


class S3Source:
    def __init__(self, bucket: str, *, prefix: str = "", endpoint_url: str | None = None, client: Any = None):
        if client is None:
            try: import boto3  # type: ignore
            except ImportError as exc: raise RuntimeError("S3 ingestion requires optional 'boto3'") from exc
            client = boto3.client("s3", endpoint_url=endpoint_url)
        self.client = client; self.bucket = bucket; self.prefix = prefix

    def objects(self) -> Iterator[SourceObject]:
        token: str | None = None
        while True:
            args = {"Bucket": self.bucket, "Prefix": self.prefix}
            if token: args["ContinuationToken"] = token
            page = self.client.list_objects_v2(**args)
            for item in page.get("Contents", []):
                body = self.client.get_object(Bucket=self.bucket, Key=item["Key"])
                yield SourceObject(f"s3://{self.bucket}/{item['Key']}", body["Body"].read(),
                    body.get("ContentType", ""), str(item.get("ETag", "")).strip('"'), str(item.get("LastModified", "")))
            if not page.get("IsTruncated"): break
            token = page.get("NextContinuationToken")


class GitSource:
    def __init__(self, repository: str, *, ref: str = "HEAD", subdirectory: str = ""):
        self.repository = repository; self.ref = ref; self.subdirectory = subdirectory

    def objects(self) -> Iterator[SourceObject]:
        temporary = Path(tempfile.mkdtemp(prefix="mimicrag-git-"))
        try:
            subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout", "--", self.repository, str(temporary)], check=True, capture_output=True)
            subprocess.run(["git", "-C", str(temporary), "checkout", self.ref, "--", self.subdirectory or "."], check=True, capture_output=True)
            commit = subprocess.run(["git", "-C", str(temporary), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
            root = (temporary / self.subdirectory).resolve()
            for item in DirectorySource(root).objects():
                relative = item.metadata["relative_path"] if item.metadata else ""
                yield SourceObject(f"git+{self.repository}@{commit}/{relative}", item.data, modified=commit, metadata={"repository": self.repository, "commit": commit, "path": relative})
        finally:
            shutil.rmtree(temporary, ignore_errors=True)


class GoogleDriveSource:
    """Adapter for an injected Google Drive v3 service; google packages stay optional."""
    def __init__(self, service: Any, query: str = "trashed = false"):
        self.service = service; self.query = query

    def objects(self) -> Iterator[SourceObject]:
        exports = {"application/vnd.google-apps.document": ("text/plain", ".txt"),
                   "application/vnd.google-apps.spreadsheet": ("text/csv", ".csv"),
                   "application/vnd.google-apps.presentation": ("text/plain", ".txt")}
        token = None
        while True:
            result = self.service.files().list(q=self.query, fields="nextPageToken,files(id,name,mimeType,md5Checksum,modifiedTime)", pageToken=token).execute()
            for item in result.get("files", []):
                media_type = item.get("mimeType", "")
                if media_type in exports:
                    exported_type, suffix = exports[media_type]
                    data = self.service.files().export_media(fileId=item["id"], mimeType=exported_type).execute()
                    name = item.get("name", "") + suffix; media_type = exported_type
                else:
                    data = self.service.files().get_media(fileId=item["id"]).execute(); name = item.get("name", "")
                yield SourceObject("gdrive://" + item["id"], data, media_type, item.get("md5Checksum", ""), item.get("modifiedTime", ""), {"name": name})
            token = result.get("nextPageToken")
            if not token: break


class SharePointSource:
    """Microsoft Graph drive adapter using an injected authenticated client."""
    def __init__(self, client: Any, drive_id: str, folder: str = "root"):
        self.client = client; self.drive_id = drive_id; self.folder = folder

    def objects(self) -> Iterator[SourceObject]:
        url = f"/drives/{self.drive_id}/items/{self.folder}/children"
        while url:
            page = self.client.get(url)
            if hasattr(page, "json"): page = page.json()
            for item in page.get("value", []):
                if "file" not in item: continue
                content = self.client.get(f"/drives/{self.drive_id}/items/{item['id']}/content")
                data = content.content if hasattr(content, "content") else content
                yield SourceObject("sharepoint://" + self.drive_id + "/" + item["id"], data,
                    item.get("file", {}).get("mimeType", ""), item.get("eTag", ""), item.get("lastModifiedDateTime", ""), {"name": item.get("name", "")})
            url = page.get("@odata.nextLink", "")
