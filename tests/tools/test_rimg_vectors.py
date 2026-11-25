"""tools/rimg.py against the seal and pack vectors in tests/vectors/rimg/.

The vectors are the contract of docs/spec/IMAGE_FORMAT.md, section Test
vectors. A C sealer must reproduce the same bytes and codes.
"""

import filecmp
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VEC = os.path.join(ROOT, "tests", "vectors", "rimg")
TOOL = os.path.join(ROOT, "tools", "rimg.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rimg  # noqa: E402

KEYS = {"op", "input", "magic", "subtype", "load_addr", "name", "image_version", "abi_version",
        "mem_size", "entry", "meta", "title", "icon", "icon_format", "expect", "output"}


def unquote(value):
    """The value as ini.h reads it: quotes kept literal inside, three escapes."""
    if not value.startswith('"'):
        return value
    assert value.endswith('"') and len(value) >= 2, value
    out, i, body = [], 0, value[1:-1]
    while i < len(body):
        c = body[i]
        if c == "\\":
            out.append({"\\": "\\", '"': '"', "n": "\n"}[body[i + 1]])
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_vectors():
    """Parses vectors.ini with the subset of ini.h syntax the file uses."""
    vectors, cur = [], None
    with open(os.path.join(VEC, "vectors.ini"), encoding="utf-8") as f:
        for line in f.read().split("\n"):
            line = line.strip()
            if not line or line[0] in "#;":
                continue
            if line.startswith("[vector ") and line.endswith("]"):
                cur = {"id": line[8:-1].strip()}
                vectors.append(cur)
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            assert key in KEYS and key not in cur, f"{cur['id']}: key {key!r}"
            cur[key] = unquote(value.strip())
    return vectors


VECTORS = read_vectors()


def load(name):
    with open(os.path.join(VEC, name), "rb") as f:
        return f.read()


def apply(v):
    """Runs one vector through tools/rimg.py. Returns (code name, bytes or None)."""
    try:
        icon = None
        if "icon" in v:
            icon = rimg.parse_icon(load(v["icon"]), v.get("icon_format", "auto"))
        version = None if "image_version" not in v else rimg.parse_version(v["image_version"])
        data = load(v["input"])
        if v["op"] == "seal":
            out = rimg.seal(data, v.get("name"), version, v.get("title"), icon)
        else:
            assert v["op"] == "pack", v
            out = rimg.pack(data, v["magic"], int(v["subtype"], 0), int(v["load_addr"], 0),
                            v.get("name"), version or 0,
                            rimg.parse_abi(v["abi_version"]) if "abi_version" in v else None,
                            int(v["mem_size"], 0) if "mem_size" in v else None,
                            {"yes": True, "no": False}.get(v.get("meta")), v.get("title"),
                            icon, int(v.get("entry", "0"), 0))
        return "ok", out
    except rimg.RimgError as e:
        return rimg.ERR_NAMES[e.code], None


@pytest.mark.parametrize("v", VECTORS, ids=[v["id"] for v in VECTORS])
def test_vector(v):
    code, out = apply(v)
    assert code == v["expect"]
    if code == "ok":
        assert out == load(v["output"])
        assert rimg.validate(out)
    else:
        assert "output" not in v


def test_vectors_cover_every_code_a_sealer_returns():
    codes = {v["expect"] for v in VECTORS}
    assert codes == {"ok", "io", "magic", "version", "size", "bounds", "abi"}
    assert {v["op"] for v in VECTORS} == {"seal", "pack"}


def test_generator_reproduces_the_committed_files(tmp_path):
    subprocess.run([sys.executable, os.path.join(VEC, "generate.py"), str(tmp_path)],
                   check=True)
    made = sorted(os.listdir(tmp_path))
    committed = sorted(n for n in os.listdir(VEC) if n != "generate.py" and n != "__pycache__")
    assert made == committed
    match, mismatch, errors = filecmp.cmpfiles(VEC, tmp_path, made, shallow=False)
    assert (mismatch, errors) == ([], [])


@pytest.mark.parametrize("v", [v for v in VECTORS if v["expect"] == "ok"],
                         ids=[v["id"] for v in VECTORS if v["expect"] == "ok"])
def test_command_line_gives_the_same_bytes(tmp_path, v):
    out = tmp_path / "out.img"
    args = [sys.executable, TOOL, v["op"], os.path.join(VEC, v["input"]), "-o", str(out)]
    if v["op"] == "pack":
        args += ["--magic", v["magic"], "--subtype", v["subtype"], "--load-addr", v["load_addr"]]
    for key in ("name", "image_version", "abi_version", "mem_size", "entry", "title",
                "icon_format"):
        if key in v:
            args += ["--" + key.replace("_", "-"), v[key]]
    if "icon" in v:
        args += ["--icon", os.path.join(VEC, v["icon"])]
    if "meta" in v:
        args.append("--meta" if v["meta"] == "yes" else "--no-meta")
    r = subprocess.run(args, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert out.read_bytes() == load(v["output"])
