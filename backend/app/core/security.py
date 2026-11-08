"""Gateway (ESP32) authentication: one bearer token per device."""

import hmac

from fastapi import Depends, HTTPException, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

from app.core.config import Settings, get_settings

_bearer = HTTPBearer(auto_error=False)


def require_gateway(
    creds: HTTPAuthorizationCredentials | None = Depends(_bearer),
    settings: Settings = Depends(get_settings),
) -> str:
    """Returns the authenticated device_id, or raises 401."""
    if creds is None or creds.scheme.lower() != "bearer":
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Missing bearer token")
    presented = creds.credentials
    device_id = None
    # Compare against every token in constant time so timing leaks nothing.
    for token, dev in settings.gateway_token_map().items():
        if hmac.compare_digest(presented.encode(), token.encode()):
            device_id = dev
    if device_id is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Invalid token")
    return device_id


def require_admin(
    creds: HTTPAuthorizationCredentials | None = Depends(_bearer),
    settings: Settings = Depends(get_settings),
) -> None:
    expected = settings.admin_token.get_secret_value()
    if len(expected) < 24:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "Admin API disabled (set ADMIN_TOKEN, >= 24 chars)")
    if creds is None or not hmac.compare_digest(creds.credentials.encode(), expected.encode()):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Invalid admin token")
