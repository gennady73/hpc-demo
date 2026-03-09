#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
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
                } catch (...) {
                     // Silently handle parse errors
                }
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
        // Fallback for bare-metal Linux or unrestricted containers
        caps.optimal_thread_count = caps.sched_affinity_cpus;
        caps.inferred_policy = "Unrestricted (No CFS Quota Detected)";
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
    std::cout << "==================================================\n";

    // Simulate thread pool initialization here...
    
    return 0;
}

