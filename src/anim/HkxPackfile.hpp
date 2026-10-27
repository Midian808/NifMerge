#pragma once
// Reader for Havok 2010.2 binary packfiles, the container of every Skyrim .hkx: LE files use 32-bit
// pointers, SE files 64-bit. Objects live in the __data__ section; pointers inside it are resolved
// through its local and global fixup tables, and its virtual fixups give each object's class name.
#include "core/Model.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nm {

class HkxPackfile {
public:
	static HkxPackfile Load(const std::string& path);

	const std::string& path() const { return path_; }
	int pointerSize() const { return pointerSize_; } // 4 (Skyrim LE) or 8 (Skyrim SE)
	const std::string& version() const { return version_; }
	const std::map<uint32_t, std::string>& objects() const { return objects_; } // data offset -> class

	// Objects of one class, in file order.
	std::vector<uint32_t> find(const std::string& cls) const;
	// The class every other object hangs off (normally hkRootLevelContainer's first variant's class),
	// for reporting what a file is.
	std::string rootClass() const;

	// Reads a plain value at a data-section offset (bounds-checked).
	template <class T>
	T read(uint32_t off) const {
		check(off, sizeof(T));
		T v;
		std::memcpy(&v, bytes_.data() + dataStart_ + off, sizeof(T));
		return v;
	}
	const uint8_t* bytes(uint32_t off, size_t n) const {
		check(off, n);
		return bytes_.data() + dataStart_ + off;
	}
	size_t dataSize() const { return dataSize_; }

	// A pointer member: its target, or nullopt when null. A non-zero pointer with no fixup means the
	// member offset is wrong for this layout, which is a hard error rather than a silent misread.
	std::optional<uint32_t> pointer(uint32_t off, const char* what) const;
	// A string pointer member (hkStringPtr / char*); nullopt when null.
	std::optional<std::string> string(uint32_t off, const char* what) const;
	struct Array {
		uint32_t data = 0;
		uint32_t size = 0;
	};
	// An hkArray member: element data offset and element count.
	Array array(uint32_t off, const char* what) const;
	// Size of an hkArray member, of a pointer member, and of hkReferencedObject in this layout.
	uint32_t arraySize() const { return pointerSize_ == 8 ? 16 : 12; }
	uint32_t ptrSize() const { return static_cast<uint32_t>(pointerSize_); }

private:
	void check(uint32_t off, size_t n) const;
	[[noreturn]] void fail(const std::string& msg) const;

	std::string path_;
	std::vector<uint8_t> bytes_;
	int pointerSize_ = 4;
	std::string version_;
	size_t dataStart_ = 0, dataSize_ = 0;
	std::unordered_map<uint32_t, uint32_t> pointers_;
	std::map<uint32_t, std::string> objects_;
};

} // namespace nm
