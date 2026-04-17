#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <xrt/xrt_device.h>
#include <xrt/xrt_uuid.h>
#include <xrt/experimental/xrt_ip.h>

#include "vnx/cmac.hpp"
#include "vnx/networklayer.hpp"

namespace tg_reg {
// ap_ctrl_hs
constexpr uint32_t AP_CTRL             = 0x00;

// From your meta.xml
constexpr uint32_t MODE                = 0x10;
constexpr uint32_t DEST_ID             = 0x14;
constexpr uint32_t NUMBER_PACKETS_LO   = 0x18;
constexpr uint32_t NUMBER_PACKETS_HI   = 0x1C;
constexpr uint32_t NUMBER_BEATS        = 0x20;
constexpr uint32_t TIME_BETWEEN_PKTS   = 0x24;
constexpr uint32_t RESET_FSM           = 0x28;
constexpr uint32_t DEBUG_FSMS          = 0x2C;

constexpr uint32_t OUT_TRAFFIC_CYCLES_LO  = 0x34;
constexpr uint32_t OUT_TRAFFIC_CYCLES_HI  = 0x38;
constexpr uint32_t OUT_TRAFFIC_BYTES_LO   = 0x3C;
constexpr uint32_t OUT_TRAFFIC_BYTES_HI   = 0x40;
constexpr uint32_t OUT_TRAFFIC_PKTS_LO    = 0x44;
constexpr uint32_t OUT_TRAFFIC_PKTS_HI    = 0x48;

constexpr uint32_t IN_TRAFFIC_CYCLES_LO   = 0x4C;
constexpr uint32_t IN_TRAFFIC_CYCLES_HI   = 0x50;
constexpr uint32_t IN_TRAFFIC_BYTES_LO    = 0x54;
constexpr uint32_t IN_TRAFFIC_BYTES_HI    = 0x58;
constexpr uint32_t IN_TRAFFIC_PKTS_LO     = 0x5C;
constexpr uint32_t IN_TRAFFIC_PKTS_HI     = 0x60;

constexpr uint32_t SUMMARY_CYCLES_LO      = 0x64;
constexpr uint32_t SUMMARY_CYCLES_HI      = 0x68;
constexpr uint32_t SUMMARY_BYTES_LO       = 0x6C;
constexpr uint32_t SUMMARY_BYTES_HI       = 0x70;
constexpr uint32_t SUMMARY_PKTS_LO        = 0x74;
constexpr uint32_t SUMMARY_PKTS_HI        = 0x78;

constexpr uint32_t DEBUG_RESET            = 0x7C;
}

enum class TgMode : uint32_t {
  PRODUCER = 0,
  CONSUMER = 1,
  LOOPBACK = 2,
  LATENCY  = 3
};

static uint64_t read_u64(xrt::ip& ip, uint32_t lo_off, uint32_t hi_off) {
  uint32_t lo = ip.read_register(lo_off);
  uint32_t hi = ip.read_register(hi_off);
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

static void write_u64(xrt::ip& ip, uint32_t lo_off, uint32_t hi_off, uint64_t v) {
  ip.write_register(lo_off, static_cast<uint32_t>(v & 0xffffffffULL));
  ip.write_register(hi_off, static_cast<uint32_t>(v >> 32));
}

static bool wait_for_link(vnx::CMAC& cmac, int tries = 10, int sleep_s = 1) {
  for (int i = 0; i < tries; ++i) {
    auto st = cmac.link_status();
    if (st["rx_status"]) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::seconds(sleep_s));
  }
  return false;
}

static void tg_debug_reset(xrt::ip& tg) {
  tg.write_register(tg_reg::DEBUG_RESET, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  tg.write_register(tg_reg::DEBUG_RESET, 0);
}

static void tg_reset_fsm(xrt::ip& tg) {
  tg.write_register(tg_reg::RESET_FSM, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  tg.write_register(tg_reg::RESET_FSM, 0);
}

static void tg_ap_start(xrt::ip& tg) {
  tg.write_register(tg_reg::AP_CTRL, 1);
}

static void tg_start_consumer(xrt::ip& tg) {
  tg_debug_reset(tg);
  tg_reset_fsm(tg);
  tg.write_register(tg_reg::MODE, static_cast<uint32_t>(TgMode::CONSUMER));
  tg_ap_start(tg);
}

static void tg_start_producer(xrt::ip& tg,
                              uint32_t socket_index,
                              uint64_t num_packets,
                              uint32_t beats,
                              uint32_t gap_cycles) {
  tg_debug_reset(tg);
  tg_reset_fsm(tg);

  tg.write_register(tg_reg::MODE, static_cast<uint32_t>(TgMode::PRODUCER));
  tg.write_register(tg_reg::DEST_ID, socket_index);
  write_u64(tg, tg_reg::NUMBER_PACKETS_LO, tg_reg::NUMBER_PACKETS_HI, num_packets);
  tg.write_register(tg_reg::NUMBER_BEATS, beats);
  tg.write_register(tg_reg::TIME_BETWEEN_PKTS, gap_cycles);
  tg_ap_start(tg);
}

int main(int argc, char* argv[]) {
  if (argc < 9) {
    std::cerr
      << "Usage:\n"
      << "  " << argv[0]
      << " <xclbin> <device_id> <role:producer|consumer> <local_ip> <remote_ip>"
      << " <local_port> <remote_port> <socket_index> [packets] [beats] [gap_cycles]\n\n"
      << "Example producer:\n"
      << "  " << argv[0]
      << " /path/to/vnx_benchmark_if0.xclbin 0 producer 192.168.0.5 192.168.0.10 60512 62177 0 1000000 23 0\n\n"
      << "Example consumer:\n"
      << "  " << argv[0]
      << " /path/to/vnx_benchmark_if0.xclbin 0 consumer 192.168.0.10 192.168.0.5 62177 60512 0 1000000 23 0\n";
    return 1;
  }

  const std::string xclbin_path = argv[1];
  const int device_id = std::stoi(argv[2]);
  const std::string role = argv[3];
  const std::string local_ip = argv[4];
  const std::string remote_ip = argv[5];
  const uint32_t local_port = static_cast<uint32_t>(std::stoul(argv[6]));
  const uint32_t remote_port = static_cast<uint32_t>(std::stoul(argv[7]));
  const uint32_t socket_index = static_cast<uint32_t>(std::stoul(argv[8]));
  const uint64_t packets = (argc >= 10) ? std::stoull(argv[9]) : 1000000ULL;
  const uint32_t beats = (argc >= 11) ? static_cast<uint32_t>(std::stoul(argv[10])) : 23U;
  const uint32_t gap_cycles = (argc >= 12) ? static_cast<uint32_t>(std::stoul(argv[11])) : 0U;

  try {
    xrt::device device(device_id);
    auto uuid = device.load_xclbin(xclbin_path);

    std::cout << "Loaded xclbin: " << xclbin_path
              << " on device " << device_id << "\n";

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // IF0 design
    vnx::CMAC cmac0{xrt::ip(device, uuid, "cmac_0:{cmac_0}")};
    vnx::Networklayer nl0{xrt::ip(device, uuid, "networklayer:{networklayer_0}")};

    cmac0.set_rs_fec(false);

    bool link0 = wait_for_link(cmac0);
    std::cout << "Link interface cmac_0: " << (link0 ? "true" : "false") << "\n";
    if (!link0) {
      std::cerr << "Link is not up on cmac_0.\n";
      return 2;
    }

    std::cout << "Setting local IP to " << local_ip << "\n";
    auto rc = nl0.update_ip_address(local_ip);
    std::cout << "update_ip_address rc = " << rc << "\n";

    std::cout << "Configuring socket to remote " << remote_ip
              << " local_port=" << local_port
              << " remote_port=" << remote_port
              << " socket_index=" << socket_index << "\n";

    nl0.configure_socket(socket_index, remote_ip, local_port, remote_port, true);
    nl0.populate_socket_table();

    for (int i = 0; i < 5; i++) {
      nl0.arp_discovery();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    auto arp0 = nl0.read_arp_table(255);
    std::cout << "ARP table for networklayer_0:\n";
    for (const auto& [idx, entry] : arp0) {
      std::cout << "  [" << idx << "] MAC=" << entry.first
                << " IP=" << entry.second << "\n";
    }

    xrt::ip tg_consumer(device, uuid, "traffic_generator:{traffic_generator_0_1}");
    xrt::ip tg_producer(device, uuid, "traffic_generator:{traffic_generator_0_3}");

    if (role == "consumer") {
      std::cout << "Starting consumer on traffic_generator_0_1\n";
      tg_start_consumer(tg_consumer);

      uint64_t last_pkts = 0;
      for (;;) {
        uint64_t in_pkts = read_u64(tg_consumer, tg_reg::IN_TRAFFIC_PKTS_LO, tg_reg::IN_TRAFFIC_PKTS_HI);
        uint64_t in_bytes = read_u64(tg_consumer, tg_reg::IN_TRAFFIC_BYTES_LO, tg_reg::IN_TRAFFIC_BYTES_HI);
        uint64_t in_cycles = read_u64(tg_consumer, tg_reg::IN_TRAFFIC_CYCLES_LO, tg_reg::IN_TRAFFIC_CYCLES_HI);

        if (in_pkts != last_pkts) {
          std::cout << "[consumer] pkts=" << in_pkts
                    << " bytes=" << in_bytes
                    << " cycles=" << in_cycles << "\n";
          last_pkts = in_pkts;
        }

        if (in_pkts >= packets) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

      uint64_t in_pkts = read_u64(tg_consumer, tg_reg::IN_TRAFFIC_PKTS_LO, tg_reg::IN_TRAFFIC_PKTS_HI);
      uint64_t in_bytes = read_u64(tg_consumer, tg_reg::IN_TRAFFIC_BYTES_LO, tg_reg::IN_TRAFFIC_BYTES_HI);
      uint64_t in_cycles = read_u64(tg_consumer, tg_reg::IN_TRAFFIC_CYCLES_LO, tg_reg::IN_TRAFFIC_CYCLES_HI);

      std::cout << "[consumer final] pkts=" << in_pkts
                << " bytes=" << in_bytes
                << " cycles=" << in_cycles << "\n";
    }
    else if (role == "producer") {
      std::cout << "Starting producer on traffic_generator_0_3\n";
      auto t0 = std::chrono::steady_clock::now();

      tg_start_producer(tg_producer, socket_index, packets, beats, gap_cycles);

      uint64_t last_pkts = 0;
      for (;;) {
        uint64_t out_pkts = read_u64(tg_producer, tg_reg::OUT_TRAFFIC_PKTS_LO, tg_reg::OUT_TRAFFIC_PKTS_HI);
        uint64_t out_bytes = read_u64(tg_producer, tg_reg::OUT_TRAFFIC_BYTES_LO, tg_reg::OUT_TRAFFIC_BYTES_HI);
        uint64_t out_cycles = read_u64(tg_producer, tg_reg::OUT_TRAFFIC_CYCLES_LO, tg_reg::OUT_TRAFFIC_CYCLES_HI);

        if (out_pkts != last_pkts) {
          std::cout << "[producer] pkts=" << out_pkts
                    << " bytes=" << out_bytes
                    << " cycles=" << out_cycles << "\n";
          last_pkts = out_pkts;
        }

        if (out_pkts >= packets) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

      auto t1 = std::chrono::steady_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();

      uint64_t out_pkts = read_u64(tg_producer, tg_reg::OUT_TRAFFIC_PKTS_LO, tg_reg::OUT_TRAFFIC_PKTS_HI);
      uint64_t out_bytes = read_u64(tg_producer, tg_reg::OUT_TRAFFIC_BYTES_LO, tg_reg::OUT_TRAFFIC_BYTES_HI);
      uint64_t out_cycles = read_u64(tg_producer, tg_reg::OUT_TRAFFIC_CYCLES_LO, tg_reg::OUT_TRAFFIC_CYCLES_HI);

      double gbps_wall = (secs > 0.0) ? (8.0 * static_cast<double>(out_bytes) / secs / 1e9) : 0.0;

      std::cout << "[producer final] pkts=" << out_pkts
                << " bytes=" << out_bytes
                << " cycles=" << out_cycles
                << " wall_time=" << secs
                << " wall_gbps=" << gbps_wall << "\n";
    }
    else {
      throw std::runtime_error("role must be producer or consumer");
    }

    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 3;
  }
}
