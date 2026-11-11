#!/bin/sh
# Host-only FastRPC/GLINK wire fixtures.  This never opens USB or touches a phone.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
B=${TMPDIR:-/tmp}/hackios-fastrpc-wire-test-$$
trap 'rm -rf "$B"' EXIT HUP INT TERM
mkdir -p "$B"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$HERE" -o "$B/test" "$HERE/test_wire.c"
"$B/test"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$HERE/test_route.c" -o "$B/test_route"
"$B/test_route"
