#include "HkxAnimation.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace nm {

namespace {

// Member offsets of the classes read here, per pointer size. Each pointer member's offset is
// confirmed by the fixup tables of real files (HkxPackfile::pointer rejects a wrong one).
struct Layout {
	uint32_t bone, annTrack, annArray, annotation, annText;
	uint32_t skelName, skelParents, skelBones, skelPose;
	uint32_t animType, animMotion, animAnnotations;
	uint32_t splineHeader, blockOffsets, floatBlockOffsets, splineData;
	uint32_t interleavedTransforms;
	uint32_t bindSkeletonName, bindAnimation, bindTrackToBone, bindBlendHint;
	uint32_t refSamples;
};

const Layout kLayout32 = {8, 16, 4, 8, 4, 8, 12, 24, 36, 8, 24, 28, 40, 68, 80, 116, 40, 8, 12, 16, 40, 52};
const Layout kLayout64 = {16, 24, 8, 16, 8, 16, 24, 40, 56, 16, 32, 40, 56, 88, 104, 152, 56, 16, 24, 32, 64, 56};

const Layout& LayoutOf(const HkxPackfile& pf) { return pf.pointerSize() == 8 ? kLayout64 : kLayout32; }

// Encodings confirmed on real Skyrim clips (the samples and a scan of ~19,600 installed mod clips);
// anything else decodes but is reported as unverified.
const std::set<std::string> kVerified = {"class hkaSplineCompressedAnimation", "class hkaInterleavedUncompressedAnimation",
										 "position 16-bit", "scale 16-bit", "rotation THREECOMP40", "rotation THREECOMP48",
										 "explicit track-to-bone binding", "float tracks"};

std::string Lower(std::string s) {
	for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

HkTransform ReadQs(const HkxPackfile& pf, uint32_t off) {
	HkTransform x;
	float v[12];
	std::memcpy(v, pf.bytes(off, sizeof v), sizeof v);
	x.t = {v[0], v[1], v[2]};
	x.r = Quat(v[4], v[5], v[6], v[7]).normalized();
	x.s = {v[8], v[9], v[10]};
	return x;
}

// ---- Spline-compressed tracks ------------------------------------------------------------------

const char* kRotName[] = {"POLAR32", "THREECOMP40", "THREECOMP48", "THREECOMP24", "STRAIGHT16", "UNCOMPRESSED"};
const int kRotSize[] = {4, 5, 6, 3, 2, 16};
const int kRotAlign[] = {4, 1, 2, 1, 2, 4};
const double kSqrtHalf = 0.70710678118654752;
const double kPi = 3.14159265358979323846;

struct Cursor {
	const uint8_t* d;
	size_t size;
	size_t o;
	const HkxPackfile& pf;
	void align(size_t n) { o = (o + n - 1) / n * n; }
	void need(size_t n) const {
		if (o + n > size) throw Error(pf.path() + ": spline data ends inside a track");
	}
	template <class T>
	T take() {
		need(sizeof(T));
		T v;
		std::memcpy(&v, d + o, sizeof(T));
		o += sizeof(T);
		return v;
	}
};

// A track component: constant, or a B-spline over the block's frames.
struct Spline {
	int degree = 0;
	std::vector<uint8_t> knots;
	std::vector<std::array<double, 4>> ctrl;
};
using Channel = std::variant<std::array<double, 4>, Spline>;

// The three smallest components of a unit quaternion plus the index of the dropped (largest) one.
Quat FromSmallestThree(const double c[3], int missing, bool negative) {
	double sum = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
	double w = std::sqrt(std::fmax(0.0, 1.0 - sum));
	if (negative) w = -w;
	double q[4];
	for (int i = 0, k = 0; i < 4; i++) q[i] = i == missing ? w : c[k++];
	return {q[0], q[1], q[2], q[3]};
}

Quat DecodeQuat(int kind, const uint8_t* b) {
	switch (kind) {
	case 0: { // POLAR32: 10-bit radius, 18-bit (phi, theta) on a quarter sphere, 4 sign bits
		uint32_t v;
		std::memcpy(&v, b, 4);
		const double rMask = (1 << 10) - 1;
		double r = static_cast<double>((v >> 18) & 0x3FF) / rMask;
		r = 1.0 - r * r;
		double phiTheta = static_cast<double>(v & 0x3FFFF);
		double phi = std::floor(std::sqrt(phiTheta));
		double theta = 0;
		if (phi > 0) {
			theta = (kPi / 4) * (phiTheta - phi * phi) / phi;
			phi = (kPi / 2) / 511.0 * phi;
		}
		double mag = std::sqrt(std::fmax(0.0, 1.0 - r * r));
		Quat q(std::sin(phi) * std::cos(theta) * mag, std::sin(phi) * std::sin(theta) * mag, std::cos(phi) * mag, r);
		if (v & 0x10000000) q.x = -q.x;
		if (v & 0x20000000) q.y = -q.y;
		if (v & 0x40000000) q.z = -q.z;
		if (v & 0x80000000) q.w = -q.w;
		return q;
	}
	case 1: { // THREECOMP40: 3 x 12 bits, 2 bits missing index, 1 sign bit
		uint64_t v = 0;
		std::memcpy(&v, b, 5);
		double c[3];
		for (int i = 0; i < 3; i++) c[i] = (static_cast<double>((v >> (12 * i)) & 0xFFF) - 2047.0) * (kSqrtHalf / 2047.0);
		return FromSmallestThree(c, static_cast<int>((v >> 36) & 3), (v >> 38) & 1);
	}
	case 2: { // THREECOMP48: 3 x 15 bits; missing index in the top bits of y (high) and x (low), sign in z's
		uint16_t s[3];
		std::memcpy(s, b, 6);
		int missing = ((s[1] >> 14) & 2) | ((s[0] >> 15) & 1);
		double c[3];
		for (int i = 0; i < 3; i++) c[i] = (static_cast<double>(s[i] & 0x7FFF) - 0x3FFF) * (kSqrtHalf / 0x3FFF);
		return FromSmallestThree(c, missing, (s[2] >> 15) != 0);
	}
	case 5: {
		float f[4];
		std::memcpy(f, b, 16);
		return {f[0], f[1], f[2], f[3]};
	}
	default:
		// THREECOMP24 and STRAIGHT16 have no published bit layout and no Skyrim sample to derive one
		// from, so they are refused rather than guessed.
		throw Error(std::string("rotation encoding ") + kRotName[kind] + " is not supported");
	}
}

// Position or scale: per axis spline / static / default.
Channel ReadVector(Cursor& c, uint8_t types, int bits, double fallback) {
	if (types & 0xF0) {
		Spline s;
		uint16_t n = c.take<uint16_t>();
		s.degree = c.take<uint8_t>();
		c.need(size_t(n) + s.degree + 2);
		s.knots.assign(c.d + c.o, c.d + c.o + n + s.degree + 2);
		c.o += n + s.degree + 2;
		c.align(4);
		struct Axis {
			bool spline;
			double a, b;
		} axes[3];
		for (int i = 0; i < 3; i++) {
			if (types & (0x10 << i)) {
				float lo = c.take<float>(), hi = c.take<float>();
				axes[i] = {true, lo, hi};
			} else if (types & (1 << i)) {
				axes[i] = {false, c.take<float>(), 0};
			} else {
				axes[i] = {false, fallback, 0};
			}
		}
		if (bits == 1) c.align(2);
		for (int k = 0; k <= n; k++) {
			std::array<double, 4> p{0, 0, 0, 0};
			for (int i = 0; i < 3; i++) {
				if (!axes[i].spline) {
					p[i] = axes[i].a;
					continue;
				}
				double q = bits == 0 ? c.take<uint8_t>() / 255.0 : c.take<uint16_t>() / 65535.0;
				p[i] = axes[i].a + (axes[i].b - axes[i].a) * q;
			}
			s.ctrl.push_back(p);
		}
		c.align(4);
		return s;
	}
	std::array<double, 4> v{fallback, fallback, fallback, 0};
	for (int i = 0; i < 3; i++)
		if (types & (1 << i)) v[i] = c.take<float>();
	c.align(4);
	return v;
}

Channel ReadRotation(Cursor& c, uint8_t types, int kind) {
	auto quat = [&]() {
		c.need(kRotSize[kind]);
		Quat q = DecodeQuat(kind, c.d + c.o);
		c.o += kRotSize[kind];
		return std::array<double, 4>{q.x, q.y, q.z, q.w};
	};
	if (types & 0xF0) {
		Spline s;
		uint16_t n = c.take<uint16_t>();
		s.degree = c.take<uint8_t>();
		c.need(size_t(n) + s.degree + 2);
		s.knots.assign(c.d + c.o, c.d + c.o + n + s.degree + 2);
		c.o += n + s.degree + 2;
		c.align(kRotAlign[kind]);
		for (int k = 0; k <= n; k++) s.ctrl.push_back(quat());
		c.align(4);
		return s;
	}
	if (types & 0x0F) {
		c.align(kRotAlign[kind]);
		auto q = quat();
		c.align(4);
		return q;
	}
	return std::array<double, 4>{0, 0, 0, 1};
}

// De Boor evaluation at parameter u (the frame index within the block).
std::array<double, 4> Evaluate(const Channel& ch, double u) {
	if (auto* v = std::get_if<std::array<double, 4>>(&ch)) return *v;
	const Spline& s = std::get<Spline>(ch);
	const int n = static_cast<int>(s.ctrl.size()) - 1, p = s.degree;
	const auto& U = s.knots;
	int span;
	if (u >= U[n + 1]) {
		span = n;
	} else {
		int lo = p, hi = n + 1;
		span = (lo + hi) / 2;
		while (u < U[span] || u >= U[span + 1]) {
			if (u < U[span]) hi = span;
			else lo = span;
			span = (lo + hi) / 2;
		}
	}
	std::array<std::array<double, 4>, 8> d;
	if (p >= 8) throw Error("spline degree " + std::to_string(p) + " is out of range");
	for (int j = 0; j <= p; j++) d[j] = s.ctrl[span - p + j];
	for (int r = 1; r <= p; r++)
		for (int j = p; j >= r; j--) {
			int i = span - p + j;
			double den = U[i + p + 1 - r] - U[i];
			double a = den == 0 ? 0.0 : (u - U[i]) / den;
			for (int k = 0; k < 4; k++) d[j][k] = (1 - a) * d[j - 1][k] + a * d[j][k];
		}
	return d[p];
}

void DecodeSpline(const HkxPackfile& pf, uint32_t obj, HkxClip& clip) {
	const Layout& L = LayoutOf(pf);
	int32_t hdr[4];
	std::memcpy(hdr, pf.bytes(obj + L.splineHeader, 16), 16);
	const int numFrames = hdr[0], numBlocks = hdr[1], maxFramesPerBlock = hdr[2], maskSize = hdr[3];
	clip.frameDuration = pf.read<float>(obj + L.splineHeader + 24);
	clip.numFrames = numFrames;
	if (numFrames < 1 || numBlocks < 1 || maxFramesPerBlock < 2) throw Error(pf.path() + ": empty animation");

	auto u32s = [&](uint32_t member, const char* what) {
		auto a = pf.array(member, what);
		std::vector<uint32_t> v(a.size);
		if (a.size) std::memcpy(v.data(), pf.bytes(a.data, 4 * size_t(a.size)), 4 * size_t(a.size));
		return v;
	};
	auto blockOffsets = u32s(obj + L.blockOffsets, "blockOffsets");
	auto floatBlockOffsets = u32s(obj + L.floatBlockOffsets, "floatBlockOffsets");
	auto dataArr = pf.array(obj + L.splineData, "data");
	const uint8_t* data = pf.bytes(dataArr.data, dataArr.size);
	if (blockOffsets.size() != size_t(numBlocks) || floatBlockOffsets.size() != size_t(numBlocks))
		throw Error(pf.path() + ": block tables do not match the block count");

	const int T = clip.numTracks;
	const int framesPerBlock = maxFramesPerBlock - 1; // neighbouring blocks share one frame
	clip.frames.assign(numFrames, std::vector<HkTransform>(T));
	for (int b = 0; b < numBlocks; b++) {
		const size_t start = blockOffsets[b];
		if (start + maskSize > dataArr.size) throw Error(pf.path() + ": block " + std::to_string(b) + " is out of range");
		Cursor c{data, dataArr.size, start + size_t(maskSize), pf};
		c.align(4);
		std::vector<std::array<Channel, 3>> tracks(T);
		for (int t = 0; t < T; t++) {
			const uint8_t* m = data + start + 4 * size_t(t);
			const int posBits = m[0] & 3, rotKind = (m[0] >> 2) & 0xF, sclBits = (m[0] >> 6) & 3;
			if (posBits > 1 || sclBits > 1 || rotKind > 5)
				throw Error(pf.path() + ": track " + std::to_string(t) + " has unknown quantization " + std::to_string(m[0]));
			if (m[1] & 0xF0) clip.formats.insert(posBits ? "position 16-bit" : "position 8-bit");
			if (m[2] & 0xFF) clip.formats.insert(std::string("rotation ") + kRotName[rotKind]);
			if (m[3] & 0xF0) clip.formats.insert(sclBits ? "scale 16-bit" : "scale 8-bit");
			try {
				tracks[t][0] = ReadVector(c, m[1], posBits, 0.0);
				tracks[t][1] = ReadRotation(c, m[2], rotKind);
				tracks[t][2] = ReadVector(c, m[3], sclBits, 1.0);
			} catch (const Error& e) {
				throw Error(pf.path() + ": block " + std::to_string(b) + ", track " + std::to_string(t) + ": " + e.what());
			}
		}
		// The file records where each block's transform data ends; decoding must land exactly there.
		if (c.o - start != floatBlockOffsets[b])
			throw Error(pf.path() + ": block " + std::to_string(b) + " decodes to " + std::to_string(c.o - start) +
						" bytes of transform data, the file says " + std::to_string(floatBlockOffsets[b]));
		const int first = b * framesPerBlock;
		const int last = b + 1 < numBlocks ? std::min(numFrames - 1, first + framesPerBlock - 1) : numFrames - 1;
		for (int f = first; f <= last; f++) {
			const double u = f - first;
			for (int t = 0; t < T; t++) {
				auto p = Evaluate(tracks[t][0], u), q = Evaluate(tracks[t][1], u), s = Evaluate(tracks[t][2], u);
				HkTransform& x = clip.frames[f][t];
				x.t = {p[0], p[1], p[2]};
				x.r = Quat(q[0], q[1], q[2], q[3]).normalized();
				x.s = {s[0], s[1], s[2]};
			}
		}
	}
}

void DecodeInterleaved(const HkxPackfile& pf, uint32_t obj, HkxClip& clip) {
	const Layout& L = LayoutOf(pf);
	auto a = pf.array(obj + L.interleavedTransforms, "transforms");
	if (clip.numTracks == 0 || a.size % clip.numTracks) throw Error(pf.path() + ": transform count is not a whole number of frames");
	clip.numFrames = static_cast<int>(a.size / clip.numTracks);
	clip.frameDuration = clip.numFrames > 1 ? clip.duration / (clip.numFrames - 1) : clip.duration;
	clip.frames.assign(clip.numFrames, std::vector<HkTransform>(clip.numTracks));
	for (int f = 0; f < clip.numFrames; f++)
		for (int t = 0; t < clip.numTracks; t++) clip.frames[f][t] = ReadQs(pf, a.data + 48 * (uint32_t(f) * clip.numTracks + t));
}

} // namespace

int HkxSkeleton::find(const std::string& bone) const {
	const std::string key = Lower(bone);
	for (size_t i = 0; i < bones.size(); i++)
		if (Lower(bones[i]) == key) return static_cast<int>(i);
	return -1;
}

std::vector<HkxSkeleton> ReadSkeletons(const HkxPackfile& pf) {
	const Layout& L = LayoutOf(pf);
	std::vector<HkxSkeleton> out;
	for (uint32_t o : pf.find("hkaSkeleton")) {
		HkxSkeleton s;
		s.name = pf.string(o + L.skelName, "hkaSkeleton.name").value_or("");
		auto parents = pf.array(o + L.skelParents, "hkaSkeleton.parentIndices");
		auto bones = pf.array(o + L.skelBones, "hkaSkeleton.bones");
		auto pose = pf.array(o + L.skelPose, "hkaSkeleton.referencePose");
		if (parents.size != bones.size || pose.size != bones.size)
			throw Error(pf.path() + ": skeleton '" + s.name + "' has mismatched bone, parent and pose counts");
		for (uint32_t i = 0; i < bones.size; i++) {
			s.bones.push_back(pf.string(bones.data + L.bone * i, "hkaBone.name").value_or(""));
			int p = pf.read<int16_t>(parents.data + 2 * i);
			if (p >= static_cast<int>(i)) throw Error(pf.path() + ": bone " + s.bones.back() + " precedes its parent");
			s.parents.push_back(p);
			s.referencePose.push_back(ReadQs(pf, pose.data + 48 * i));
		}
		out.push_back(std::move(s));
	}
	return out;
}

HkxClip ReadClip(const HkxPackfile& pf) {
	const Layout& L = LayoutOf(pf);
	HkxClip clip;
	clip.path = pf.path();
	clip.pointerSize = pf.pointerSize();
	uint32_t obj = 0;
	for (const char* cls : {"hkaSplineCompressedAnimation", "hkaInterleavedUncompressedAnimation"})
		if (auto found = pf.find(cls); !found.empty()) {
			if (found.size() > 1) throw Error(pf.path() + ": holds " + std::to_string(found.size()) + " animations");
			obj = found[0];
			clip.animClass = cls;
			break;
		}
	if (clip.animClass.empty()) throw Error(pf.path() + ": holds no animation (root: " + pf.rootClass() + ")");
	clip.formats.insert("class " + clip.animClass);

	clip.duration = pf.read<float>(obj + L.animType + 4);
	clip.numTracks = pf.read<int32_t>(obj + L.animType + 8);
	clip.numFloatTracks = pf.read<int32_t>(obj + L.animType + 12);
	if (clip.numTracks <= 0) throw Error(pf.path() + ": animation has no transform tracks");
	if (clip.numFloatTracks > 0) clip.formats.insert("float tracks");

	// Annotation tracks: one per transform track, named after its bone (names are null in SE clips).
	auto ann = pf.array(obj + L.animAnnotations, "annotationTracks");
	for (uint32_t i = 0; i < ann.size; i++) {
		uint32_t tr = ann.data + L.annTrack * i;
		clip.trackNames.push_back(pf.string(tr, "annotationTrack.trackName"));
		auto notes = pf.array(tr + L.annArray, "annotationTrack.annotations");
		for (uint32_t k = 0; k < notes.size; k++) {
			uint32_t a = notes.data + L.annotation * k;
			clip.events.push_back({pf.read<float>(a), pf.string(a + L.annText, "annotation.text").value_or(""), static_cast<int>(i)});
		}
	}
	std::stable_sort(clip.events.begin(), clip.events.end(), [](auto& a, auto& b) { return a.time < b.time; });
	clip.trackNames.resize(clip.numTracks);

	// Extracted motion (root motion), when present.
	if (auto rf = pf.pointer(obj + L.animMotion, "extractedMotion")) {
		clip.hasReferenceFrame = true;
		if (pf.objects().count(*rf) && pf.objects().at(*rf) == "hkaDefaultAnimatedReferenceFrame") {
			auto smp = pf.array(*rf + L.refSamples, "referenceFrameSamples");
			for (uint32_t i = 0; i < smp.size * 4 && !clip.referenceFrameMoves; i++)
				clip.referenceFrameMoves = pf.read<float>(smp.data + 4 * i) != 0.0f;
		}
	}

	// The binding that refers to this animation.
	for (uint32_t b : pf.find("hkaAnimationBinding")) {
		if (pf.pointer(b + L.bindAnimation, "binding.animation") != std::optional<uint32_t>(obj)) continue;
		clip.skeletonName = pf.string(b + L.bindSkeletonName, "binding.originalSkeletonName").value_or("");
		auto map = pf.array(b + L.bindTrackToBone, "binding.transformTrackToBoneIndices");
		for (uint32_t i = 0; i < map.size; i++) clip.trackToBone.push_back(pf.read<int16_t>(map.data + 2 * i));
		clip.blendHint = pf.read<int8_t>(b + L.bindBlendHint);
	}
	if (!clip.trackToBone.empty()) clip.formats.insert("explicit track-to-bone binding");
	if (clip.blendHint != 0) clip.formats.insert("additive blending");

	if (clip.animClass == "hkaSplineCompressedAnimation") DecodeSpline(pf, obj, clip);
	else DecodeInterleaved(pf, obj, clip);

	for (auto& f : clip.formats)
		if (!kVerified.count(f)) clip.unverifiedFormats.insert(f);
	return clip;
}

HkxKind Classify(const HkxPackfile& pf) {
	if (!pf.find("hkaSplineCompressedAnimation").empty() || !pf.find("hkaInterleavedUncompressedAnimation").empty())
		return HkxKind::Animation;
	if (!pf.find("hkaSkeleton").empty()) return HkxKind::Skeleton;
	return HkxKind::Other;
}

} // namespace nm
