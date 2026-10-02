"""Live regression for PLE waits across normal/profiled graph transitions.

Run against an idle production-configured engine. The first four requests
match the desktop Benchmark button: unprofiled warmup, then three measured
requests. The last request returns to the normal graph cache.
"""

import argparse
import hashlib
import json
import math
import statistics
import urllib.error
import urllib.request
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--max-tokens", type=int, default=8192)
    parser.add_argument("--temperature", type=float, default=0.35)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    prompt = (
        "Explain speculative decoding, memory bandwidth, recurrent state, and "
        "long-context inference in technically precise, non-repetitive prose. "
    ) * 48
    body = {
        "model": "local", "messages": [{"role": "user", "content": prompt}],
        "max_tokens": args.max_tokens, "temperature": args.temperature,
        "top_p": 0.90, "top_k": 40, "min_p": 0.05,
        "repeat_penalty": 1.05, "repeat_last_n": 512, "seed": 12345,
        "stream": False,
        "chat_template_kwargs": {
            "enable_thinking": False, "preserve_thinking": False,
            "reasoning_effort": "medium",
        },
    }
    results = []
    for stage, profile in [("warmup", False), ("measured-1", True),
                           ("measured-2", True), ("measured-3", True),
                           ("normal-again", False)]:
        body["flashnext_profile"] = profile
        request = urllib.request.Request(
            args.url.rstrip("/") + "/v1/chat/completions",
            data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=1200) as response:
                result = json.load(response)
        except urllib.error.HTTPError as error:
            raise RuntimeError(f"{stage}: {error.read().decode()}") from error
        metrics = result["usage"]["flashnext_velocity"]
        settings = metrics["effective_request_settings"]
        rate = metrics["completion_tokens_per_second"]
        assert math.isfinite(rate) and rate > 0, f"{stage}: invalid decode rate"
        assert settings["seed"] == 12345, f"{stage}: seed changed"
        assert settings["max_tokens"] == args.max_tokens, f"{stage}: token limit changed"
        content = result["choices"][0]["message"]["content"]
        record = {
            "stage": stage, "profile": profile, "decode_tps": rate,
            "tokens": result["usage"]["completion_tokens"],
            "lookup_cycles": metrics["lookup_cycles"],
            "sha256": hashlib.sha256(content.encode()).hexdigest(),
        }
        results.append(record)
        print(json.dumps(record), flush=True)
    if args.temperature == 0:
        assert len({r["sha256"] for r in results}) == 1, "greedy output changed with profiling"
    rates = [r["decode_tps"] for r in results if r["profile"]]
    report = {"runs": results, "median": statistics.median(rates),
              "min": min(rates), "max": max(rates)}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False))
    print("PASS", flush=True)


if __name__ == "__main__":
    main()
