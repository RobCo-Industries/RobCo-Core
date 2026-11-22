# robco-ci image

CI for RobCo-Core and the sibling repos runs in this container. It pins
QEMU 10.1.2, the version our tests are verified against. Ubuntu's apt
QEMU is 8.x and its raspi machine models differ.

## Contents

- Ubuntu 24.04
- QEMU 10.1.2 in `/usr/local`: `qemu-system-arm` and
  `qemu-system-aarch64` only, built from the release tarball with a
  pinned SHA-256. No GUI, no docs, no tools, no slirp. Pixman is
  enabled because QMP `screendump` needs it; the image build checks it.
- `gcc-arm-none-eabi`, `binutils-arm-none-eabi`
- `make`, `git`, `mtools`, `dosfstools`
- `python3`, `python3-venv`, `python3-pip`, `ca-certificates`

## Tag

The tag is the first 12 hex characters of the SHA-256 of `ci/Dockerfile`.
`ci/image-tag.sh` prints it. `ci/IMAGE_TAG` stores it, and CI fails if
the two differ. After editing the Dockerfile, run:

    ci/image-tag.sh > ci/IMAGE_TAG

CI pushes `ghcr.io/robco-industries/robco-ci:<tag>` when the tag is new,
and also `:latest` on `main`. Sibling repos read the tag from
`ci/IMAGE_TAG` in their RobCo-Core submodule and pass it to
`.github/workflows/robco-ci.yml`.

## Build locally

    docker build -f ci/Dockerfile -t robco-ci:local ci

The QEMU build takes several minutes. To run a shell in the image:

    docker run --rm -it -v "$PWD:/work" -w /work robco-ci:local bash
