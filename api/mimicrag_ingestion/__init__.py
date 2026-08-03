"""Optional ingestion ecosystem for MimicRAG.

Connectors normalize sources here and submit plain documents to the native server;
the query engine never imports connector dependencies.
"""

from .adapters import AdapterRegistry, NormalizedDocument, StructuredBlock
from .pipeline import IngestionPipeline, Manifest, MimicRagSink, ModelRoute
from .sources import DirectorySource, GitSource, GoogleDriveSource, SharePointSource, SitemapSource, S3Source

__all__ = [
    "AdapterRegistry", "DirectorySource", "GitSource", "IngestionPipeline",
    "GoogleDriveSource", "Manifest", "MimicRagSink", "ModelRoute", "NormalizedDocument",
    "S3Source", "SharePointSource", "SitemapSource", "StructuredBlock",
]
