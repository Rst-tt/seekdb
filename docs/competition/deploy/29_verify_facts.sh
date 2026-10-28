#!/bin/bash
/home/rst/seekdb-env/bin/python - <<'PY'
import datetime
us = 1793146992399022
t = datetime.datetime.fromtimestamp(us/1e6, datetime.timezone(datetime.timedelta(hours=8)))
print("bootstrap time (+08:00):", t.isoformat())
PY
echo
docker inspect seekdb-docker --format 'name={{.Name}} status={{.State.Status}} started={{.State.StartedAt}} image={{.Image}}'
echo
cat /var/lib/seekdb-docker/run/telemetry.json | /home/rst/seekdb-env/bin/python -c "import json,sys; print('id=', json.load(sys.stdin)['content']['id'])"
