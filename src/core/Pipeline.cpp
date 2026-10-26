#include "Pipeline.hpp"

#include "Export.hpp"
#include "Merge.hpp"

#include <filesystem>

namespace nm {

RunResult RunPipeline(const RunOptions& opt, const LogFn& log) {
	if (opt.fbxPath.empty()) throw Error("No output file selected");
	namespace fs = std::filesystem;
	fs::path out = fs::u8path(opt.fbxPath);
	if (out.has_parent_path() && !fs::exists(out.parent_path())) fs::create_directories(out.parent_path());

	MergedModel m = MergeModel(opt.request, log);
	WriteFbx(m, opt.fbxPath, log);
	RunResult r;
	r.fbxPath = opt.fbxPath;
	r.sidecarPath = SidecarPathFor(opt.fbxPath);
	WriteMaterialsJson(m, opt.fbxPath, r.sidecarPath);
	log(LogLevel::Info, "Wrote " + r.sidecarPath);
	if (!opt.objPath.empty()) {
		WriteObj(m, opt.objPath);
		log(LogLevel::Info, "Wrote " + opt.objPath);
	}
	r.materials = m.materials.size();
	r.vertices = m.positions.size();
	r.triangles = m.tris.size();
	r.bones = m.bones.size();
	return r;
}

} // namespace nm
