# mymuduo-http perf baseline

- **Tag**: `after-writev-final`
- **Time**: 2026-04-28T00:34:43+08:00
- **Host**: Linux 5.15.153.1-microsoft-standard-WSL2 x86_64 | CPU 12 cores | 7863 MB RAM
- **Server**: `build/http_server` PID 471951
- **Endpoint**: `http://127.0.0.1:8080/api/hello`  (response Content-Length=21 bytes)
- **Server RSS after run**: 204544 kB; threads=5

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
    Latency     1.80ms    1.02ms  19.44ms   71.14%
    Req/Sec    28.41k     2.06k   76.56k    84.71%
  Latency Distribution
     50%    1.67ms
     75%    2.37ms
     90%    3.08ms
     99%    4.76ms
  3383971 requests in 30.07s, 532.49MB read
Requests/sec: 112549.27
Transfer/sec:     17.71MB
```

## 2) wrk single-conn (1×1, 10s)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   148.79us   38.47us   0.91ms   89.19%
    Req/Sec     6.76k   388.10     7.85k    69.31%
  Latency Distribution
     50%  140.00us
     75%  152.00us
     90%  185.00us
     99%  311.00us
  67947 requests in 10.10s, 10.69MB read
Requests/sec:   6727.73
Transfer/sec:      1.06MB
```

## 3) wrk short-conn (4×100, 20s, Connection: close)

```
Running 20s test @ http://127.0.0.1:8080/api/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     4.79ms    1.87ms  30.16ms   66.95%
    Req/Sec     4.09k   379.93    11.14k    78.75%
  Latency Distribution
     50%    5.07ms
     75%    6.15ms
     90%    6.91ms
     99%    8.66ms
  325355 requests in 20.06s, 49.65MB read
Requests/sec:  16215.27
Transfer/sec:      2.47MB
```

## 4) wrk single-conn pipelined (same single-conn target, Lua harness)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   145.67us   40.08us   1.44ms   90.17%
    Req/Sec     6.82k   415.21     8.42k    79.21%
  Latency Distribution
     50%  138.00us
     75%  146.00us
     90%  174.00us
     99%  317.00us
  68498 requests in 10.10s, 10.78MB read
Requests/sec:   6782.28
Transfer/sec:      1.07MB
```

## 5) wrk 64KB body keep-alive (4×100, 20s, /api/large)

```
Running 20s test @ http://127.0.0.1:8080/api/large
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    12.03ms    7.12ms  72.79ms   72.13%
    Req/Sec     2.15k   367.58     4.26k    70.88%
  Latency Distribution
     50%   10.81ms
     75%   15.42ms
     90%   21.98ms
     99%   35.10ms
  171604 requests in 20.07s, 10.49GB read
Requests/sec:   8548.79
Transfer/sec:    535.34MB
```

## Notes

- Workload 2 is the canonical 'is Nagle hurting us?' probe. Without
  TCP_NODELAY, single-conn back-to-back small requests will show
  visible latency from the 40 ms ACK delay timer.
- All numbers single-host loopback; absolute throughput is bounded by
  loopback bandwidth and shared CPU. Use these only for **before/after**
  comparisons against the same machine.
