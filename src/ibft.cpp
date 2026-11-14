#include "ibft.h"
#include "error.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

using namespace nic_onboard;

namespace {
	// the iSCSI Boot Firmware Table: iPXE leaves it in memory for the OS, with the session it booted from.
	// iScsiPrt reads the same table to find its boot NIC, so its MAC is the adapter Windows will have to start with.
	// layout from the iBFT specification (and Linux' iscsi_ibft.h): an ACPI header, then blocks found through the control block
#pragma pack(push, 1)
	struct Block
	{
		std::uint8_t id;
		std::uint8_t version;
		std::uint16_t length;
		std::uint8_t index;
		std::uint8_t flags;
	};

	struct Nic
	{
		Block header;
		std::uint8_t ip[16];
		std::uint8_t prefix;
		std::uint8_t origin;
		std::uint8_t gateway[16];
		std::uint8_t dns[2][16];
		std::uint8_t dhcp[16];
		std::uint16_t vlan;
		std::uint8_t mac[6];
		std::uint16_t pci_bdf;
		std::uint16_t hostname_offset;
		std::uint16_t hostname_length;
	};
#pragma pack(pop)
	static_assert(sizeof(Block) == 6 && sizeof(Nic) == 102);

	// the control block follows the ACPI header (36 bytes) and 12 reserved bytes, and after its own header
	// and the extensions word it is a list of 16-bit offsets (from the table start) to the other blocks
	constexpr auto control_offset = std::size_t{ 48 };
	constexpr auto control_offsets_start = std::size_t{ sizeof(Block) + sizeof(std::uint16_t) };
	constexpr auto control_id = std::uint8_t{ 1 };
	constexpr auto nic_id = std::uint8_t{ 3 };
	constexpr auto block_valid = std::uint8_t{ 1 };
	constexpr auto boot_selected = std::uint8_t{ 2 };

	bool block_at(std::span<const std::byte> table, std::size_t offset, Block& block)
	{
		if (offset + sizeof(block) > table.size())
			return false;

		std::memcpy(&block, table.data() + offset, sizeof(block));
		return offset + block.length <= table.size();
	}
}

std::optional<std::wstring> nic_onboard::boot_mac_from_ibft(std::span<const std::byte> table)
{
	auto control = Block{};
	if (!::block_at(table, ::control_offset, control) || control.id != ::control_id || control.length < ::control_offsets_start)
		throw Error{ L"The iSCSI boot firmware table is damaged." };

	// one NIC block is marked as the one the firmware booted from. when no block says so, a lone valid one will do
	auto selected = std::optional<std::wstring>{};
	auto valid = std::vector<std::wstring>{};
	for (auto position = ::control_offset + ::control_offsets_start; position + sizeof(std::uint16_t) <= ::control_offset + control.length; position += sizeof(std::uint16_t)) {
		auto offset = std::uint16_t{ 0 };
		std::memcpy(&offset, table.data() + position, sizeof(offset));

		auto header = Block{};
		if (!offset || !::block_at(table, offset, header) || header.id != ::nic_id || header.length < sizeof(Nic) || !(header.flags & ::block_valid))
			continue;

		auto nic = Nic{};
		std::memcpy(&nic, table.data() + offset, sizeof(nic));
		if (std::ranges::all_of(nic.mac, [](auto byte) { return byte == 0; }))
			continue;

		auto mac = std::wstring{};
		for (auto byte : nic.mac)
			mac += std::format(L"{:02X}", byte);

		if (header.flags & ::boot_selected)
			selected = mac;

		valid.push_back(std::move(mac));
	}

	if (selected)
		return selected;

	if (valid.size() == 1)
		return valid.front();

	return std::nullopt;
}
