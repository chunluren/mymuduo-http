# mymuduo-http perf baseline

- **Tag**: `after-tcpnodelay`
- **Time**: 2026-04-28T00:14:22+08:00
- **Host**: Linux 5.15.153.1-microsoft-standard-WSL2 x86_64 | CPU 12 cores | 7863 MB RAM
- **Server**: `build/http_server` PID 467777
- **Endpoint**: `http://127.0.0.1:8080/api/hello`  (response Content-Length=21 bytes)
- **Server RSS after run**: 207152 kB; threads=5

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
    Latency     1.84ms    1.22ms  55.57ms   81.40%
    Req/Sec    28.05k     2.87k   92.13k    93.41%
  Latency Distribution
     50%    1.70ms
     75%    2.38ms
     90%    3.06ms
     99%    5.05ms
  3344204 requests in 30.10s, 526.23MB read
Requests/sec: 111103.48
Transfer/sec:     17.48MB
```

## 2) wrk single-conn (1×1, 10s)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   138.99us   39.47us   1.08ms   90.35%
    Req/Sec     7.25k   500.59     8.99k    72.28%
  Latency Distribution
     50%  130.00us
     75%  142.00us
     90%  170.00us
     99%  311.00us
  72919 requests in 10.10s, 11.47MB read
Requests/sec:   7219.83
Transfer/sec:      1.14MB
```

## 3) wrk short-conn (4×100, 20s, Connection: close)

```
Running 20s test @ http://127.0.0.1:8080/api/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     4.62ms    1.93ms  29.57ms   65.63%
    Req/Sec     4.14k   384.36    10.63k    82.44%
  Latency Distribution
     50%    4.86ms
     75%    6.04ms
     90%    6.85ms
     99%    8.86ms
  330620 requests in 20.10s, 50.45MB read
Requests/sec:  16448.47
Transfer/sec:      2.51MB
```

## 4) wrk single-conn pipelined (same single-conn target, Lua harness)

```
Running 10s test @ http://127.0.0.1:8080/api/hello
  1 threads and 1 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   147.58us   44.30us   1.62ms   87.34%
    Req/Sec     6.70k   517.59     8.26k    73.27%
  Latency Distribution
     50%  137.00us
     75%  154.00us
     90%  189.00us
     99%  323.00us
  67330 requests in 10.10s, 10.59MB read
Requests/sec:   6666.31
Transfer/sec:      1.05MB
```

## Notes

- Workload 2 is the canonical 'is Nagle hurting us?' probe. Without
  TCP_NODELAY, single-conn back-to-back small requests will show
  visible latency from the 40 ms ACK delay timer.
- All numbers single-host loopback; absolute throughput is bounded by
  loopback bandwidth and shared CPU. Use these only for **before/after**
  comparisons against the same machine.
