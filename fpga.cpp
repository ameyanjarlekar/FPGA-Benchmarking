// host_benchmark.cpp
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

#include <xrt/xrt_device.h>
#include <xrt/xrt_uuid.h>
#include <xrt/experimental/xrt_ip.h>

#include "vnx/cmac.hpp"
#include "vnx/networklayer.hpp"

namespace fs = std::filesystem;

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

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <xclbin> [device_id]\n";
    return 1;
  }

  const std::string xclbin_path = argv[1];
  const int device_id = (argc >= 3) ? std::stoi(argv[2]) : 0;

  try {
    xrt::device device(device_id);
    auto uuid = device.load_xclbin(xclbin_path);

    std::cout << "Loaded xclbin: " << xclbin_path
              << " on device " << device_id << "\n";

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // IF0 design: use only interface 0 IP blocks
    vnx::CMAC cmac0{xrt::ip(device, uuid, "cmac_0:{cmac_0}")};
    vnx::Networklayer nl0{xrt::ip(device, uuid, "networklayer:{networklayer_0}")};

    cmac0.set_rs_fec(false);

    bool link0 = wait_for_link(cmac0);

    std::cout << "Link interface cmac_0: "
              << (link0 ? "true" : "false") << "\n";

    if (!link0) {
      std::cerr << "Link is not up on cmac_0.\n";
      return 2;
    }

    // Match one side of the notebook’s setup.
    // You can change these IP/ports later depending on which host/card is local.
    nl0.update_ip_address("192.168.0.5");
    nl0.configure_socket(
        0,                // socket index
        "192.168.0.10",   // remote IP
        60512,            // local port
        62177,            // remote port
        true              // enable
    );
    nl0.populate_socket_table();
    nl0.arp_discovery();

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto arp0 = nl0.read_arp_table(255);

    std::cout << "ARP table for networklayer_0:\n";
    for (const auto& [idx, entry] : arp0) {
      std::cout << "  [" << idx << "] MAC=" << entry.first
                << " IP=" << entry.second << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 3;
  }
}
