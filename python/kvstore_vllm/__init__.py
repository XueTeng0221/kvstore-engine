"""vLLM 0.29.0 connector entry point for the KVStore adapter."""

def __getattr__(name):
    if name == "KVStoreConnector":
        from .connector import KVStoreConnector
        return KVStoreConnector
    raise AttributeError(name)
from .uds import SessionTransport

__all__ = ["KVStoreConnector", "SessionTransport"]
