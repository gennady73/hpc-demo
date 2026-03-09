#ifndef _GNU_SOURCE
#define _GNU_SOURCE // Required for RTLD_NEXT in dlfcn.h
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <thread>
#include <vector>
#include <chrono>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

// Structure to hold our environment diagnosis
struct RuntimeCapabilities {
    int cgroup_quota_cpus = -1;
    int sched_affinity_cpus = -1;
    int windows_affinity_cpus = -1;
    int hw_concurrency = -1;
    int optimal_thread_count = 1;
    std::string inferred_policy = "Unknown / Not Applicable";
    std::string environment = "Unknown";
    std::string cgroup_version = "None Detected";
};

RuntimeCapabilities detect_environment() {
    RuntimeCapabilities caps;
    caps.hw_concurrency = std::thread::hardware_concurrency();

#if defined(__linux__)
    caps.environment = "Linux (Potential Container/OpenShift)";
    
    // Tier 1a: Parse cgroup v2 cpu.max for CFS Quotas
    std::ifstream cpu_max_file("/sys/fs/cgroup/cpu.max");
    if (cpu_max_file.is_open()) {
        caps.cgroup_version = "v2";
        std::string quota_str, period_str;
        cpu_max_file >> quota_str >> period_str;
        
        if (quota_str!= "max" &&!quota_str.empty()) {
            try {
                long long quota = std::stoll(quota_str);
                long long period = std::stoll(period_str);
                if (period > 0) {
                    caps.cgroup_quota_cpus = static_cast<int>(std::ceil(static_cast<double>(quota) / period));
                }
            } catch (...) {
                // Silently handle parse errors
            }
        }
    } else {
        // Tier 1b: Fallback to parse cgroup v1 for CFS Quotas
        std::ifstream cfs_quota_file("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
        std::ifstream cfs_period_file("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
        
        if (cfs_quota_file.is_open() && cfs_period_file.is_open()) {
            caps.cgroup_version = "v1";
            std::string quota_str, period_str;
            cfs_quota_file >> quota_str;
            cfs_period_file >> period_str;
            
            // In cgroup v1, a quota of "-1" means unconstrained limits
            if (!quota_str.empty() && quota_str!= "-1") {
                try {
                    long long quota = std::stoll(quota_str);
                    long long period = std::stoll(period_str);
                    if (period > 0 && quota > 0) {
                        caps.cgroup_quota_cpus = static_cast<int>(std::ceil(static_cast<double>(quota) / period));
                    }
                } catch (...) {}
            }
        }
    }

    // Tier 2: Check OS-level scheduler affinity
    cpu_set_t cpuset;
    if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        caps.sched_affinity_cpus = CPU_COUNT(&cpuset);
    }

    // Tier 3: Infer OpenShift CPU Manager Policy and set optimal threads
    if (caps.cgroup_quota_cpus > 0) {
        caps.optimal_thread_count = caps.cgroup_quota_cpus;
        
        if (caps.sched_affinity_cpus > 0) {
            if (caps.cgroup_quota_cpus == caps.sched_affinity_cpus) {
                caps.inferred_policy = "OpenShift/K8s 'static' policy (Exclusive Pinned Cores)";
            } else if (caps.sched_affinity_cpus > caps.cgroup_quota_cpus) {
                caps.inferred_policy = "OpenShift/K8s 'none' policy (CFS Time-Slicing / Floating Threads)";
            }
        }
    } else if (caps.sched_affinity_cpus > 0) {
        caps.optimal_thread_count = caps.sched_affinity_cpus;
        
        // OpenShift 4.20+ optimization: CFS quota might be intentionally disabled for pinned workloads
        if (caps.sched_affinity_cpus < caps.hw_concurrency) {
            caps.inferred_policy = "OpenShift 'static' policy (CFS Quota Disabled for Peak Perf)";
        } else {
            caps.inferred_policy = "Unrestricted (No CFS Quota or Pinning Detected)";
        }
    } else {
        caps.optimal_thread_count = caps.hw_concurrency > 0? caps.hw_concurrency : 1;
    }

#elif defined(_WIN32)
    caps.environment = "Windows";
    DWORD_PTR processAffinityMask, systemAffinityMask;
    
    // Process Windows affinity for local development environments
    if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinityMask, &systemAffinityMask)) {
        int count = 0;
        for (DWORD_PTR i = 1; i!= 0; i <<= 1) {
            if (processAffinityMask & i) count++;
        }
        caps.windows_affinity_cpus = count;
    }

    if (caps.windows_affinity_cpus > 0) {
        caps.optimal_thread_count = caps.windows_affinity_cpus;
    } else {
        caps.optimal_thread_count = caps.hw_concurrency > 0? caps.hw_concurrency : 1;
    }
#else
    caps.environment = "Other OS";
    caps.optimal_thread_count = caps.hw_concurrency > 0? caps.hw_concurrency : 1;
#endif

    return caps;
}

// ==============================================================================
// GLOBAL SYSCONF OVERRIDE (C++ Symbol Interposition for Legacy Libraries)
// ==============================================================================
#if defined(__linux__)
extern "C" {
    long sysconf(int name) {
        // Intercept CPU core queries (used by gRPC, std::thread, OpenBLAS, etc.)
        if (name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF) {
            
            // Magic statics: Thread-safe initialization in C++11 and later
            static int cached_cpu_limit = -1;
            static bool initialized = false;

            // Execute the cgroup file I/O exactly once
            if (!initialized) {
                RuntimeCapabilities env = detect_environment();
                cached_cpu_limit = env.optimal_thread_count;
                initialized = true;
            }

            // Trick legacy libraries into seeing only the pod's limit
            if (cached_cpu_limit > 0) {
                return cached_cpu_limit;
            }
        }
        
        // Fallback to the real libc sysconf for all other queries (like page size)
        static long (*real_sysconf)(int) = nullptr;
        if (!real_sysconf) {
            // RTLD_NEXT finds the next occurrence of a function in the search order (i.e., glibc)
            real_sysconf = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
        }
        return real_sysconf(name);
    }
}
#endif

int main() {
    RuntimeCapabilities env = detect_environment();

    std::cout << "==================================================\n";
    std::cout << "   HPC Application Runtime Environment Report     \n";
    std::cout << "==================================================\n";
    std::cout << "Detected OS Environment  : " << env.environment << "\n";
    std::cout << "Hardware Concurrency     : " << env.hw_concurrency << " cores (Physical/Logical Host)\n";
    
    if (env.environment == "Linux (Potential Container/OpenShift)") {
        std::cout << "Detected Cgroup Version  : " << env.cgroup_version << "\n";
        std::cout << "Cgroup CFS Quota Limit   : " << (env.cgroup_quota_cpus > 0? std::to_string(env.cgroup_quota_cpus) : "Unlimited/Not Found") << " CPUs\n";
        std::cout << "Scheduler Affinity Mask  : " << (env.sched_affinity_cpus > 0? std::to_string(env.sched_affinity_cpus) : "Error") << " CPUs\n";
        std::cout << "Inferred CPU Manager     : " << env.inferred_policy << "\n";
    } else if (env.environment == "Windows") {
        std::cout << "Windows Process Affinity : " << (env.windows_affinity_cpus > 0? std::to_string(env.windows_affinity_cpus) : "Error") << " CPUs\n";
    }

    std::cout << "--------------------------------------------------\n";
    std::cout << ">>> ACTION: Initializing thread pool with " << env.optimal_thread_count << " workers.\n";
    
#if defined(__linux__)
    std::cout << ">>> PROOF: sysconf(_SC_NPROCESSORS_ONLN) intercepted. Returning: " << sysconf(_SC_NPROCESSORS_ONLN) << " cores.\n";
#endif
    std::cout << "==================================================\n";

    // Simulate continuous thread pool workload and keep container alive for debugging
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    
    return 0;
}

