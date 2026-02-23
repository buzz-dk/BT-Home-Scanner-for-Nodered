// Shelly_Data_Reader_Console.cpp
// 
// A console application that scans for BLE advertisements,
// extracts the service data from devices with a specific header (D2 FC) == BTHome format,
// and sends the data as a JSON formatted string over TCP to Node-RED:
// {"addr":"0c:ef:f6:f0:d1:24","data":[68,0,93,1,99,46,43,58,1,69,237,0],"rssi":-41}
// The output includes the device's MAC address, the service data (without the D2 FC header),
// and the RSSI value. The application runs until the user presses the Escape key.
// 
// Purpose: To read BLE advertisements from Shelly devices (or any device using the BTHome format)
// and send the relevant data in a structured JSON format directly to Node-RED via TCP.
// Node-red have a few nodes for BLE scanning, but they seem to be old, unmaintained and unreliable,
// so this custom scanner can be used as a more robust alternative.
// The JSON can be parsed in Node-RED using a JSON node,
// allowing for further processing or integration with other systems.
//
// Requirements:
// - Windows 10 or later (for C++/WinRT support)
// - Bluetooth 4.0 or later adapter
//
// How to use:
// Run the executable with optional arguments:  Shelly_Data_Reader_Console.exe [host] [port]
// Default: connects to 127.0.0.1:5000
// In Node-RED, use a "tcp in" node configured to listen on the same port.
// The Node-red flow supplied with this project expects the executable file to be placed in the .nodered folder,
// but you can run it from anywhere as long as you provide the correct host and port.

// ------------- C++ Standard Library ----------------------------------------------------------------

#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <iomanip>                                          // std::setw, std::setfill
#include <sstream>                                          // std::sstream
#include <string>                                           // std::string
#include <unordered_map>                                    // std::unordered_map
#include <vector>   

// ------------- System / Win32 / Winsock ------------------------------------------------------------
#include <winsock2.h>                                       // Winsock2 (must be before windows.h)
#include <ws2tcpip.h>                                       // getaddrinfo, inet_pton
#include <windows.h>                                        // Core Win32 API

// ------------- C++/WinRT ---------------------------------------------------------------------------
#include <winrt/base.h>                                     // C++/WinRT helpers (com_ptr, hresult…)
#include <winrt/Windows.Foundation.h>                       // HSTRING, IAsync…​, TimeSpan
#include <winrt/Windows.Foundation.Collections.h>           // IIterable, IVector, IMap…
#include <winrt/Windows.Storage.Streams.h>                  // IBuffer, DataReader/DataWriter
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>  // BLE-advertisement APIs

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Foundation::Collections;

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ws2_32.lib")

// --------------- TCP Client -----------------------------------------------------------------------

class TcpClient
{
public:
  TcpClient(const std::string& host, uint16_t port)
    : host_(host), port_(port), sock_(INVALID_SOCKET)
  {
  }

  ~TcpClient() { Disconnect(); }

  bool Connect()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return ConnectInternal();
  }

  void Disconnect()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseSocket();
  }

  // Send a string. Appends '\n' as the message delimiter for Node-RED "tcp in" node.
  // Before sending, checks whether the remote end has closed the connection (e.g. after
  // a Node-RED re-deploy) and reconnects automatically if needed.
  bool Send(const std::string& message)
  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Detect remote close (FIN) that may have arrived since the last send
    if (sock_ != INVALID_SOCKET && !IsSocketAlive())
    {
      std::cerr << "[TCP] Remote end closed connection." << std::endl;
      CloseSocket();
    }

    // Auto-reconnect if disconnected
    if (sock_ == INVALID_SOCKET) {
      if (!ConnectInternal()) return false;
      std::cout << "[TCP] Reconnected." << std::endl;
    }

    std::string data = message + "\n";
    int sent = send(sock_, data.c_str(), (int)data.size(), 0);
    if (sent == SOCKET_ERROR) {
      std::cerr << "[TCP] send() failed (error " << WSAGetLastError() << ")." << std::endl;
      CloseSocket();
      return false;
    }
    return true;
  }

  bool IsConnected()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return sock_ != INVALID_SOCKET;
  }

private:
  std::string host_;
  uint16_t    port_;
  SOCKET      sock_;
  std::mutex  mutex_;

  // Must be called with mutex_ held.
  bool ConnectInternal()
  {
    if (sock_ != INVALID_SOCKET) return true; // already connected

    struct addrinfo hints{}, * result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string portStr = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &result) != 0)
      return false;

    sock_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock_ == INVALID_SOCKET) {
      freeaddrinfo(result);
      return false;
    }

    // Enable TCP keep-alive so the OS can detect a dead peer even when we are idle
    BOOL keepAlive = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive));

    if (connect(sock_, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
      closesocket(sock_);
      sock_ = INVALID_SOCKET;
      freeaddrinfo(result);
      return false;
    }

    freeaddrinfo(result);
    return true;
  }

  // Must be called with mutex_ held.
  void CloseSocket()
  {
    if (sock_ != INVALID_SOCKET) {
      shutdown(sock_, SD_BOTH);
      closesocket(sock_);
      sock_ = INVALID_SOCKET;
    }
  }

  // Non-blocking check whether the remote end has sent a FIN (graceful close).
  // Uses select() + recv(MSG_PEEK) so no data is consumed.
  // Must be called with mutex_ held.
  bool IsSocketAlive()
  {
    if (sock_ == INVALID_SOCKET) return false;

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock_, &readfds);

    timeval tv{};           // zero timeout → non-blocking poll
    int sel = select(0, &readfds, nullptr, nullptr, &tv);

    if (sel > 0)
    {
      // Socket is readable: either unexpected incoming data, or a FIN arrived
      char buf;
      int r = recv(sock_, &buf, 1, MSG_PEEK);
      if (r == 0) return false;   // FIN – remote closed gracefully
      if (r < 0)  return false;   // error
    }
    else if (sel < 0)
    {
      return false;               // select() error
    }
    // sel == 0: nothing pending → connection still alive
    return true;
  }
};

// Global TCP client instance (set up in main before scanner starts)
static TcpClient* g_tcpClient = nullptr;

// --------------- Converter ------------------------------------------------------------------------

class Converter
{
public:
  // Format MAC address as “aa:bb:cc:dd:ee:ff”
  static std::string FormatMac(uint64_t addr)
  {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int b = 5; b >= 0; --b) {
      ss << std::setw(2) << ((addr >> (b * 8)) & 0xFF);
      if (b) ss << ':';
    }
    return ss.str();
  }
};

class BleBeaconScanner
{
public:
  // Call once
  static void Initialize()
  {
    watcher = BluetoothLEAdvertisementWatcher{};
    watcher.ScanningMode(BluetoothLEScanningMode::Passive); // We have no use for scan responses
    watcher.Received(OnAdvertisementReceived);
    watcher.Start();
  }

  static void Shutdown()
  {
    watcher.Stop();
  }

  // Device data struct:
  struct BeaconData
  {
    uint64_t address{};
    std::vector<uint8_t> serviceData;
    int16_t rssi{};
  };

  // Store one Beacon advertisement per device:
  // Used for comparison with previous advertisements in order to avoid duplicates.
  static inline std::unordered_map<uint64_t, BeaconData> beacons;

private:
  static inline BluetoothLEAdvertisementWatcher watcher{ nullptr };

  // WinRT callback
  static void OnAdvertisementReceived(BluetoothLEAdvertisementWatcher const&,
    BluetoothLEAdvertisementReceivedEventArgs const& args)
  {
    // Gather the values contained in *this* advertisement PDU
    BeaconData incoming;
    incoming.address = args.BluetoothAddress();
    incoming.rssi = args.RawSignalStrengthInDBm();

    // ----- extract “Service Data – 16-bit UUID” blocks (type 0x16) -----
    constexpr uint8_t ServiceData16Bit = 0x16;

    incoming.serviceData.clear(); // reset vector

    for (auto const& section : args.Advertisement().DataSections())
    {
      if (section.DataType() == ServiceData16Bit)
      {
        auto reader = DataReader::FromBuffer(section.Data());
        uint32_t len = reader.UnconsumedBufferLength();

        incoming.serviceData.resize(len);          //  make room
        reader.ReadBytes(incoming.serviceData);    //  fill vector
        break;                                     // keep first 0x16
      }
    }

    if (incoming.serviceData.size() > 2 && incoming.serviceData[0] == 0xD2 && incoming.serviceData[1] == 0xFC)
    {
      // Create entry or get existing:
      auto& entry = beacons[incoming.address];      // inserts default if new
      bool changed = false;

      // RSSI – always keep the latest reading.
      entry.rssi = incoming.rssi;
      //changed = true; // we only output on name/serviceData changes, not RSSI changes

      if (!incoming.serviceData.empty() && entry.serviceData != incoming.serviceData)
      {
        entry.serviceData = std::move(incoming.serviceData); // Note: incoming.serviceData is no longer valid
        changed = true; // We got new service data.
      }

      // Output JSON only if something changed, Note: BLE devices often send bursts of advertisements with the same data,
      // so this helps reduce noise in the output.
      // Shelly sensors typically send new readings every minute, and data will then change due to a counter in the service data.
      if (changed)
      {
        std::string addressStr = Converter::FormatMac(incoming.address);

        // Remove the first two bytes from the serviceData vector (D2 FC), as expected by nodered:
        std::vector<uint8_t> serviceDataWithoutHeader(entry.serviceData.begin() + 2, entry.serviceData.end());

        // Build JSON array of decimal byte values: [68,0,93,1,...]
        std::ostringstream dataArray;
        dataArray << "[";
        for (size_t i = 0; i < serviceDataWithoutHeader.size(); ++i)
        {
          dataArray << int(serviceDataWithoutHeader[i]);
          if (i + 1 < serviceDataWithoutHeader.size()) dataArray << ',';
        }
        dataArray << "]";

        // Build the complete JSON string, Note: rssi is not really used for anything in Node-red(?),
        // but it can be helpful for debugging or filtering devices based on signal strength.
        std::ostringstream jsonStream;
        jsonStream << "{"
          << "\"addr\":\"" << addressStr << "\","
          << "\"data\":" << dataArray.str() << ","
          << "\"rssi\":" << entry.rssi
          << "}";

        std::string json = jsonStream.str();

        // Send over TCP to Node-RED; fall back to stdout if send fails
        if (g_tcpClient && g_tcpClient->Send(json))
        {
          // optionally echo to console for debugging
          std::cout << json << std::endl;
        }
        else
        {
          std::cerr << "[TCP] send failed: " << json << std::endl;
        }
      }
    }
  }
};

int main(int argc, char* argv[])
{
  // --- Parse optional command-line arguments: [host] [port] ---

  std::string host = "127.0.0.1"; // localhost
  uint16_t    port = 5000;

  if (argc >= 2) host = argv[1];
  if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));

  // --- Initialize Winsock ---
  WSADATA wsaData{};
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "[TCP] WSAStartup failed." << std::endl;
    return 1;
  }

  // --- Connect to Node-RED ---
  TcpClient client(host, port);
  g_tcpClient = &client;

  std::cout << "[TCP] Connecting to " << host << ":" << port << " ..." << std::endl;
  if (client.Connect()) {
    std::cout << "[TCP] Connected." << std::endl;
  }
  else {
    std::cerr << "[TCP] Initial connection failed – will retry on first BLE event." << std::endl;
  }

  // --- Start BLE scanning ---
  BleBeaconScanner::Initialize();
  std::cout << "[BLE] Scanning... Press Escape to stop." << std::endl;

  while (true)
  {
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // --- Cleanup ---
  BleBeaconScanner::Shutdown();
  g_tcpClient = nullptr;
  client.Disconnect();
  WSACleanup();

  std::cout << "[TCP] Disconnected. Exiting." << std::endl;
  return 0;
}
