#include <iostream>
#include <cassert>
#include "src/util/Metrics.h"

using namespace std;

void testCounter() {
    cout << "=== Testing Counter ===" << endl;
    auto& m = Metrics::instance();
    m.reset();

    m.increment("requests");
    m.increment("requests");
    m.increment("requests", 3);
    assert(m.getCounter("requests") == 5);

    cout << "Counter test passed!" << endl;
}

void testGauge() {
    cout << "=== Testing Gauge ===" << endl;
    auto& m = Metrics::instance();
    m.reset();

    m.gauge("connections", 10);
    assert(m.getGauge("connections") == 10);
    m.gauge("connections", 5);
    assert(m.getGauge("connections") == 5);

    cout << "Gauge test passed!" << endl;
}

void testObserve() {
    cout << "=== Testing Observe ===" << endl;
    auto& m = Metrics::instance();
    m.reset();

    m.observe("latency_ms", 10.5);
    m.observe("latency_ms", 20.3);

    string output = m.toPrometheus();
    assert(output.find("latency_ms_count 2") != string::npos);
    assert(output.find("latency_ms_sum 30.8") != string::npos);

    cout << "Observe test passed!" << endl;
}

void testPrometheusFormat() {
    cout << "=== Testing Prometheus Format ===" << endl;
    auto& m = Metrics::instance();
    m.reset();

    m.increment("http_total", 100);
    m.gauge("active", 42);

    string output = m.toPrometheus();
    assert(output.find("# TYPE http_total counter") != string::npos);
    assert(output.find("http_total 100") != string::npos);
    assert(output.find("# TYPE active gauge") != string::npos);
    assert(output.find("active 42") != string::npos);

    cout << "Prometheus format test passed!" << endl;
}

int main() {
    cout << "Starting Metrics Tests..." << endl << endl;
    testCounter();
    testGauge();
    testObserve();
    testPrometheusFormat();
    cout << endl << "All Metrics tests passed!" << endl;
    return 0;
}
