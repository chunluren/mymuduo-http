# mymuduo-http perf baseline

- **Tag**: `after-keepalive-tune`
- **Time**: 2026-04-28T00:20:57+08:00
- **Host**: Linux 5.15.153.1-microsoft-standard-WSL2 x86_64 | CPU 12 cores | 7863 MB RAM
- **Server**: `build/http_server` PID 469453
- **Endpoint**: `http://127.0.0.1:8080/api/hello`  (response Content-Length=21 bytes)
- **Server RSS after run**: 205344 kB; threads=5

## Workloads

| # | Tool | Args | What it stresses |
|---|------|------|------------------|
| 1 | wrk  | -t4 -c200 -d30s | HTTP keep-alive throughput |
| 2 | wrk  | -t1 -c1 -d10s   | Single-conn latency (Nagle visible here) |
| 3 | wrk  | -t4 -c100 -d20s -H 'Connection: close' | Short-conn accept+close path |
| 4 | wrk  | -t1 -c1 -d10s   | Single-conn (same as #2) for serial baseline |

---

## 1) wrk keep-alive (4×200, 30s)

```
Running 30s test @ http://127.0.0.1:8080/api/hello
  4 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.83ms    1.07ms  26.81ms   73.89%
    Req/Sec    28.06k     1.99k   72.39k    82.37%
  Latency Distribution
     50%    1.68ms
     75%    2.38ms
     90%    3.10ms
     99%    4.97ms
  3341994 requests in 30.06s, 525.88MB read
Requests/sec: 111193.43
Transfer/sec:     17.50MB
```

## 2) wrk single-conn (1×1, 10s)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   151.50us   66.36us   2.70ms   92.62%
    Req/Sec     6.75k   400.41     7.89k    75.25%
  Latency Distribution
     50%  134.00us
     75%  152.00us
     90%  197.00us
     99%  389.00us
  67848 requests in 10.10s, 10.68MB read
Requests/sec:   6718.14
Transfer/sec:      1.06MB
```

## 3) wrk short-conn (4×100, 20s, Connection: close)

```
Running 20s test @ http://127.0.0.1:8080/api/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     4.65ms    1.97ms  30.27ms   65.83%
    Req/Sec     4.11k   380.02    10.18k    82.52%
  Latency Distribution
     50%    4.89ms
     75%    6.11ms
     90%    6.90ms
     99%    9.04ms
  327634 requests in 20.10s, 49.99MB read
Requests/sec:  16300.85
Transfer/sec:      2.49MB
```

## 4) wrk single-conn pipelined (same single-conn target, Lua harness)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   147.94us   48.35us   1.38ms   91.29%
    Req/Sec     6.73k   385.35     7.72k    65.35%
  Latency Distribution
     50%  135.00us
     75%  152.00us
     90%  187.00us
     99%  358.00us
  67599 requests in 10.10s, 10.64MB read
Requests/sec:   6693.06
Transfer/sec:      1.05MB
```

## Notes

- Workload 2 is the canonical 'is Nagle hurting us?' probe. Without
  TCP_NODELAY, single-conn back-to-back small requests will show
  visible latency from the 40 ms ACK delay timer.
- All numbers single-host loopback; absolute throughput is bounded by
  loopback bandwidth and shared CPU. Use these only for **before/after**
  comparisons against the same machine.
