# mymuduo-http perf baseline

- **Tag**: `before-tcpnodelay`
- **Time**: 2026-04-28T00:11:21+08:00
- **Host**: Linux 5.15.153.1-microsoft-standard-WSL2 x86_64 | CPU 12 cores | 7863 MB RAM
- **Server**: `build/http_server` PID 467081
- **Endpoint**: `http://127.0.0.1:8080/api/hello`  (response Content-Length=21 bytes)
- **Server RSS after run**: 208316 kB; threads=5

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
    Latency     1.82ms    1.13ms  28.50ms   76.84%
    Req/Sec    28.37k     1.40k   33.16k    70.15%
  Latency Distribution
     50%    1.67ms
     75%    2.36ms
     90%    3.07ms
     99%    5.10ms
  3376872 requests in 30.07s, 531.37MB read
Requests/sec: 112297.88
Transfer/sec:     17.67MB
```

## 2) wrk single-conn (1×1, 10s)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   149.62us   44.64us   1.32ms   91.07%
    Req/Sec     6.76k   345.71     7.78k    70.30%
  Latency Distribution
     50%  138.00us
     75%  151.00us
     90%  187.00us
     99%  345.00us
  67879 requests in 10.10s, 10.68MB read
Requests/sec:   6721.00
Transfer/sec:      1.06MB
```

## 3) wrk short-conn (4×100, 20s, Connection: close)

```
Running 20s test @ http://127.0.0.1:8080/api/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     4.53ms    1.99ms  17.23ms   64.06%
    Req/Sec     4.15k   389.27     9.39k    77.78%
  Latency Distribution
     50%    4.75ms
     75%    6.05ms
     90%    6.89ms
     99%    9.06ms
  331105 requests in 20.10s, 50.52MB read
Requests/sec:  16473.45
Transfer/sec:      2.51MB
```

## 4) wrk single-conn pipelined (same single-conn target, Lua harness)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   144.53us   44.03us   1.38ms   89.87%
    Req/Sec     6.86k   465.92     8.93k    72.00%
  Latency Distribution
     50%  134.00us
     75%  146.00us
     90%  180.00us
     99%  325.00us
  68250 requests in 10.00s, 10.74MB read
Requests/sec:   6824.89
Transfer/sec:      1.07MB
```

## Notes

- Workload 2 is the canonical 'is Nagle hurting us?' probe. Without
  TCP_NODELAY, single-conn back-to-back small requests will show
  visible latency from the 40 ms ACK delay timer.
- All numbers single-host loopback; absolute throughput is bounded by
  loopback bandwidth and shared CPU. Use these only for **before/after**
  comparisons against the same machine.
