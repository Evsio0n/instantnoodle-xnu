#!/bin/bash
set -eu
cd "$(dirname "$0")/../.."
exec python3 tools/sensors/cold_recover.py "$@"
