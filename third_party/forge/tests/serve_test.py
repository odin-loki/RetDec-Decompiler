#!/usr/bin/env python3
"""forge-serve integration test: concurrent completions match the CLI, streaming, chat, grammar, stop strings."""
import json, subprocess, sys, threading, time, urllib.request
serve, cli, fpk = sys.argv[1], sys.argv[2], sys.argv[3]
p = subprocess.Popen([serve, fpk, "--port", "0", "--max-active", "3"], stdout=subprocess.PIPE, text=True)
try:
    line = p.stdout.readline()
    port = int(line.rsplit(":", 1)[1].split()[0])
    base = f"http://127.0.0.1:{port}"
    def post(path, body, raw=False):
        req = urllib.request.Request(base + path, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
        r = urllib.request.urlopen(req, timeout=60)
        return r.read().decode() if raw else json.loads(r.read())
    assert json.loads(urllib.request.urlopen(base + "/health").read())["status"] == "ok"
    prompts = ["the", "in the", "a cat", "on the mat", "hello world", "re"]
    want = {}
    for q in prompts:
        want[q] = [int(x) for x in subprocess.run([cli, "run", fpk, "--prompt", q, "-n", "10", "--ids"], capture_output=True, text=True, check=True).stdout.split()]
    got = {}
    def worker(q): got[q] = post("/v1/completions", {"prompt": q, "max_tokens": 10})["choices"][0]["tokens"]
    ts = [threading.Thread(target=worker, args=(q,)) for q in prompts]
    [t.start() for t in ts]; [t.join() for t in ts]
    for q in prompts:
        w = [t for t in want[q] if t != 291]
        assert got[q] == w, (q, got[q], w)
    # streaming
    raw = post("/v1/completions", {"prompt": "the", "max_tokens": 10, "stream": True}, raw=True)
    events = [json.loads(l[6:]) for l in raw.split("\n") if l.startswith("data: ") and l != "data: [DONE]"]
    toks = [e["choices"][0]["token"] for e in events if "token" in e["choices"][0]]
    assert toks == [t for t in want["the"] if t != 291], toks
    assert raw.rstrip().endswith("data: [DONE]")
    # chat + grammar + stop
    r = post("/v1/chat/completions", {"messages": [{"role": "user", "content": "hi"}], "max_tokens": 20, "temperature": 0.7, "seed": 2,
                                     "grammar": 'root ::= "ok" | "fine" | "sure"'})
    assert r["choices"][0]["message"]["content"] in ("ok", "fine", "sure"), r
    r = post("/v1/chat/completions", {"messages": [{"role": "user", "content": "hi"}], "max_tokens": 40, "temperature": 0.9, "seed": 3,
                                     "response_format": {"type": "json_schema", "json_schema": {"schema": {"enum": ["yes", "no", 7]}}}})
    assert json.loads(r["choices"][0]["message"]["content"]) in ("yes", "no", 7), r
    full = post("/v1/completions", {"prompt": "the", "max_tokens": 10})["choices"][0]["text"]
    pen = post("/v1/completions", {"prompt": "the", "max_tokens": 10, "presence_penalty": 50.0, "frequency_penalty": 5.0})["choices"][0]["tokens"]
    assert len(pen) == 10 and len(set(pen)) == len(pen), pen   # huge penalties forbid any repeat in greedy decoding
    if len(full) > 3:
        cut = full[2:4]
        s = post("/v1/completions", {"prompt": "the", "max_tokens": 10, "stop": [cut]})["choices"][0]
        assert s["text"] == full[:full.find(cut)] and s["finish_reason"] == "stop", (s, full, cut)
    try:
        post("/v1/completions", {"prompt": "x", "grammar": 'root ::= "unterminated'}); raise SystemExit("expected 400")
    except urllib.error.HTTPError as e:
        assert e.code == 400
    m = urllib.request.urlopen(base + "/metrics").read().decode()
    print("serve ok;", m.replace("\n", " "))
finally:
    p.kill()

# Memory budget: a tiny --mem-limit must serialise requests (admission control), not fail or change output.
p = subprocess.Popen([serve, fpk, "--port", "0", "--max-active", "3", "--mem-limit", "4K", "--max-queue", "64"], stdout=subprocess.PIPE, text=True)
try:
    line = p.stdout.readline()
    assert "memory budget 4.0 KiB" in line, line
    port = int(line.split("http://127.0.0.1:")[1].split()[0])
    base = f"http://127.0.0.1:{port}"
    got = {}
    ts = [threading.Thread(target=worker, args=(q,)) for q in prompts]
    [t.start() for t in ts]; [t.join() for t in ts]
    for q in prompts:
        assert got[q] == [t for t in want[q] if t != 291], q
    m = urllib.request.urlopen(base + "/metrics").read().decode()
    assert 'forge_mem_limit_bytes{pool="host"} 4096' in m, m
    assert "forge_admission_deferred_total" in m and "forge_queue_waiting 0" in m, m
    print("serve mem-limit ok")
finally:
    p.kill()
