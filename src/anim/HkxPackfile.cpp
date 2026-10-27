#include "HkxPackfile.hpp"

#include <filesystem>
#include <fstream>

namespace nm {

namespace {

const uint8_t kMagic[8] = {0x57, 0xE0, 0xE0, 0x57, 0x10, 0xC0, 0xC0, 0x10};
const char* kVersion = "hk_2010.2.0-r1"; // Skyrim LE and SE

struct SectionHeader {
	std::string tag;
	uint32_t start = 0, local = 0, global = 0, virt = 0, exports = 0, imports = 0, end = 0;
};

} // namespace

HkxPackfile HkxPackfile::Load(const std::string& path) {
	HkxPackfile pf;
	pf.path_ = path;
	std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
	if (!in) throw Error("Cannot open " + path);
	pf.bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	const auto& f = pf.bytes_;
	auto u32 = [&](size_t off) {
		if (off + 4 > f.size()) pf.fail("file is truncated");
		uint32_t v;
		std::memcpy(&v, f.data() + off, 4);
		return v;
	};

	if (f.size() >= 5 && std::memcmp(f.data(), "<?xml", 5) == 0)
		pf.fail("is a Havok XML file; only binary packfiles (what Skyrim loads) are read");
	if (f.size() < 0x40 || std::memcmp(f.data(), kMagic, 8) != 0) pf.fail("not a Havok binary packfile");
	// Layout rules: pointer size, little-endian, reuse-padding, empty-base-class optimisation.
	pf.pointerSize_ = f[16];
	if ((pf.pointerSize_ != 4 && pf.pointerSize_ != 8) || f[17] != 1)
		pf.fail("unsupported layout (pointer size " + std::to_string(f[16]) + ", " +
				(f[17] ? "little" : "big") + "-endian)");
	int numSections = static_cast<int>(u32(20));
	pf.version_.assign(reinterpret_cast<const char*>(f.data() + 40), strnlen(reinterpret_cast<const char*>(f.data() + 40), 16));
	if (pf.version_ != kVersion)
		pf.fail("Havok version '" + pf.version_ + "' is not Skyrim's (" + kVersion + ")");
	if (numSections != 3) pf.fail("expected 3 sections, found " + std::to_string(numSections));

	std::vector<SectionHeader> sections(numSections);
	for (int i = 0; i < numSections; i++) {
		size_t h = 0x40 + 0x30 * static_cast<size_t>(i);
		if (h + 0x30 > f.size()) pf.fail("file is truncated");
		SectionHeader& s = sections[i];
		s.tag.assign(reinterpret_cast<const char*>(f.data() + h), strnlen(reinterpret_cast<const char*>(f.data() + h), 19));
		uint32_t* fields[] = {&s.start, &s.local, &s.global, &s.virt, &s.exports, &s.imports, &s.end};
		for (int k = 0; k < 7; k++) *fields[k] = u32(h + 20 + 4 * k);
	}
	const SectionHeader& names = sections[0];
	const SectionHeader& data = sections[2];
	if (names.tag != "__classnames__" || data.tag != "__data__") pf.fail("unexpected section layout");
	if (!(data.local <= data.global && data.global <= data.virt && data.virt <= data.exports) ||
		size_t(data.start) + data.exports > f.size())
		pf.fail("corrupt __data__ section header");
	pf.dataStart_ = data.start;
	pf.dataSize_ = data.local; // objects end where the fixup tables begin

	// Local fixups: (pointer offset, target offset) pairs within __data__; 0xFFFFFFFF pads the table.
	for (uint32_t o = data.local; o + 8 <= data.global; o += 8) {
		uint32_t src = u32(data.start + o), dst = u32(data.start + o + 4);
		if (src != 0xFFFFFFFF) pf.pointers_[src] = dst;
	}
	// Global fixups: (pointer offset, target section, target offset); Skyrim's all point into __data__.
	for (uint32_t o = data.global; o + 12 <= data.virt; o += 12) {
		uint32_t src = u32(data.start + o), sec = u32(data.start + o + 4), dst = u32(data.start + o + 8);
		if (src == 0xFFFFFFFF) continue;
		if (sec != 2) pf.fail("global fixup into section " + std::to_string(sec) + " is not supported");
		pf.pointers_[src] = dst;
	}
	// Virtual fixups: (object offset, class-name section, class-name offset).
	for (uint32_t o = data.virt; o + 12 <= data.exports; o += 12) {
		uint32_t obj = u32(data.start + o), sec = u32(data.start + o + 4), name = u32(data.start + o + 8);
		if (obj == 0xFFFFFFFF) continue;
		if (sec != 0 || size_t(names.start) + name >= f.size()) pf.fail("corrupt virtual fixup");
		const char* s = reinterpret_cast<const char*>(f.data() + names.start + name);
		pf.objects_[obj] = std::string(s, strnlen(s, f.size() - names.start - name));
	}
	if (pf.objects_.empty()) pf.fail("contains no objects");
	return pf;
}

std::vector<uint32_t> HkxPackfile::find(const std::string& cls) const {
	std::vector<uint32_t> out;
	for (auto& [off, c] : objects_)
		if (c == cls) out.push_back(off);
	return out;
}

std::string HkxPackfile::rootClass() const {
	// hkRootLevelContainer: hkArray<NamedVariant{name, className, variant}> at offset 0.
	auto roots = find("hkRootLevelContainer");
	if (roots.empty()) return objects_.begin()->second;
	Array named = array(roots[0], "hkRootLevelContainer.namedVariants");
	for (uint32_t i = 0; i < named.size; i++)
		if (auto cls = string(named.data + 3 * ptrSize() * i + ptrSize(), "NamedVariant.className")) return *cls;
	return "hkRootLevelContainer";
}

std::optional<uint32_t> HkxPackfile::pointer(uint32_t off, const char* what) const {
	auto it = pointers_.find(off);
	if (it != pointers_.end()) return it->second;
	const uint8_t* p = bytes(off, ptrSize());
	for (uint32_t i = 0; i < ptrSize(); i++)
		if (p[i]) fail(std::string(what) + " at data offset " + std::to_string(off) + " holds a pointer with no fixup");
	return std::nullopt;
}

std::optional<std::string> HkxPackfile::string(uint32_t off, const char* what) const {
	auto t = pointer(off, what);
	if (!t) return std::nullopt;
	if (*t >= dataSize_) fail(std::string(what) + " points outside the data section");
	const char* s = reinterpret_cast<const char*>(bytes_.data() + dataStart_ + *t);
	return std::string(s, strnlen(s, dataSize_ - *t));
}

HkxPackfile::Array HkxPackfile::array(uint32_t off, const char* what) const {
	Array a;
	a.size = read<uint32_t>(off + ptrSize());
	auto t = pointer(off, what);
	if (a.size && !t) fail(std::string(what) + " has " + std::to_string(a.size) + " elements but no data");
	a.data = t.value_or(0);
	return a;
}

void HkxPackfile::check(uint32_t off, size_t n) const {
	if (size_t(off) + n > dataSize_)
		fail("read of " + std::to_string(n) + " bytes at data offset " + std::to_string(off) + " is out of bounds");
}

void HkxPackfile::fail(const std::string& msg) const {
	throw Error(std::filesystem::u8path(path_).filename().u8string() + ": " + msg);
}

} // namespace nm
