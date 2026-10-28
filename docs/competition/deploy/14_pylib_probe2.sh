#!/bin/bash
P=/home/rst/seekdb-env/lib/python3.12/site-packages/pylibseekdb
echo "=== env/socket strings ==="
strings -n 6 "$P/pylibseekdb.abi3.so" | grep -iE 'SEEKDB_[A-Z_]+|uds|sql\.sock|PYLIB' | sort -u | head -40
echo "=== api names ==="
strings -n 6 "$P/pylibseekdb.abi3.so" | grep -E '^(connection_options|close|open|aopen|SeekdbInstance|execute|query|SeekdbError)$' | sort -u
echo "=== python dir() ==="
/home/rst/seekdb-env/bin/python - <<'PY'
import pylibseekdb as s
print([n for n in dir(s) if not n.startswith('_')])
import inspect
try:
    print(inspect.signature(s.open))
except Exception as e:
    print('sig err', e)
PY
