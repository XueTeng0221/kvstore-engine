"""vLLM 0.29.0 connector entry point for the KVStore adapter."""

def __getattr__(name):
    if name == "KVStoreConnector":
        from .connector import KVStoreConnector
        return KVStoreConnector
    if name == "VllmSessionBridge":
        from .bridge import VllmSessionBridge
        return VllmSessionBridge
    raise AttributeError(name)
from .uds import Session, SessionError, SessionTransport
from .protocol import TensorManifest

__all__ = ["KVStoreConnector", "Session", "SessionError", "SessionTransport", "TensorManifest",
           "VllmSessionBridge"]

def __dir__():
    return __all__
