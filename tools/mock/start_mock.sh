#!/bin/bash
# restart the mock ai service inside WSL
cp /mnt/e/seekdb-ai/mock/mock_ai_service.py /home/rst/seekdb-ai/mock_ai_service.py || exit 1
pkill -f mock_ai_service.py >/dev/null 2>&1
sleep 0.3
cd /home/rst/seekdb-ai
nohup setsid python3 mock_ai_service.py > /home/rst/seekdb-ai/mock.log 2>&1 &
sleep 1
curl -s -m 5 -X POST http://127.0.0.1:8000/openai/v1/chat/completions -d '{"model":"m","messages":[{"role":"user","content":"hi"}]}' | head -c 200
echo
curl -s -m 5 -X POST http://127.0.0.1:8000/compatible-mode/v1/embeddings -d '{"model":"m","input":["a","b"]}' | head -c 120
echo
curl -s -m 5 -X POST http://127.0.0.1:8000/v1/rerank -d '{"model":"m","query":"apple","documents":["apple","banana"]}' | head -c 200
echo
curl -s -m 5 -X POST http://127.0.0.1:8000/api/generate -d '{"model":"m","prompt":"hi"}' | head -c 120
echo
curl -s -m 5 -X POST http://127.0.0.1:8000/api/embed -d '{"model":"m","input":["hi"]}' | head -c 100
echo
