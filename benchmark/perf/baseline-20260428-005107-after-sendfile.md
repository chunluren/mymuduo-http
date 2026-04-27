# mymuduo-http perf baseline

- **Tag**: `after-sendfile`
- **Time**: 2026-04-28T00:52:38+08:00
- **Host**: Linux 5.15.153.1-microsoft-standard-WSL2 x86_64 | CPU 12 cores | 7863 MB RAM
- **Server**: `build/http_server` PID 475906
- **Endpoint**: `http://127.0.0.1:8080/api/hello`  (response Content-Length=21 bytes)
- **Server RSS after run**: 233204 kB; threads=5

## Workloads

| # | Tool | Args | What it stresses |
|---|------|------|------------------|
| 1 | wrk  | -t4 -c200 -d30s | HTTP keep-alive throughput |
| 2 | wrk  | -t1 -c1 -d10s   | Single-conn latency (Nagle visible here) |
| 3 | wrk  | -t4 -c100 -d20s -H 'Connection: close' | Short-conn accept+close path |
| 4 | wrk  | -t1 -c1 -d10s   | Single-conn (same as #2) for serial baseline |
| 5 | wrk  | -t4 -c100 -d20s /api/large | 64 KB body — writev header/body split |

---

## 1) wrk keep-alive (4×200, 30s)

```
Running 30s test @ http://127.0.0.1:8080/api/hello
  4 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.81ms    1.22ms  39.90ms   80.70%
    Req/Sec    28.68k     2.68k   92.53k    94.42%
  Latency Distribution
     50%    1.64ms
     75%    2.35ms
     90%    3.11ms
     99%    5.16ms
  3427463 requests in 30.09s, 539.33MB read
Requests/sec: 113893.64
Transfer/sec:     17.92MB
```

## 2) wrk single-conn (1×1, 10s)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   157.44us   41.28us   1.59ms   90.20%
    Req/Sec     6.40k   292.14     8.28k    85.15%
  Latency Distribution
     50%  146.00us
     75%  157.00us
     90%  187.00us
     99%  339.00us
  64289 requests in 10.10s, 10.12MB read
Requests/sec:   6365.30
Transfer/sec:      1.00MB
```

## 3) wrk short-conn (4×100, 20s, Connection: close)

```
Running 20s test @ http://127.0.0.1:8080/api/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     4.82ms    1.87ms  19.38ms   66.67%
    Req/Sec     4.07k   384.94    11.47k    82.88%
  Latency Distribution
     50%    5.13ms
     75%    6.20ms
     90%    6.94ms
     99%    8.56ms
  324191 requests in 20.07s, 49.47MB read
Requests/sec:  16152.87
Transfer/sec:      2.46MB
```

## 4) wrk single-conn pipelined (same single-conn target, Lua harness)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   143.74us   41.75us   1.56ms   90.82%
    Req/Sec     6.88k   489.71     7.99k    63.37%
  Latency Distribution
     50%  135.00us
     75%  147.00us
     90%  179.00us
     99%  310.00us
  69128 requests in 10.10s, 10.88MB read
Requests/sec:   6844.57
Transfer/sec:      1.08MB
```

## 5) wrk 64KB body keep-alive (4×100, 20s, /api/large)

```
Running 20s test @ http://127.0.0.1:8080/api/large
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    11.63ms    5.09ms  45.09ms   72.36%
    Req/Sec     2.17k   227.76     6.65k    80.93%
  Latency Distribution
     50%   11.04ms
     75%   14.29ms
     90%   18.27ms
     99%   26.77ms
  172581 requests in 20.03s, 10.55GB read
Requests/sec:   8617.72
Transfer/sec:    539.68MB
```

## Notes

- Workload 2 is the canonical 'is Nagle hurting us?' probe. Without
  TCP_NODELAY, single-conn back-to-back small requests will show
  visible latency from the 40 ms ACK delay timer.
- All numbers single-host loopback; absolute throughput is bounded by
  loopback bandwidth and shared CPU. Use these only for **before/after**
  comparisons against the same machine.
