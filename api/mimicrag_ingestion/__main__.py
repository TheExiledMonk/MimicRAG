from __future__ import annotations

import argparse
import json

from .pipeline import IngestionPipeline, MimicRagSink, ModelRoute
from .sources import DirectorySource, GitSource, S3Source, SitemapSource


def main() -> int:
    parser = argparse.ArgumentParser(description="Synchronize external sources into MimicRAG")
    parser.add_argument("kind", choices=("directory", "sitemap", "git", "s3"))
    parser.add_argument("location", help="path, URL, repository, or bucket")
    parser.add_argument("--server", default="http://127.0.0.1:8080")
    parser.add_argument("--api-key", default="")
    parser.add_argument("--tenant", default="default")
    parser.add_argument("--state", default=".mimicrag-ingestion.json")
    parser.add_argument("--prefix", default="", help="S3 prefix or Git subdirectory")
    parser.add_argument("--endpoint-url", default=None, help="S3-compatible endpoint")
    parser.add_argument("--ref", default="HEAD")
    parser.add_argument("--bearer-token", default="")
    parser.add_argument("--watch", action="store_true")
    parser.add_argument("--interval", type=float, default=2.0)
    args = parser.parse_args()
    if args.kind == "directory": source = DirectorySource(args.location)
    elif args.kind == "sitemap": source = SitemapSource(args.location, bearer_token=args.bearer_token)
    elif args.kind == "git": source = GitSource(args.location, ref=args.ref, subdirectory=args.prefix)
    else: source = S3Source(args.location, prefix=args.prefix, endpoint_url=args.endpoint_url)
    pipeline = IngestionPipeline(MimicRagSink(args.server, args.api_key), default_route=ModelRoute("multilingual"))
    if args.watch:
        pipeline.watch(source, state_path=args.state, tenant_id=args.tenant, interval_seconds=args.interval)
        return 0
    manifest = pipeline.sync(source.objects(), state_path=args.state, tenant_id=args.tenant)
    print(json.dumps(manifest.to_dict(), indent=2, ensure_ascii=False))
    return 1 if any(entry.status == "rejected" for entry in manifest.entries) else 0


if __name__ == "__main__":
    raise SystemExit(main())
