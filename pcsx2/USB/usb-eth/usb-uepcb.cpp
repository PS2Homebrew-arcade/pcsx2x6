// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "USB/qemu-usb/qusb.h"
#include "USB/qemu-usb/desc.h"
#include "USB/qemu-usb/USBinternal.h"
#include "USB/usb-eth/usb-uepcb.h"
#include "USB/USB.h"
#include "common/Console.h"
#include "StateWrapper.h"
#include "IopMem.h" // runtime IOP-side patches for the game's network stack (see uepcb_handle_data)

#include <cstring>
#include <cstdio>
#include <vector>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h> // firewall rule elevation (ShellExecuteEx "runas")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
using socket_t = SOCKET;
#define UEPCB_INVALID_SOCKET INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define UEPCB_INVALID_SOCKET (-1)
#endif

namespace usb_uepcb
{
	// ---- UE PCB (ADMtek Pegasus / AN986) USB descriptors ----
	// VID 0x0B9A (Namco), PID 0x0500. EP1 IN bulk(0x81), EP2 OUT bulk(0x02),
	// EP3 IN interrupt(0x83). All bulk maxpkt 64. Values match a real UE PCB's
	// enumeration (originally ported from Play!'s HLE UsbUePcbDevice).
	static const u8 uepcb_dev_descriptor[] = {
		0x12, 0x01, 0x10, 0x01, 0xFF, 0x00, 0x00, 0x40,
		0x9A, 0x0B, 0x00, 0x05, 0x01, 0x02, 0x01, 0x02, 0x03, 0x01};

	static const u8 uepcb_config_descriptor[] = {
		// config (9): wTotalLength=0x27(39), 1 iface, self-powered, 100mA
		0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0xC0, 0x32,
		// interface (9): 3 endpoints, vendor class
		0x09, 0x04, 0x00, 0x00, 0x03, 0xFF, 0x00, 0x00, 0x00,
		// EP1 IN bulk (7)
		0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
		// EP2 OUT bulk (7)
		0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
		// EP3 IN interrupt (7): maxpkt 8, interval 10
		0x07, 0x05, 0x83, 0x03, 0x08, 0x00, 0x0A};

	static const char* uepcb_strings[] = {
		"", "Namco", "UE PCB", ""};

	typedef struct UePcbState
	{
		USBDevice dev{};
		USBDesc desc{};
		USBDescDevice desc_dev{};

		// AN986 chip register shadow. Reg 0x10-0x15 = MAC address.
		u8 an986_regs[0x40] = {};
		u8 eeprom_word = 0; // last EEPROM word selected via reg 0x20 write
		// The AN986 driver splits ethernet frames larger than the 64-byte USB
		// max packet size across multiple BULK OUT packets; accumulate until
		// the frame is complete (length prefix) before forwarding.
		std::vector<u8> tx_accum;

		// Netplay peer transport. Role: 1P = host (listens), 2P-4P = join
		// (connect to the host). The host also acts as an N-way hub, flooding
		// every frame to all other peers, which handles both 2-player and
		// 4-player games with one code path.
		bool is_host = false;
		u8 mac[6] = {0x00, 0x90, 0x2E, 0x11, 0x22, 0x33}; // this instance's NIC MAC
		int peer_port = 7500;
		std::string peer_ip; // join: host address to connect to
		std::string bind_ip; // host: listen address
		u8 mii_phyaddr = 1;  // PHY addr selected by the last reg 0x25 write
		u8 mii_reg = 1;      // MII register selected by the last reg 0x25 write
		bool init_done = false;       // reg 0x7C written = NIC setup complete
		bool link_event_sent = false; // link-up interrupt already delivered once
		std::mutex in_lock;
		std::deque<std::vector<u8>> in_q; // inbound frames (already BULK IN framed)
		std::mutex sock_lock;
		socket_t listen_sock = UEPCB_INVALID_SOCKET;
		std::thread peer_thread;
		std::atomic<bool> peer_stop{false};
		std::mutex peers_lock;
		std::vector<socket_t> peers; // host: all joined peers / join: 1 hub connection
	} UePcbState;

	// ---------------- peer transport helpers ----------------
	static void wsa_ensure()
	{
#ifdef _WIN32
		static bool init = []() {
			WSADATA w;
			WSAStartup(MAKEWORD(2, 2), &w);
			return true;
		}();
		(void)init;
#endif
	}

	static void sock_close(socket_t x)
	{
#ifdef _WIN32
		closesocket(x);
#else
		close(x);
#endif
	}

	// Role auto-detection: every PC enters the same "1P's IP" value; the PC
	// whose own (non-loopback) local IP matches becomes the host. Detected by
	// attempting to bind the address - bind succeeds only for local IPs.
	// 127.x is excluded so a second instance on the same PC joins instead.
	static bool ip_is_own(const std::string& ip)
	{
		if (ip.empty() || ip == "0.0.0.0" || ip.rfind("127.", 0) == 0)
			return false;
		wsa_ensure();
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = 0;
		if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1)
			return false;
		socket_t t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (t == UEPCB_INVALID_SOCKET)
			return false;
		const bool local = (bind(t, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
		sock_close(t);
		return local;
	}

	static bool recv_exact(socket_t c, u8* buf, int len)
	{
		int off = 0;
		while (off < len)
		{
			const int n = recv(c, reinterpret_cast<char*>(buf + off), len - off, 0);
			if (n <= 0)
				return false;
			off += n;
		}
		return true;
	}

	static bool send_exact(socket_t c, const u8* buf, int len)
	{
		int off = 0;
		while (off < len)
		{
			const int n = send(c, reinterpret_cast<const char*>(buf + off), len - off, 0);
			if (n <= 0)
				return false;
			off += n;
		}
		return true;
	}

	// AN986 MII register values, modeled on a real UE PCB. Only PHY address 1
	// exists; regs 2/3 are the PHY ID, which the game's driver validates.
	static u16 mii_val(u8 reg)
	{
		switch (reg)
		{
			case 0x00: return 0x3100; // BMCR
			case 0x01: return 0x786d; // BMSR: link-up + autoneg-complete
			case 0x02: return 0x001d; // PHY ID hi
			case 0x03: return 0x2411; // PHY ID lo
			case 0x04: return 0x05e1; // ANAR
			case 0x05: return 0x0001; // ANLPAR
			default: return 0x0000;
		}
	}

	// Generate the response for a vendor register read (bRequest 0xF0).
	// Output is `length` bytes, zero padded. handle_control applies further
	// overlays (MAC/EEPROM/link bits) on top of this afterwards.
	static void an986_read_local(UePcbState* s, int reg, int length, u8* data)
	{
		u8 tmp[8] = {};
		int n = 0;
		if (reg == 0x2B && s->init_done && !s->link_event_sent)
		{
			// Deliver a link-up interrupt event exactly once, right after the
			// driver finishes NIC setup. Without real PHY interrupts the
			// driver would otherwise never see the link come up.
			tmp[0] = 0x18; tmp[1] = 0x01; tmp[3] = 0x60; n = 5;
			s->link_event_sent = true;
		}
		else if (reg == 0x25)
		{
			const u16 val = (s->mii_phyaddr == 1) ? mii_val(s->mii_reg) : 0xFFFF;
			tmp[0] = s->mii_phyaddr;
			tmp[1] = val & 0xFF;
			tmp[2] = (val >> 8) & 0xFF;
			tmp[3] = 0x80 | (s->mii_reg & 0x1F);
			n = 4;
		}
		else if (reg == 0x28)
		{
			tmp[0] = 0x80 | (s->mii_reg & 0x1F);
			n = 2;
		}
		else if (reg == 0x2B)
		{
			tmp[3] = 0x60; n = 5; // idle, 100M link
		}
		else if (reg == 0x23)
		{
			tmp[0] = 0x04; n = 2; // EEPROM/PHY done bit
		}
		else if (reg == 0x21)
		{
			tmp[2] = 0x04; n = 3; // EEPROM data + done; words 0-2 carry the MAC
			if (s->eeprom_word < 3)
			{
				tmp[0] = s->mac[s->eeprom_word * 2];
				tmp[1] = s->mac[s->eeprom_word * 2 + 1];
			}
		}
		else if (reg == 0x10)
		{
			std::memcpy(tmp, s->mac, 6); n = 6;
		}
		else if (reg == 0x01)
		{
			n = 2;
		}
		else
		{
			n = length; // unknown register -> zeros
		}
		std::memset(data, 0, length);
		const int copy = std::min<int>(std::min<int>(length, n), (int)sizeof(tmp));
		if (copy > 0)
			std::memcpy(data, tmp, copy);
	}

	// Wrap an ethernet frame in the AN986 BULK IN (RX) format:
	// [2-byte LE length = frame length + 8][frame][8 zero bytes].
	static std::vector<u8> to_bulkin(const u8* eth, int len)
	{
		std::vector<u8> v;
		v.reserve(2 + len + 8);
		const int p = len + 8;
		v.push_back(static_cast<u8>(p & 0xFF));
		v.push_back(static_cast<u8>((p >> 8) & 0xFF));
		v.insert(v.end(), eth, eth + len);
		v.insert(v.end(), 8, 0);
		return v;
	}

	static void push_in_q(UePcbState* s, std::vector<u8> v)
	{
		std::lock_guard<std::mutex> lk(s->in_lock);
		if (s->in_q.size() >= 4096)
			s->in_q.pop_front(); // drop-oldest
		s->in_q.push_back(std::move(v));
	}

	static bool pop_in_q(UePcbState* s, std::vector<u8>& out)
	{
		std::lock_guard<std::mutex> lk(s->in_lock);
		if (s->in_q.empty())
			return false;
		out = std::move(s->in_q.front());
		s->in_q.pop_front();
		return true;
	}

	// Receive loop for one peer connection (join side). Wire format is
	// [4-byte BE length][raw ethernet frame].
	static void peer_recv_loop(UePcbState* s, socket_t conn)
	{
		while (!s->peer_stop)
		{
			u8 hdr[4];
			if (!recv_exact(conn, hdr, 4))
				break;
			const u32 ln = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
			if (ln < 14 || ln > 2048)
				break;
			std::vector<u8> eth(ln);
			if (!recv_exact(conn, eth.data(), ln))
				break;
			push_in_q(s, to_bulkin(eth.data(), static_cast<int>(eth.size())));
		}
	}

	// Send a raw ethernet frame to every connected peer.
	static void peer_send_all(UePcbState* s, const u8* eth, int len)
	{
		const u8 hdr[4] = {static_cast<u8>((len >> 24) & 0xFF), static_cast<u8>((len >> 16) & 0xFF),
			static_cast<u8>((len >> 8) & 0xFF), static_cast<u8>(len & 0xFF)};
		std::lock_guard<std::mutex> lk(s->peers_lock);
		for (socket_t p : s->peers)
		{
			if (p != UEPCB_INVALID_SOCKET)
			{
				send_exact(p, hdr, 4);
				send_exact(p, eth, len);
			}
		}
	}

	static void hub_remove(UePcbState* s, socket_t p)
	{
		std::lock_guard<std::mutex> lk(s->peers_lock);
		for (auto it = s->peers.begin(); it != s->peers.end(); ++it)
		{
			if (*it == p)
			{
				s->peers.erase(it);
				break;
			}
		}
		sock_close(p);
	}

	// Host relays a received frame to every peer except the sender (dumb L2 hub).
	static void hub_flood(UePcbState* s, socket_t src, const u8* eth, int len)
	{
		const u8 hdr[4] = {static_cast<u8>((len >> 24) & 0xFF), static_cast<u8>((len >> 16) & 0xFF),
			static_cast<u8>((len >> 8) & 0xFF), static_cast<u8>(len & 0xFF)};
		std::lock_guard<std::mutex> lk(s->peers_lock);
		for (socket_t p : s->peers)
		{
			if (p != src && p != UEPCB_INVALID_SOCKET)
			{
				send_exact(p, hdr, 4);
				send_exact(p, eth, len);
			}
		}
	}

	// Join side: keep one connection to the host, reconnecting every ~2s.
	static void hub_join_loop(UePcbState* s)
	{
		while (!s->peer_stop)
		{
			socket_t conn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (conn != UEPCB_INVALID_SOCKET)
			{
				sockaddr_in a{};
				a.sin_family = AF_INET;
				a.sin_port = htons(static_cast<u16>(s->peer_port));
				inet_pton(AF_INET, s->peer_ip.c_str(), &a.sin_addr);
				if (connect(conn, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
				{
					int nd = 1;
					setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
					{
						std::lock_guard<std::mutex> lk(s->peers_lock);
						s->peers.assign(1, conn);
					}
					{
						std::lock_guard<std::mutex> lk(s->in_lock);
						s->in_q.clear();
					}
					Console.WriteLn("UePCB: netplay JOIN connected %s:%d", s->peer_ip.c_str(), s->peer_port);
					peer_recv_loop(s, conn);
					{
						std::lock_guard<std::mutex> lk(s->peers_lock);
						if (!s->peers.empty())
						{
							sock_close(s->peers[0]);
							s->peers.clear();
						}
					}
				}
				else
					sock_close(conn);
			}
			for (int i = 0; i < 20 && !s->peer_stop; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	// Host side: select-based accept/receive loop. Every received frame goes
	// to our own inbound queue (the host is a player too) and is flooded to
	// all other peers.
	static void hub_host_loop(UePcbState* s)
	{
		socket_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (ls == UEPCB_INVALID_SOCKET)
			return;
		int one = 1;
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(static_cast<u16>(s->peer_port));
		inet_pton(AF_INET, s->bind_ip.c_str(), &a.sin_addr);
		if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
		{
			Console.Error("UePCB: netplay host bind %s:%d FAILED", s->bind_ip.c_str(), s->peer_port);
			sock_close(ls);
			return;
		}
		listen(ls, 8);
		{
			std::lock_guard<std::mutex> lk(s->sock_lock);
			s->listen_sock = ls;
		}
		Console.WriteLn("UePCB: netplay HOST listening %s:%d", s->bind_ip.c_str(), s->peer_port);
		while (!s->peer_stop)
		{
			fd_set rf;
			FD_ZERO(&rf);
			FD_SET(ls, &rf);
			socket_t maxfd = ls;
			{
				std::lock_guard<std::mutex> lk(s->peers_lock);
				for (socket_t p : s->peers)
				{
					FD_SET(p, &rf);
					if (p > maxfd)
						maxfd = p;
				}
			}
			timeval tv{0, 500000};
			const int r = select(static_cast<int>(maxfd) + 1, &rf, nullptr, nullptr, &tv);
			if (s->peer_stop)
				break;
			if (r <= 0)
				continue;
			if (FD_ISSET(ls, &rf))
			{
				socket_t conn = accept(ls, nullptr, nullptr);
				if (conn != UEPCB_INVALID_SOCKET)
				{
					int nd = 1;
					setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
					size_t cnt;
					{
						std::lock_guard<std::mutex> lk(s->peers_lock);
						s->peers.push_back(conn);
						cnt = s->peers.size();
					}
					Console.WriteLn("UePCB: netplay peer connected (%d total)", static_cast<int>(cnt));
				}
			}
			std::vector<socket_t> snap;
			{
				std::lock_guard<std::mutex> lk(s->peers_lock);
				snap = s->peers;
			}
			for (socket_t p : snap)
			{
				if (!FD_ISSET(p, &rf))
					continue;
				u8 hdr[4];
				if (!recv_exact(p, hdr, 4))
				{
					hub_remove(s, p);
					continue;
				}
				const u32 ln = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
				if (ln < 14 || ln > 2048)
				{
					hub_remove(s, p);
					continue;
				}
				std::vector<u8> eth(ln);
				if (!recv_exact(p, eth.data(), ln))
				{
					hub_remove(s, p);
					continue;
				}
				push_in_q(s, to_bulkin(eth.data(), static_cast<int>(ln))); // our own RX
				hub_flood(s, p, eth.data(), static_cast<int>(ln));         // relay to others
			}
		}
		std::lock_guard<std::mutex> lk(s->peers_lock);
		for (socket_t p : s->peers)
			if (p != UEPCB_INVALID_SOCKET)
				sock_close(p);
		s->peers.clear();
	}

	static void peer_thread_main(UePcbState* s)
	{
		wsa_ensure();
		if (s->is_host)
			hub_host_loop(s);
		else
			hub_join_loop(s);
	}

#ifdef _WIN32
	// Host side: allow the netplay TCP port through Windows Firewall. Checks
	// for the rule first so the UAC prompt appears only once; the rule
	// persists across sessions.
	static bool fw_rule_exists(const char* rule)
	{
		std::string cmd = "netsh advfirewall firewall show rule name=\"" + std::string(rule) + "\"";
		STARTUPINFOA si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi{};
		std::vector<char> c(cmd.begin(), cmd.end());
		c.push_back('\0');
		if (!CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
			return false;
		WaitForSingleObject(pi.hProcess, 5000);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return code == 0; // netsh exits 0 if the rule exists
	}

	static void open_firewall_port(int port)
	{
		char rule[64];
		std::snprintf(rule, sizeof(rule), "uepcb-netplay-%d", port);
		if (fw_rule_exists(rule))
			return;
		char params[256];
		std::snprintf(params, sizeof(params),
			"advfirewall firewall add rule name=\"%s\" dir=in action=allow protocol=TCP localport=%d",
			rule, port);
		SHELLEXECUTEINFOA sei{};
		sei.cbSize = sizeof(sei);
		sei.fMask = SEE_MASK_NOCLOSEPROCESS;
		sei.lpVerb = "runas"; // elevation (UAC)
		sei.lpFile = "netsh";
		sei.lpParameters = params;
		sei.nShow = SW_HIDE;
		if (ShellExecuteExA(&sei) && sei.hProcess)
		{
			WaitForSingleObject(sei.hProcess, 15000);
			CloseHandle(sei.hProcess);
			Console.WriteLn("UePCB: firewall inbound allow rule added for TCP %d", port);
		}
		else
			Console.Warning("UePCB: firewall rule declined/failed - allow TCP %d manually if joins can't connect", port);
	}
#endif

	// ---------------- USB handlers ----------------
	static void uepcb_handle_reset(USBDevice* dev)
	{
		UePcbState* s = USB_CONTAINER_OF(dev, UePcbState, dev);
		std::memset(s->an986_regs, 0, sizeof(s->an986_regs));
		std::memcpy(s->an986_regs + 0x10, s->mac, 6); // MAC regs
	}

	static void uepcb_handle_control(USBDevice* dev, USBPacket* p, int request,
		int value, int index, int length, u8* data)
	{
		UePcbState* s = USB_CONTAINER_OF(dev, UePcbState, dev);
		if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0)
			return;

		const u8 bRequest = request & 0xFF;

		// AN986 vendor protocol: 0xF0 = read N bytes from chip reg at wIndex,
		// 0xF1 = write N bytes to chip reg at wIndex.
		if (bRequest == 0xF0 || bRequest == 0xF1)
		{
			if (s->an986_regs[0x10] == 0)
				std::memcpy(s->an986_regs + 0x10, s->mac, 6);

			if (bRequest == 0xF0) // IN: return register data
			{
				an986_read_local(s, index, length, data);

				// PHY link-up overlays. The real chip reports link-down while
				// the RJ-45 has no physical link; force the bits the driver
				// checks so AN986.IRX proceeds past PHY scan into bulk setup.
				if (index == 0x20 && length >= 2)
				{
					data[0] = 0x2D;
					data[1] = 0x78; // 0x782D = link-up, 100M-FDX, ANEG-done
				}
				else if (index == 0x25 && length >= 2)
				{
					// The driver requires (BMSR & 0x24) == 0x24 (link bit 2 +
					// autoneg-complete bit 5) before it accepts the link.
					data[1] |= 0x24;
				}
				else if (index == 0x2B && length >= 4)
				{
					// Byte 3 (= chip reg 0x2E) carries link/speed bits; the
					// driver's link check tests (byte3 & 0x6c).
					data[3] |= 0x60;
				}
				// MAC override (reg 0x10-0x15): the NIC must report OUR MAC so
				// inbound frames addressed to it are accepted by the driver.
				for (int i = 0; i < length; i++)
				{
					const int reg = index + i;
					if (reg >= 0x10 && reg < 0x16)
						data[i] = s->mac[reg - 0x10];
				}
				// EEPROM MAC override: the game reads the MAC from EEPROM
				// (reg 0x20 selects the word, reg 0x21 returns it) and then
				// writes it to reg 0x10 itself, so the EEPROM read has to
				// return the same MAC. Words 0/1/2 = MAC[0..5].
				if (index == 0x21 && length >= 2 && s->eeprom_word < 3)
				{
					data[0] = s->mac[s->eeprom_word * 2];
					data[1] = s->mac[s->eeprom_word * 2 + 1];
				}
				p->actual_length = length;
			}
			else // 0xF1 OUT: track chip state, stash write into the shadow
			{
				if (index == 0x25 && length >= 4)
				{
					// MII access setup: data[0] = PHY address, data[3] low
					// bits = MII register to read next.
					s->mii_phyaddr = data[0];
					s->mii_reg = data[3] & 0x1F;
				}
				else if (index == 0x7C)
					s->init_done = true; // NIC setup complete -> next reg 0x2B read delivers link-up

				for (int i = 0; i < length && (index + i) < (int)sizeof(s->an986_regs); i++)
					s->an986_regs[index + i] = data[i];
				// Reg 0x20 write selects the EEPROM word to read next.
				if (index == 0x20 && length >= 1)
					s->eeprom_word = data[0];
			}
			return;
		}
	}

	static void uepcb_handle_data(USBDevice* dev, USBPacket* p)
	{
		UePcbState* s = USB_CONTAINER_OF(dev, UePcbState, dev);
		const u8 ep = p->ep->nr;

		// ---- Runtime IOP-side fixes for the game's network stack ----
		// The stock AVE-TCP/AN986 stack was written for real hardware and has
		// two behaviors that break under emulation (verified against both a
		// real cabinet and emu-vs-emu sessions):
		//
		// 1. NETIF_FLAG_LINK_UP (0x04) never gets set on avetcp's netif
		//    because the link-up event path relies on real PHY interrupts.
		//    AVE-TCP checks this flag per received frame and silently drops
		//    everything (including the peer's TCP SYN) while it's clear.
		// 2. avetcp leaves the netif broadcast address as 255.255.255.255.
		//    Its destination-IP classifier tests (first_octet & 1), which
		//    misclassifies every unicast destination as broadcast and drops
		//    inbound SYNs before the TCP state machine. Rewriting it to the
		//    subnet broadcast (net | ~mask) fixes classification. (Play!'s
		//    Iop_NetDev applied the same fix for the same IRX.)
		//
		// The netif structure is located by pattern (MTU 1500 bytes followed
		// by the game's static IP 192.168.0.1); both fixes are idempotent.
		{
			static int s_nf = 0;
			static u8 s_lastflags = 0xfe;
			if ((s_nf++ % 64) == 0)
			{
				u8* m = iopMem->Main;
				for (u32 k = 2; k + 6 < 0x800000; k++)
				{
					if (m[k] == 0xdc && m[k + 1] == 0x05
						&& m[k + 2] == 0xc0 && m[k + 3] == 0xa8 && m[k + 4] == 0x00 && m[k + 5] == 0x01)
					{
						const u8 fl = m[k - 2];
						if (fl != s_lastflags)
						{
							s_lastflags = fl;
							Console.WriteLn("UePCB: netif flags=0x%02x %s @ IOP 0x%x",
								fl, (fl & 0x04) ? "LINK_UP" : "(no LINK_UP)", k - 2);
						}
						if (!(m[k - 2] & 0x04))
						{
							m[k - 2] |= 0x04;
							Console.WriteLn("UePCB: netif flags forced LINK_UP (0x%02x->0x%02x)",
								fl, m[k - 2]);
						}
						if (m[k + 6] == 0xff && m[k + 7] == 0xff && m[k + 8] == 0xff && m[k + 9] == 0xff)
						{
							u8 bc[4];
							for (int i = 0; i < 4; i++)
								bc[i] = m[k + 10 + i] | (u8)~m[k + 14 + i]; // net | ~mask
							if (!(bc[0] == 0xff && bc[1] == 0xff && bc[2] == 0xff && bc[3] == 0xff))
							{
								for (int i = 0; i < 4; i++)
									m[k + 6 + i] = bc[i];
								Console.WriteLn("UePCB: netif bcast fix 255.255.255.255 -> %u.%u.%u.%u",
									bc[0], bc[1], bc[2], bc[3]);
							}
						}
						break;
					}
				}
			}
		}

		// AN986.IRX link-check patch: the driver's link establishment routine
		// SKIPS the "wait for link and mark device up" path when the BMSR
		// link bit is already set at first read (it expects to observe a
		// down->up transition on real hardware). Since our PHY reports
		// link-up from the start, nop the branch so the driver still runs
		// the establishment path and marks the device link-up. The patch
		// site is identified by its unique instruction sequence:
		//   andi v0,v0,4 / bnez v0,+skip / addiu v0,zero,0xa
		{
			static bool s_patched = false;
			static int s_pc = 0;
			if (!s_patched && (s_pc++ % 64) == 0)
			{
				u8* m = iopMem->Main;
				static const u8 pat[12] = {0x04, 0x00, 0x42, 0x30, 0x15, 0x00, 0x40, 0x14, 0x0a, 0x00, 0x02, 0x24};
				for (u32 k = 0; k + 12 < 0x800000; k++)
				{
					if (std::memcmp(m + k, pat, 12) == 0)
					{
						m[k + 4] = 0; m[k + 5] = 0; m[k + 6] = 0; m[k + 7] = 0; // bnez -> nop
						s_patched = true;
						Console.WriteLn("UePCB: AN986 link-check patched @ IOP 0x%x", k);
						break;
					}
				}
			}
		}

		// AVE-TCP RX dispatch gate patch: the RX dispatcher drops every frame
		// unless the netif's state field equals 1, and the AN986 glue never
		// reliably sets it under emulation. Nop the branch so frames always
		// proceed to the ethertype dispatch (IP -> ip_input -> tcp_input).
		// Identified by instruction sequence; the branch offset is relative,
		// so the match is load-address independent.
		{
			static bool s_dpatched = false;
			static int s_dpc = 0;
			if (!s_dpatched && (s_dpc++ % 64) == 0)
			{
				u8* m = iopMem->Main;
				static const u8 dpat[16] = {0x04, 0x1a, 0x63, 0x94, 0x01, 0x00, 0x02, 0x24,
											0x30, 0x00, 0xbf, 0xaf, 0x32, 0x00, 0x62, 0x14};
				for (u32 k = 0; k + 16 < 0x800000; k++)
				{
					if (std::memcmp(m + k, dpat, 16) == 0)
					{
						m[k + 12] = 0; m[k + 13] = 0; m[k + 14] = 0; m[k + 15] = 0; // bne -> nop
						s_dpatched = true;
						Console.WriteLn("UePCB: RX dispatch state-gate patched @ IOP 0x%x", k + 12);
						break;
					}
				}
			}
		}

		switch (p->pid)
		{
			case USB_TOKEN_OUT:
			{
				// BULK OUT (EP2): a frame the game is transmitting. The AN986
				// driver splits frames larger than 64 bytes across multiple
				// USB packets; the 2-byte LE length prefix tells us the full
				// frame size, so accumulate until complete, then forward.
				u8 pkt[2048];
				int np = std::min<int>(p->buffer_size, (int)sizeof(pkt));
				usb_packet_copy(p, pkt, np);
				s->tx_accum.insert(s->tx_accum.end(), pkt, pkt + np);
				if (s->tx_accum.size() > 8192)
					s->tx_accum.clear(); // runaway safety
				while (s->tx_accum.size() >= 2)
				{
					int ethlen = s->tx_accum[0] | (s->tx_accum[1] << 8);
					int total = 2 + ethlen;
					if (ethlen < 14 || ethlen > 1600)
					{
						s->tx_accum.clear(); // bad prefix -> reset
						break;
					}
					if ((int)s->tx_accum.size() < total)
						break; // wait for the rest of this frame
					u8 buf[2048];
					int n = std::min<int>(total, (int)sizeof(buf));
					std::memcpy(buf, s->tx_accum.data(), n);
					s->tx_accum.erase(s->tx_accum.begin(), s->tx_accum.begin() + total);
					peer_send_all(s, buf + 2, ethlen);
					static int g_oe = 0;
					if (g_oe++ < 40 || (g_oe % 200) == 0)
						Console.WriteLn("UePCB: OUT->peer #%d %dB ethertype=%02x%02x",
							g_oe, ethlen, (n > 13 ? buf[12] : 0), (n > 13 ? buf[13] : 0));
				}
				break;
			}
			case USB_TOKEN_IN:
			{
				// BULK IN (EP1) / INT IN (EP3): deliver one queued inbound
				// frame if available.
				std::vector<u8> frame;
				if (pop_in_q(s, frame) && !frame.empty())
				{
					const int copyLen = std::min<int>(p->buffer_size, (int)frame.size());
					usb_packet_copy(p, frame.data(), copyLen);
					static int g_ie = 0;
					if (g_ie++ < 40 || (g_ie % 200) == 0)
						Console.WriteLn("UePCB: IN<-peer #%d %dB", g_ie, copyLen);
				}
				else if (ep == 3)
				{
					u8 ready = 0x40; // INT IN status: "ready"
					usb_packet_copy(p, &ready, 1);
				}
				else
					p->status = USB_RET_NAK; // no RX frame yet -> retry
				break;
			}
			default:
				p->status = USB_RET_STALL;
				break;
		}
	}

	static void uepcb_handle_destroy(USBDevice* dev)
	{
		UePcbState* s = USB_CONTAINER_OF(dev, UePcbState, dev);
		s->peer_stop = true;
		{
			std::lock_guard<std::mutex> lk(s->sock_lock);
			if (s->listen_sock != UEPCB_INVALID_SOCKET)
			{
				sock_close(s->listen_sock);
				s->listen_sock = UEPCB_INVALID_SOCKET;
			}
		}
		{
			// Close active peer connections to unblock recv loops.
			std::lock_guard<std::mutex> lk(s->peers_lock);
			for (socket_t p : s->peers)
				if (p != UEPCB_INVALID_SOCKET)
					sock_close(p);
			s->peers.clear();
		}
		if (s->peer_thread.joinable())
			s->peer_thread.join();
		delete s;
	}

	// ---------------- DeviceProxy ----------------
	USBDevice* UePcbDevice::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		UePcbState* s = new UePcbState();
		{
			// Role: blank PeerIP (or our own IP, see ip_is_own) = 1P/host;
			// anything else = 2P-4P/join. Hub topology covers both 2-player
			// and 4-player games.
			const std::string peer = USB::GetConfigString(si, port, TypeName(), "PeerIP", "");
			const bool has_peer = !peer.empty() && peer != "0.0.0.0";
			s->is_host = !has_peer || ip_is_own(peer);
			s->peer_port = USB::GetConfigInt(si, port, TypeName(), "Port", 7500);
			if (s->is_host)
			{
				s->bind_ip = USB::GetConfigString(si, port, TypeName(), "BindIP", "0.0.0.0");
#ifdef _WIN32
				open_firewall_port(s->peer_port);
#endif
			}
			else
				s->peer_ip = peer;

			// MAC: optional fixed override via MacHex, otherwise auto-generate
			// a unique one (Namco OUI 00:90:2e + 3 random bytes). Distinct
			// MACs per instance are required - the hub floods every frame to
			// everyone, and a duplicate MAC would alias two players.
			const std::string mh = USB::GetConfigString(si, port, TypeName(), "MacHex", "");
			if (mh.size() >= 12)
			{
				const auto hx = [](char c) -> int {
					if (c >= '0' && c <= '9') return c - '0';
					if (c >= 'a' && c <= 'f') return c - 'a' + 10;
					if (c >= 'A' && c <= 'F') return c - 'A' + 10;
					return 0;
				};
				for (int i = 0; i < 6; i++)
					s->mac[i] = static_cast<u8>((hx(mh[i * 2]) << 4) | hx(mh[i * 2 + 1]));
			}
			else
			{
				static std::mt19937 rng(std::random_device{}());
				const u32 r = rng();
				s->mac[0] = 0x00; s->mac[1] = 0x90; s->mac[2] = 0x2E;
				s->mac[3] = static_cast<u8>(r & 0xFF);
				s->mac[4] = static_cast<u8>((r >> 8) & 0xFF);
				s->mac[5] = static_cast<u8>((r >> 16) & 0xFF);
			}
		}
		s->dev.speed = USB_SPEED_FULL;
		s->desc.full = &s->desc_dev;
		s->desc.str = uepcb_strings;

		if (usb_desc_parse_dev(uepcb_dev_descriptor, sizeof(uepcb_dev_descriptor), s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(uepcb_config_descriptor, sizeof(uepcb_config_descriptor), s->desc_dev) < 0)
			goto fail;

		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = uepcb_handle_reset;
		s->dev.klass.handle_control = uepcb_handle_control;
		s->dev.klass.handle_data = uepcb_handle_data;
		s->dev.klass.unrealize = uepcb_handle_destroy;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = s->desc.str[2];

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);
		uepcb_handle_reset(&s->dev);
		s->peer_thread = std::thread(peer_thread_main, s);
		Console.WriteLn("UePCB: netplay %s port=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x",
			s->is_host ? "HOST (1P)" : "JOIN (2P-4P)", s->peer_port,
			s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
		return &s->dev;

	fail:
		uepcb_handle_destroy(&s->dev);
		return nullptr;
	}

	const char* UePcbDevice::Name() const { return "UE PCB (Namco arcade net)"; }
	const char* UePcbDevice::TypeName() const { return "UePcb"; }
	const char* UePcbDevice::IconName() const { return ""; }

	bool UePcbDevice::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		UePcbState* s = USB_CONTAINER_OF(dev, UePcbState, dev);
		if (!sw.DoMarker("UePcbDevice"))
			return false;
		sw.DoBytes(s->an986_regs, sizeof(s->an986_regs));
		return true;
	}

	std::span<const SettingInfo> UePcbDevice::Settings(u32 subtype) const
	{
		// Stored under [USB1] UePcb_*.
		static const SettingInfo settings[] = {
			{.type = SettingInfo::Type::String,
				.name = "PeerIP",
				.display_name = "1P's IP (same value on every PC)",
				.description = "Cross-PC: enter the 1P's LAN IP here on EVERY PC (same value). "
							   "The PC whose own IP matches auto-becomes 1P (listens); the rest (2P-4P) connect. "
							   "Same-PC: leave blank on 1P, 127.0.0.1 on the others.",
				.default_value = ""},
			{.type = SettingInfo::Type::String,
				.name = "BindIP",
				.display_name = "Bind IP (1P listen)",
				.description = "1P listen address. 0.0.0.0 = all adapters (LAN/VPN); "
							   "127.0.0.1 = same PC only. Ignored on 2P-4P.",
				.default_value = "0.0.0.0"},
			{.type = SettingInfo::Type::Integer,
				.name = "Port",
				.display_name = "TCP Port",
				.description = "TCP port for the peer link (1P opens it and adds a firewall rule automatically).",
				.default_value = "7500",
				.min_value = "1",
				.max_value = "65535",
				.step_value = "1"},
			{.type = SettingInfo::Type::String,
				.name = "MacHex",
				.display_name = "MAC 12-hex (optional override)",
				.description = "Leave BLANK - each emu auto-picks a unique MAC (00902e + random). "
							   "Only set this (12 hex) if you need a fixed MAC.",
				.default_value = ""}};
		return settings;
	}
} // namespace usb_uepcb
