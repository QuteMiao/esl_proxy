#!/usr/bin/env bash
# Build and run esl_proxy onboard on a real NPU via task-submit (root queue).
# Device id comes from task-submit ($TASK_DEVICE); do not pass -d to run_onboard.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASCEND_ENV="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.0.0}/bin/setenv.bash"
SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_BUILD_FLAG=""
if [[ "$SKIP_BUILD" == "1" ]]; then
  SKIP_BUILD_FLAG="--skip-build"
fi

exec task-submit --device auto --max-time 0 --timeout 3600 \
  --env ASCEND_HOME_PATH --env PATH --env LD_LIBRARY_PATH --env HOME --env USER \
  --run "source '${ASCEND_ENV}' && cd '${ROOT}' && bash tools/run_onboard.sh ${SKIP_BUILD_FLAG}"
