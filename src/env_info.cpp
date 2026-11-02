#include "ob/env_info.hpp"

#include <fstream>
#include <sstream>
#include <thread>

#include "ob/timer.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/utsname.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#endif

#ifndef OB_BUILD_FLAGS
#define OB_BUILD_FLAGS "unknown"
#endif

namespace ob {
namespace {

[[maybe_unused]] std::string read_first_line(const char* path) {
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    return s;
}

[[maybe_unused]] std::string file_contains(const char* path, const char* needle) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return line;
    return {};
}

#if defined(__APPLE__)
std::string sysctl_str(const char* name) {
    char buf[256];
    size_t len = sizeof(buf);
    if (sysctlbyname(name, buf, &len, nullptr, 0) == 0) return std::string(buf, len ? len - 1 : 0);
    return {};
}
int sysctl_int(const char* name) {
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) == 0) return v;
    return -1;
}
#endif

}  // namespace

EnvInfo EnvInfo::capture() {
    EnvInfo e;
    e.logical_cores = std::thread::hardware_concurrency();
#if defined(__clang__)
    e.compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
    e.compiler = "gcc " __VERSION__;
#else
    e.compiler = "unknown";
#endif
    e.flags = OB_BUILD_FLAGS;
#if defined(__linux__) || defined(__APPLE__)
    utsname u{};
    if (uname(&u) == 0) e.os = std::string(u.sysname) + " " + u.release + " " + u.machine;
#endif
#if defined(__linux__)
    auto model = file_contains("/proc/cpuinfo", "model name");
    if (auto p = model.find(':'); p != std::string::npos) e.cpu = model.substr(p + 2);
    const bool hyper = !file_contains("/proc/cpuinfo", " hypervisor").empty();
    const std::string ver = read_first_line("/proc/version");
    const bool wsl = ver.find("microsoft") != std::string::npos || ver.find("Microsoft") != std::string::npos;
    const bool container = std::ifstream("/.dockerenv").good();
    e.platform = wsl ? "WSL" : hyper ? "virtual machine (hypervisor flag set)" : "bare metal (no hypervisor flag)";
    if (container) e.platform += ", container";
    e.governor = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    e.isolated = read_first_line("/sys/devices/system/cpu/isolated");
#elif defined(__APPLE__)
    e.cpu = sysctl_str("machdep.cpu.brand_string");
    e.platform = sysctl_int("kern.hv_vmm_present") == 1 ? "virtual machine" : "bare metal";
    e.governor = "n/a (macOS manages DVFS)";
    e.isolated = "n/a (macOS has no core isolation)";
#endif
    std::ostringstream t;
    t << Timer::source() << ", " << Timer::ns_per_tick() << " ns/tick";
    e.timer = t.str();
    return e;
}

std::string EnvInfo::to_markdown() const {
    std::ostringstream out;
    out << "| Item | Value |\n|---|---|\n"
       << "| CPU | " << cpu << " (" << logical_cores << " logical cores) |\n"
       << "| OS | " << os << " |\n"
       << "| Platform | " << platform << " |\n"
       << "| Compiler | " << compiler << " |\n"
       << "| Flags | `" << flags << "` |\n"
       << "| Governor | " << (governor.empty() ? "unknown" : governor) << " |\n"
       << "| Isolated CPUs | " << (isolated.empty() ? "none" : isolated) << " |\n"
       << "| Timer | " << timer << " |\n";
    return out.str();
}

bool pin_thread_to_cpu(int cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(__APPLE__)
    (void)cpu;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return false;
#else
    (void)cpu;
    return false;
#endif
}

}  // namespace ob
