"""A minimal subprocess environment built from an allowlist.

Never put a full copy of the host environment in a test value. pytest prints
arguments and locals when a test fails, so a copy of os.environ ends up in the
terminal, in saved logs and in CI logs, secrets included.

minimal_env() copies only the variables the tools need. Pass the rest as
explicit extras. network_env() adds the proxy and CA settings for the few
tests that reach the network. They stay out of ALLOWED because a proxy URL
can hold a user name and password. The returned mapping hides its values in repr() as a second
guard, so a traceback shows the variable names only.
"""

import os

ALLOWED = (
    # Process basics.
    "PATH", "HOME", "TMPDIR", "USER", "LOGNAME", "SHELL",
    "LANG", "LC_ALL", "LC_CTYPE", "TERM",
    # macOS toolchain lookup (xcrun, cc).
    "DEVELOPER_DIR", "SDKROOT",
    # Python in the venv.
    "VIRTUAL_ENV",
    # Build overrides the Makefiles read with ?=.
    "CI", "PYTHON", "CROSS", "HOST_CC", "HOST_SANITIZE",
    # Project switches the tests and the harness read.
    "ROBCO_CORE", "ROBCO_QEMU_KEEPALIVE", "ROBCO_NET_TESTS", "ROBCO_UPDATE_GOLDEN",
    "MTOOLS_SKIP_CHECK",
)


class Env(dict):
    """A dict that prints its keys but never its values."""

    def __repr__(self):
        return f"Env({sorted(self)})"

    __str__ = __repr__


def minimal_env(**extra):
    """Allowlisted host variables plus `extra`. A None extra removes the variable."""
    env = Env((k, os.environ[k]) for k in ALLOWED if k in os.environ)
    for k, v in extra.items():
        if v is None:
            env.pop(k, None)
        else:
            env[k] = str(v)
    return env


NETWORK = tuple(n for p in ("HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY", "NO_PROXY")
                for n in (p, p.lower())) + ("SSL_CERT_FILE", "SSL_CERT_DIR", "CURL_CA_BUNDLE")


def network_env(**extra):
    """minimal_env(**extra) plus the host proxy and CA settings that are set."""
    env = minimal_env()
    env.update((k, os.environ[k]) for k in NETWORK if k in os.environ)
    for k, v in extra.items():
        if v is None:
            env.pop(k, None)
        else:
            env[k] = str(v)
    return env


def prepend_path(directory):
    """The allowlisted PATH with `directory` in front."""
    return f"{directory}{os.pathsep}{os.environ.get('PATH', os.defpath)}"
