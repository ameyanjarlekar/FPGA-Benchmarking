// host_benchmark.cpp
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <xrt/xrt_uuid.h>

#include "vnx/cmac.hpp"
#include "vnx/networklayer.hpp"

namespace fs = std::filesystem;

// ------------------------------
// Helpers
// ------------------------------

enum xclbin_type { IF0, IF1, IF3 };

struct xclbin_info {
  std::string path;
  xclbin_type type;
};

xclbin_info parse_xclbin(const std::string& path) {
  const std::string filename = fs::path(path).filename().string();
  if (filename.find("if0.xclbin") != std::string::npos) return {path, IF0};
  if (filename.find("if1.xclbin") != std::string::npos) return {path, IF1};
  if (filename.find("if3.xclbin") != std::string::npos) return {path, IF3};
  throw std::runtime_error("Cannot infer interface type from xclbin name: " + filename);
}

// ------------------------------
// Benchmark traffic-generator registers
// CONFIRM THESE OFFSETS against your built kernel metadata.
// The field names are based on the notebook behavior.
// ------------------------------
namespace tg_reg {
constexpr uint32_t DEBUG_RESET      = 0x00;
constexpr uint32_t MODE             = 0x10;
constexpr uint32_t SOCKET_INDEX     = 0x18;
constexpr uint32_t NUM_PACKETS_LO   = 0x20;
constexpr uint32_t NUM_PACKETS_HI   = 0x24;
constexpr uint32_t NUM_BEATS        = 0x28;
constexpr uint32_t GAP              = 0x30;
constexpr uint32_t START            = 0x38;

constexpr uint32_t OUT_TRAFFIC_PKTS_LO = 0x80;
constexpr uint32_t OUT_TRAFFIC_PKTS_HI = 0x84;

// If your design exposes throughput/time counters directly, add them here.
// Otherwise compute throughput from packet counts and elapsed wall time.
} // namespace tg_reg

enum class TgMode : uint32_t {
  PRODUCER = 0,
  CONSUMER = 1,
  LOOPBACK = 2,
  LATENCY  = 3
};

// 64-bit register read helper
static uint64_t read_u64(xrt::ip& ip, uint32_t lo_off, uint32_t hi_off) {
  uint32_t lo = ip.read_register(lo_off);
  uint32_t hi = ip.read_register(hi_off);
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

static void write_u64(xrt::ip& ip, uint32_t lo_off, uint32_t hi_off, uint64_t v) {
  ip.write_register(lo_off, static_cast<uint32_t>(v & 0xffffffffULL));
  ip.write_register(hi_off, static_cast<uint32_t>(v >> 32));
}

static bool wait_for_link(vnx::CMAC& cmac, int tries = 6, int sleep_s = 1) {
  for (int i = 0; i < tries; ++i) {
    auto st = cmac.link_status();
    if (st["rx_status"]) return true;
    std::this_thread::sleep_for(std::chrono::seconds(sleep_s));
  }
  return false;
}

static void tg_reset(xrt::ip& tg) {
  tg.write_register(tg_reg::DEBUG_RESET, 1);
}

static void tg_start_consumer(xrt::ip& tg) {
  tg.write_register(tg_reg::MODE, static_cast<uint32_t>(TgMode::CONSUMER));
  tg.write_register(tg_reg::START, 1);
}

static void tg_start_producer(xrt::ip& tg,
                              uint32_t socket_index,
                              uint64_t num_packets,
                              uint32_t beats,
                              uint32_t gap) {
  tg.write_register(tg_reg::MODE, static_cast<uint32_t>(TgMode::PRODUCER));
  tg.write_register(tg_reg::SOCKET_INDEX, socket_index);
  write_u64(tg, tg_reg::NUM_PACKETS_LO, tg_reg::NUM_PACKETS_HI, num_packets);
  tg.write_register(tg_reg::NUM_BEATS, beats);
  tg.write_register(tg_reg::GAP, gap);
  tg.write_register(tg_reg::START, 1);
}

static uint64_t tg_out_packets(xrt::ip& tg) {
  return read_u64(tg, tg_reg::OUT_TRAFFIC_PKTS_LO, tg_reg::OUT_TRAFFIC_PKTS_HI);
}

// ------------------------------
// Main
// ------------------------------

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr
      << "Usage:\n"
      << "  " << argv[0] << " <xclbin> [device_id]\n";
    return 1;
  }

  const std::string xclbin_path = argv[1];
  const int device_id = (argc >= 3) ? std::stoi(argv[2]) : 0;

  const auto xb = parse_xclbin(xclbin_path);

  xrt::device device(device_id);
  auto uuid = device.load_xclbin(xclbin_path);
  std::cout << "Loaded xclbin: " << xclbin_path << " on device " << device_id << "\n";

  std::this_thread::sleep_for(std::chrono::seconds(1));

  // Example shown for IF3, matching the notebook naming.
  // Adjust names if your xclbin has only one interface.
  vnx::CMAC cmac0{xrt::ip(device, uuid, "cmac_0:{cmac_0}")};
  vnx::CMAC cmac1{xrt::ip(device, uuid, "cmac_1:{cmac_1}")};
  vnx::Networklayer nl0{xrt::ip(device, uuid, "networklayer:{networklayer_0}")};
  vnx::Networklayer nl1{xrt::ip(device, uuid, "networklayer:{networklayer_1}")};

  cmac0.set_rs_fec(false);
  cmac1.set_rs_fec(false);

  bool link0 = wait_for_link(cmac0);
  bool link1 = wait_for_link(cmac1);

  std::cout << "Link interface cmac_0: " << (link0 ? "true" : "false") << "\n";
  std::cout << "Link interface cmac_1: " << (link1 ? "true" : "false") << "\n";

  if (!link0 || !link1) {
    std::cerr << "Link is not up on both interfaces.\n";
    return 2;
  }

  // Match the notebook:
  // remote card / interface 1:
  //   IP 192.168.0.10
  //   socket[1] = ('192.168.0.5', 62177, 60512, True)
  //
  // local card / interface 0:
  //   IP 192.168.0.5
  //   socket[7] = ('192.168.0.10', 60512, 62177, True)
  //
  // On a single card with IF3 you can still configure both interfaces this way.
  // Across two hosts, run analogous code on each host, one interface each.

  nl1.update_ip_address("192.168.0.10");
  nl1.configure_socket(1, "192.168.0.5", 62177, 60512, true);
  nl1.populate_socket_table();
  nl1.arp_discovery();

  nl0.update_ip_address("192.168.0.5");
  nl0.configure_socket(7, "192.168.0.10", 60512, 62177, true);
  nl0.populate_socket_table();
  nl0.arp_discovery();

  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto arp0 = nl0.read_arp_table(255);
  auto arp1 = nl1.read_arp_table(255);

  std::cout << "ARP table for networklayer_0:\n";
  for (const auto& [idx, entry] : arp0) {
    std::cout << "  [" << idx << "] MAC=" << entry.first << " IP=" << entry.second << "\n";
  }

  std::cout << "ARP table for networklayer_1:\n";
  for (const auto& [idx, entry] : arp1) {
    std::cout << "  [" << idx << "] MAC=" << entry.first << " IP=" << entry.second << "\n";
  }

  // Notebook uses:
  //   ol_w1.traffic_generator_1_1 as CONSUMER
  //   ol_w0.traffic_generator_1_3 as PRODUCER
  //
  // With XRT host code, open the matching IP names from your built xclbin.
  // Confirm names with:
  //   xclbinutil --dump-section IP_LAYOUT:JSON --input <xclbin>
  //
  // These names are plausible for IF3 benchmark builds, but verify them.
  xrt::ip tg_consumer(device, uuid, "traffic_generator_1_1:{traffic_generator_1_1}");
  xrt::ip tg_producer(device, uuid, "traffic_generator_1_3:{traffic_generator_1_3}");

  tg_reset(tg_consumer);
  tg_reset(tg_producer);

  tg_start_consumer(tg_consumer);

  const int freq_mhz = 250;  // confirm from your clock config if needed
  const int overhead = 8 + 20 + 14 + 4 + 12 + 7 + 1;

  for (uint64_t pkt_count : {1000000ULL, 1000000000ULL}) {
    for (uint32_t beats = 1; beats <= 23; ++beats) {
      tg_reset(tg_producer);
      tg_reset(tg_consumer);
      tg_start_consumer(tg_consumer);

      auto t0 = std::chrono::steady_clock::now();
      tg_start_producer(tg_producer, /*socket_index=*/7, pkt_count, beats, /*gap=*/0);

      while (tg_out_packets(tg_producer) != pkt_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
      }

      auto t1 = std::chrono::steady_clock::now();
      const double secs = std::chrono::duration<double>(t1 - t0).count();
      const double bytes = static_cast<double>(pkt_count) * static_cast<double>(beats * 64);
      const double gbps = (bytes * 8.0) / secs / 1e9;
      const double theoretical = (beats * 64.0 * 100.0) / ((beats * 64.0) + overhead);

      std::cout
        << "Sent " << pkt_count
        << " size=" << (beats * 64) << "B"
        << " time=" << secs << "s"
        << " tx_est=" << gbps << " Gbps"
        << " theoretical=" << theoretical << " Gbps\n";
    }
  }

  return 0;
}
