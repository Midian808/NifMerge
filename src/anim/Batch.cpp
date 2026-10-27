#include "Batch.hpp"

#include "AnimExport.hpp"
#include "core/NifIO.hpp"
#include "core/TextUtil.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace nm {

namespace fs = std::filesystem;

namespace {

std::string LowerExt(const fs::path& p) {
	std::string e = p.extension().u8string();
	for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return e;
}

std::string Lower(std::string s) {
	for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string Name(const std::string& path) { return fs::u8path(path).filename().u8string(); }

void Sort(const fs::path& file, const std::string& relDir, BatchInputs& in, const LogFn& log) {
	const std::string ext = LowerExt(file), path = file.u8string();
	if (ext == ".nif") {
		in.nifs.push_back(path);
		return;
	}
	if (ext != ".hkx") return;
	try {
		HkxPackfile pf = HkxPackfile::Load(path);
		switch (Classify(pf)) {
			case HkxKind::Animation: in.clips.push_back({path, relDir}); break;
			case HkxKind::Skeleton: in.skeletons.push_back(path); break;
			default: in.skipped.push_back({path, pf.rootClass()}); log(LogLevel::Info, "Skipped " + Name(path) + " (" + pf.rootClass() + ")");
		}
	} catch (const Error& e) {
		// An unreadable .hkx is still a clip the user asked for; it fails in its own right.
		in.clips.push_back({path, relDir});
		(void)e;
	}
}

std::string Pick(const std::vector<std::string>& found, const std::string& what) {
	if (found.size() == 1) return found[0];
	std::string names;
	for (auto& f : found) names += "\n  " + f;
	throw Error("More than one " + what + " in the selection; choose one:" + names);
}

} // namespace

size_t BatchResult::exported() const {
	return std::count_if(items.begin(), items.end(), [](auto& i) { return i.error.empty(); });
}
size_t BatchResult::failed() const { return items.size() - exported(); }

BatchInputs ClassifyInputs(const std::vector<std::string>& inputs, const LogFn& log) {
	BatchInputs in;
	for (auto& input : inputs) {
		fs::path p = fs::u8path(input);
		std::error_code ec;
		if (fs::is_directory(p, ec)) {
			std::vector<fs::path> files;
			for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec))
				if (e.is_regular_file(ec)) files.push_back(e.path());
			std::sort(files.begin(), files.end());
			for (auto& f : files) {
				std::string rel = fs::relative(f.parent_path(), p, ec).generic_u8string();
				Sort(f, rel == "." ? "" : rel, in, log);
			}
		} else if (fs::is_regular_file(p, ec)) {
			Sort(p, "", in, log);
		} else {
			throw Error("Not found: " + input);
		}
	}
	return in;
}

BatchResult RunBatch(const BatchRequest& req, const LogFn& log) {
	if (req.outDir.empty()) throw Error("No output folder given");
	BatchInputs in = ClassifyInputs(req.inputs, log);
	BatchResult res;
	res.skipped = in.skipped;

	// Skeletons: overrides first, else exactly one of each among the inputs, else the .hkx beside the .nif.
	res.skeletonNif = !req.skeletonNif.empty() ? req.skeletonNif : (in.nifs.empty() ? "" : Pick(in.nifs, "skeleton .nif"));
	if (res.skeletonNif.empty()) throw Error("No skeleton .nif given");
	if (!req.skeletonHkx.empty()) {
		res.skeletonHkx = req.skeletonHkx;
	} else if (!in.skeletons.empty()) {
		res.skeletonHkx = Pick(in.skeletons, "skeleton .hkx");
	} else {
		fs::path nif = fs::u8path(res.skeletonNif);
		for (fs::path c : {nif.parent_path() / (nif.stem().u8string() + ".hkx"), nif.parent_path() / "skeleton.hkx"})
			if (fs::is_regular_file(c)) {
				res.skeletonHkx = c.u8string();
				break;
			}
		if (res.skeletonHkx.empty()) throw Error("No skeleton .hkx given, and none beside " + res.skeletonNif);
	}
	Skeleton nif = LoadSkeleton(res.skeletonNif);
	HkxPackfile hkxFile = HkxPackfile::Load(res.skeletonHkx);
	std::vector<HkxSkeleton> skeletons = ReadSkeletons(hkxFile);
	if (skeletons.empty()) throw Error(res.skeletonHkx + ": holds no skeleton");
	log(LogLevel::Info, "Skeleton: " + res.skeletonNif + " + " + res.skeletonHkx);
	if (in.clips.empty()) throw Error("No animation clips in the selection");

	// Output paths, mirroring the folder each clip was found under. Clips that would share an output
	// path both fail instead of overwriting each other.
	std::map<std::string, std::vector<size_t>> byOutput;
	std::vector<std::string> outPaths;
	for (size_t i = 0; i < in.clips.size(); i++) {
		fs::path out = fs::u8path(req.outDir);
		if (!in.clips[i].relDir.empty()) out /= fs::u8path(in.clips[i].relDir);
		out /= fs::u8path(in.clips[i].path).stem().u8string() + ".fbx";
		outPaths.push_back(out.u8string());
		byOutput[Lower(out.lexically_normal().u8string())].push_back(i);
	}

	for (size_t i = 0; i < in.clips.size(); i++) {
		BatchResult::Item item;
		item.clip = in.clips[i].path;
		const std::string clipName = fs::u8path(item.clip).stem().u8string();
		try {
			auto& same = byOutput[Lower(fs::u8path(outPaths[i]).lexically_normal().u8string())];
			if (same.size() > 1) {
				std::string others;
				for (size_t j : same)
					if (j != i) others += (others.empty() ? "" : ", ") + in.clips[j].path;
				throw Error("would be written to the same file as " + others);
			}
			HkxPackfile pf = HkxPackfile::Load(item.clip);
			HkxClip clip = ReadClip(pf);
			const HkxSkeleton& hkx = BodySkeleton(skeletons, clip.skeletonName, res.skeletonHkx);
			ClipBinding binding = BindClip(clip, hkx);
			PosedClip posed = PoseClip(clip, binding, hkx, nif);

			std::error_code ec;
			fs::create_directories(fs::u8path(outPaths[i]).parent_path(), ec);
			AnimExportInfo info{clipName, item.clip, res.skeletonNif, res.skeletonHkx, &clip, &binding};
			auto warn = [&](LogLevel lvl, const std::string& msg) {
				if (lvl != LogLevel::Info) item.warnings.push_back(msg);
				log(lvl, msg);
			};
			WriteAnimFbx(posed, info, outPaths[i]);
			WriteAnimJson(posed, info, outPaths[i], warn);
			item.fbx = outPaths[i];

			std::ostringstream m;
			m << clipName << ": " << clip.numFrames << " frames (" << JsonNum(clip.duration) << " s, " << (clip.pointerSize == 8 ? "SE" : "LE")
			  << "), binding " << binding.modeName() << ", " << clip.events.size() << " events" << (posed.loops ? ", loops" : "");
			log(LogLevel::Info, m.str());
			if (!binding.dropped.empty())
				warn(LogLevel::Warning, clipName + ": " + std::to_string(binding.dropped.size()) + " tracks have no bone in " +
											Name(res.skeletonHkx) + " and are left out");
			if (binding.conflicts)
				warn(LogLevel::Warning, clipName + ": " + std::to_string(binding.conflicts) +
											" tracks' binding indices point at differently named bones (made for another skeleton); bound by name");
			if (clip.numFloatTracks)
				warn(LogLevel::Warning, clipName + ": its " + std::to_string(clip.numFloatTracks) +
											" float tracks drive skeleton float slots, not bones, and are not exported");
			for (auto& f : clip.unverifiedFormats) warn(LogLevel::Warning, clipName + ": uses " + f + ", which no real sample has verified yet");
			if (clip.referenceFrameMoves) warn(LogLevel::Warning, clipName + ": has root motion, which is not exported (the clip plays in place)");
		} catch (const std::exception& e) {
			item.error = e.what();
			log(LogLevel::Error, clipName + ": " + item.error);
		}
		res.items.push_back(std::move(item));
	}

	// Report.
	std::error_code ec;
	fs::create_directories(fs::u8path(req.outDir), ec);
	res.reportPath = (fs::u8path(req.outDir) / "batch_report.json").u8string();
	std::ostringstream o;
	o << "{\n  \"generator\": \"SkyAnim\",\n  \"formatVersion\": 1,\n";
	o << "  \"skeletonNif\": " << JsonQuote(res.skeletonNif) << ",\n";
	o << "  \"skeletonHkx\": " << JsonQuote(res.skeletonHkx) << ",\n";
	o << "  \"exported\": " << res.exported() << ",\n  \"failed\": " << res.failed() << ",\n";
	o << "  \"clips\": [";
	for (size_t i = 0; i < res.items.size(); i++) {
		auto& it = res.items[i];
		o << (i ? ",\n" : "\n") << "    {\"clip\": " << JsonQuote(it.clip);
		if (it.error.empty()) o << ", \"fbx\": " << JsonQuote(it.fbx);
		else o << ", \"error\": " << JsonQuote(it.error);
		o << ", \"warnings\": [";
		for (size_t k = 0; k < it.warnings.size(); k++) o << (k ? ", " : "") << JsonQuote(it.warnings[k]);
		o << "]}";
	}
	o << (res.items.empty() ? "],\n" : "\n  ],\n");
	o << "  \"skipped\": [";
	for (size_t i = 0; i < res.skipped.size(); i++)
		o << (i ? ",\n" : "\n") << "    {\"file\": " << JsonQuote(res.skipped[i].first) << ", \"holds\": " << JsonQuote(res.skipped[i].second) << "}";
	o << (res.skipped.empty() ? "]\n" : "\n  ]\n") << "}\n";
	std::ofstream f(fs::u8path(res.reportPath), std::ios::binary);
	if (!f) throw Error("Cannot write " + res.reportPath);
	f << o.str();

	log(res.failed() ? LogLevel::Warning : LogLevel::Info,
		"Exported " + std::to_string(res.exported()) + " of " + std::to_string(res.items.size()) + " clips" +
			(res.failed() ? " (" + std::to_string(res.failed()) + " failed)" : "") + "; report: " + res.reportPath);
	return res;
}

} // namespace nm
