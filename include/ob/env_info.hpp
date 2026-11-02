// Captures the benchmark environment (hardware, OS, compiler, flags,
// virtualisation, CPU governor) and provides best-effort core pinning.
#pragma once

#include <string>

namespace ob {

struct EnvInfo {
    std::string cpu;
    unsigned logical_cores = 0;
    std::string os;
    std::string compiler;
    std::string flags;
    std::string platform;  // bare metal / VM / WSL / container (best effort)
    std::string governor;  // Linux cpufreq governor, if readable
    std::string isolated;  // Linux isolated cpus, if any
    std::string timer;     // timestamp source and resolution

    static EnvInfo capture();
    std::string to_markdown() const;
};

// Pins the calling thread to `cpu`. Returns false where unsupported (macOS has
// no hard affinity API; we fall back to the highest QoS class there).
bool pin_thread_to_cpu(int cpu);

}  // namespace ob
