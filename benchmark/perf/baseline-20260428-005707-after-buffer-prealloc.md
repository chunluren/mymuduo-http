# mymuduo-http perf baseline

- **Tag**: `after-buffer-prealloc`
- **Time**: 2026-04-28T00:58:38+08:00
- **Host**: Linux 5.15.153.1-microsoft-standard-WSL2 x86_64 | CPU 12 cores | 7863 MB RAM
- **Server**: `build/http_server` PID 476951
- **Endpoint**: `http://127.0.0.1:8080/api/hello`  (response Content-Length=21 bytes)
- **Server RSS after run**: 241480 kB; threads=5

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
    Latency     1.73ms    0.91ms  20.61ms   69.61%
    Req/Sec    29.29k     1.32k   34.22k    69.15%
  Latency Distribution
     50%    1.62ms
     75%    2.25ms
     90%    2.93ms
     99%    4.35ms
  3485617 requests in 30.01s, 548.48MB read
Requests/sec: 116139.86
Transfer/sec:     18.28MB
```

## 2) wrk single-conn (1×1, 10s)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   138.86us   38.35us   1.70ms   92.12%
    Req/Sec     7.26k   315.56     8.40k    74.26%
  Latency Distribution
     50%  129.00us
     75%  139.00us
     90%  165.00us
     99%  308.00us
  73000 requests in 10.10s, 11.49MB read
Requests/sec:   7228.07
Transfer/sec:      1.14MB
```

## 3) wrk short-conn (4×100, 20s, Connection: close)

```
Running 20s test @ http://127.0.0.1:8080/api/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     5.17ms    1.53ms  12.77ms   69.39%
    Req/Sec     4.15k   438.55    11.03k    78.78%
  Latency Distribution
     50%    5.48ms
     75%    6.29ms
     90%    6.87ms
     99%    7.89ms
  331130 requests in 20.10s, 50.53MB read
Requests/sec:  16474.63
Transfer/sec:      2.51MB
```

## 4) wrk single-conn pipelined (same single-conn target, Lua harness)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   148.83us   46.53us   1.46ms   89.88%
    Req/Sec     6.66k   589.87     8.30k    81.19%
  Latency Distribution
     50%  136.00us
     75%  151.00us
     90%  191.00us
     99%  339.00us
  66870 requests in 10.10s, 10.52MB read
Requests/sec:   6621.22
Transfer/sec:      1.04MB
```

## 5) wrk 64KB body keep-alive (4×100, 20s, /api/large)

```
Running 20s test @ http://127.0.0.1:8080/api/large
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    11.22ms    6.42ms  61.79ms   75.20%
    Req/Sec     2.32k   277.35     4.93k    66.29%
  Latency Distribution
     50%    9.72ms
     75%   14.17ms
     90%   20.27ms
     99%   31.60ms
  184171 requests in 20.02s, 11.26GB read
Requests/sec:   9200.37
Transfer/sec:    576.14MB
```

## Notes

- Workload 2 is the canonical 'is Nagle hurting us?' probe. Without
  TCP_NODELAY, single-conn back-to-back small requests will show
  visible latency from the 40 ms ACK delay timer.
- All numbers single-host loopback; absolute throughput is bounded by
  loopback bandwidth and shared CPU. Use these only for **before/after**
  comparisons against the same machine.
