"""SOAP client helper — the "intent -> action" execution
channel settled on this session (see STATE.md "SOAP enabled on
oci-arm1"). Not wired up to anything yet; this is just the
reusable call primitive a future milestone will use to
actually invoke .llmc ahlist/ahbuy from the intent-evaluation
loop.

Request/response shape mirrors what was proven working
manually this session (HTTP Basic auth against the worldserver
SOAP endpoint, a bare XML envelope, <command> holding the exact
console command text with no leading dot).
"""

import base64
import logging
import re
import urllib.error
import urllib.request
from xml.sax.saxutils import escape as xml_escape

logger = logging.getLogger(__name__)

# Deliberately stdlib-only (urllib, not requests) — this
# module isn't in requirements.txt yet and pip-installing a
# new dependency on every bridge start (see the "pip install
# on every start" note in the compose service) is a cost this
# one-off admin call doesn't need to add.

_ENVELOPE = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<SOAP-ENV:Envelope '
    'xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/">'
    '<SOAP-ENV:Body>'
    '<ns1:executeCommand xmlns:ns1="urn:AC">'
    '<command>{command}</command>'
    '</ns1:executeCommand>'
    '</SOAP-ENV:Body>'
    '</SOAP-ENV:Envelope>'
)

_RESULT_RE = re.compile(
    r'<result>(.*?)</result>', re.DOTALL
)
_FAULT_RE = re.compile(
    r'<faultstring>(.*?)</faultstring>', re.DOTALL
)


class SoapError(Exception):
    """Raised when the SOAP endpoint returns a SOAP-ENV:Fault
    (bad command, permission denied, etc.) rather than a
    transport-level failure."""


def call_soap_command(
    host: str,
    port: int,
    username: str,
    password: str,
    command: str,
    timeout: float = 10.0,
) -> str:
    """Run one console command via SOAP, return its <result>
    text. Raises SoapError on a SOAP fault, urllib.error.URLError
    (or a subclass) on a transport-level failure (connection
    refused, timeout).

    `command` is the bare command text with no leading dot,
    e.g. "llmc ahlist Grimjaw 25 5000".
    """
    url = f"http://{host}:{port}/"
    body = _ENVELOPE.format(
        command=xml_escape(command)
    ).encode("utf-8")

    credentials = base64.b64encode(
        f"{username}:{password}".encode("utf-8")
    ).decode("ascii")

    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": "text/xml",
            "Authorization": f"Basic {credentials}",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(
            req, timeout=timeout
        ) as resp:
            text = resp.read().decode(
                "utf-8", errors="replace"
            )
    except urllib.error.HTTPError as exc:
        # SOAP faults come back as a 500 with a fault body,
        # not a plain transport error — still worth parsing.
        text = exc.read().decode(
            "utf-8", errors="replace"
        )

    fault_match = _FAULT_RE.search(text)
    if fault_match:
        raise SoapError(fault_match.group(1).strip())

    result_match = _RESULT_RE.search(text)
    if result_match:
        return result_match.group(1).strip()

    # Some commands (e.g. plain PSendSysMessage output with
    # no <result> wrapper) may not match — return the raw
    # body rather than silently swallowing it.
    return text.strip()
