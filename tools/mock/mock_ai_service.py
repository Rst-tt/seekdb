#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mock AI service for seekdb ai_function tests (0.0.0.0:8000).

Routes are matched by URL suffix because the tests use arbitrary prefixes
such as /compatible-mode/v1/chat/completions.
"""
import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_DIM = 768


def vec(n, seed):
    return [round(((i * 37 + seed * 13) % 1000) / 1000.0, 6) for i in range(n)]


def rerank_score(query, doc):
    q = (query or "").lower()
    d = (doc or "").lower()
    if q and q == d:
        return 1.0
    if q and q in d:
        return 0.5
    if q and d and d in q:
        return 0.25
    return 0.0


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, payload, raw=False):
        body = payload if raw else json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._send(200, {"status": "ok"})

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length else b""
            req = {}
            if raw:
                try:
                    req = json.loads(raw.decode("utf-8"))
                except Exception:
                    req = {}
            path = (self.path or "").split("?")[0].lower()
            if path.endswith("/chat/completions/empty"):
                self._send(200, {"id": "cmpl-empty", "object": "chat.completion",
                                 "model": req.get("model", "mock"),
                                 "choices": [{"index": 0, "message": {"role": "assistant", "content": ""},
                                              "finish_reason": "stop"}]})
            elif path.endswith("/chat/completions/badjson"):
                self._send(200, b"this is not a json body {", raw=True)
            elif path.endswith("/chat/completions"):
                self._send(200, {"id": "cmpl-1", "object": "chat.completion",
                                 "model": req.get("model", "mock"),
                                 "choices": [{"index": 0,
                                              "message": {"role": "assistant",
                                                          "content": "mock completion response"},
                                              "finish_reason": "stop"}],
                                 "usage": {"prompt_tokens": 3, "completion_tokens": 4, "total_tokens": 7}})
            elif path.endswith("/embeddings/wrongdim"):
                inputs = req.get("input") or []
                if not isinstance(inputs, list):
                    inputs = [inputs]
                data = [{"object": "embedding", "index": i, "embedding": vec(16, i + 1)}
                        for i in range(len(inputs))]
                self._send(200, {"object": "list", "model": req.get("model", "mock"), "data": data})
            elif path.endswith("/embeddings/dupidx"):
                data = [{"object": "embedding", "index": 0, "embedding": vec(4, 1)},
                        {"object": "embedding", "index": 0, "embedding": vec(4, 2)}]
                self._send(200, {"object": "list", "model": req.get("model", "mock"), "data": data})
            elif path.endswith("/embeddings"):
                inputs = req.get("input")
                if inputs is None:
                    inputs = []
                if not isinstance(inputs, list):
                    inputs = [inputs]
                inputs = [str(x) if x is not None else "" for x in inputs]
                dim = DEFAULT_DIM
                try:
                    if isinstance(req.get("dimensions"), int) and req["dimensions"] > 0:
                        dim = req["dimensions"]
                except Exception:
                    pass
                data = []
                for i, text in enumerate(inputs):
                    data.append({"object": "embedding", "index": i,
                                 "embedding": vec(dim, i + 1)})
                # answer out of order on purpose, index drives the mapping
                data.reverse()
                self._send(200, {"object": "list", "model": req.get("model", "mock"),
                                 "data": data, "usage": {"prompt_tokens": len(inputs), "total_tokens": len(inputs)}})
            elif path.endswith("/rerank"):
                docs = req.get("documents") or []
                if not isinstance(docs, list):
                    docs = [docs]
                query = req.get("query") or ""
                results = []
                for i, d in enumerate(docs):
                    text = d if isinstance(d, str) else json.dumps(d, ensure_ascii=False, sort_keys=True)
                    results.append({"index": i, "relevance_score": rerank_score(query, text),
                                    "document": d})
                self._send(200, {"id": "rerank-1", "results": results,
                                 "meta": {"billed_units": {"search_units": 1}}})
            elif path.endswith("/api/generate"):
                self._send(200, {"model": req.get("model", "mock"), "created_at": "2025-01-01T00:00:00Z",
                                 "response": "mock ollama completion response", "done": True})
            elif path.endswith("/api/embed"):
                inputs = req.get("input")
                if inputs is None:
                    inputs = []
                if not isinstance(inputs, list):
                    inputs = [inputs]
                embeddings = [vec(DEFAULT_DIM, i + 1) for i in range(len(inputs))]
                self._send(200, {"model": req.get("model", "mock"), "embeddings": embeddings,
                                 "total_duration": 1000, "load_duration": 10,
                                 "prompt_eval_count": len(inputs)})
            else:
                self._send(404, {"error": "unknown path %s" % path})
        except Exception as exc:  # never crash the service
            try:
                self._send(500, {"error": str(exc)})
            except Exception:
                pass


def main():
    server = ThreadingHTTPServer(("0.0.0.0", 8000), Handler)
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    main()
