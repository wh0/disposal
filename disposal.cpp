#include <apt-pkg/algorithms.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/cacheset.h>
#include <apt-pkg/cmndline.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/init.h>
#include <apt-pkg/pkgcache.h>
#include <apt-pkg/progress.h>

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

template<typename callback_t>
static void read_file(const char * const filename, callback_t callback) {
	std::ifstream ifs(filename);
	std::string line;
	while (std::getline(ifs, line)) {
		if (line[0] == '#') continue;
		if (line.empty()) continue;
		callback(line);
	}
}

// whatever per-package info we need
struct scan_info {
	pkgCache::Version *orig_cur;
	pkgCache::Version *orig_cand;
	bool in_no;
	bool in_yes;
};

bool notable_new_install(pkgCacheFile &Cache, const std::vector<scan_info> &info, const pkgCache::PkgIterator pkg, pkgDepCache::StateCache &P) {
	// new install as requested
	if (info[pkg->ID].in_yes) return true;
	for (pkgCache::DepIterator dep = pkg.RevDependsList(); !dep.end(); ++dep) {
		if (dep.IsNegative()) continue;
		if (!Cache->IsImportantDep(dep)) continue;
		if (Cache[dep.ParentPkg()].NewInstall()) continue;
		if (dep.ParentVer() != Cache[dep.ParentPkg()].InstallVer) continue;
		if (!dep.IsSatisfied(P.InstVerIter(Cache))) continue;
		// something that isn't a new install depends on it
		return true;
	}
	return false;
}

bool notable_remove(pkgCacheFile &Cache, const std::vector<scan_info> &info, const pkgCache::PkgIterator pkg) {
	// removing as requested
	if (info[pkg->ID].in_no) return true;
	for (pkgCache::DepIterator dep = pkg.RevDependsList(); !dep.end(); ++dep) {
		if (dep.IsNegative()) continue;
		if (!Cache->IsImportantDep(dep)) continue;
		if (!Cache[dep.ParentPkg()].Delete()) continue;
		if (dep.ParentVer() != dep.ParentPkg().CurrentVer()) continue;
		if (!dep.IsSatisfied(pkg.CurrentVer())) continue;
		// something else being removed depends on it
		return false;
	}
	return true;
}

bool scan(CommandLine &CmdL) {
	pkgCacheFile Cache;

	OpTextProgress Prog(*_config);
	if (!Cache.BuildCaches(&Prog, false)) return false;
	if (!Cache.BuildPolicy(&Prog)) return false;

	// remember original current and candidate versions
	std::vector<scan_info> info(Cache.GetPkgCache()->Head().PackageCount);
	for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
		info[pkg->ID].orig_cur = pkg.CurrentVer();
		info[pkg->ID].orig_cand = Cache.GetPolicy()->GetCandidateVer(pkg);
	}

	// read in our state
	APT::PackageList no;
	APT::VersionList yes;
	{
		APT::CacheSetHelper helper;

		read_file(_config->FindFile("Disposal::State::No", "no.txt").c_str(), [&](const std::string s) {
			APT::PackageContainerInterface::FromString(&no, Cache, s, helper);
		});

		read_file(_config->FindFile("Disposal::State::Yes", "yes.txt").c_str(), [&](const std::string s) {
			APT::VersionContainerInterface::FromString(&yes, Cache, s, APT::VersionContainerInterface::CANDIDATE, helper);
		});

		// show messages from packages not found, but don't bail
		_error->DumpErrors();
	}

	// pretend nothing is installed
	for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
		pkg->CurrentVer = 0;
	}

	if (!Cache.BuildDepCache(&Prog)) return false;
	{
		pkgDepCache::ActionGroup group(Cache);
		pkgProblemResolver Fix(Cache);

		// fill in the original candidate versions
		for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
			if (info[pkg->ID].orig_cand != NULL) {
				Cache->SetCandidateVersion(pkgCache::VerIterator(Cache, info[pkg->ID].orig_cand));
			}
		}

		// prevent install of "no" packages
		for (pkgCache::PkgIterator pkg : no) {
			info[pkg->ID].in_no = true;
			Fix.Protect(pkg);
			Fix.Remove(pkg);
			Cache->MarkProtected(pkg);
		}

		// install required packages
		for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
			if (pkg->Flags & (pkgCache::Flag::Essential | pkgCache::Flag::Important)) {
				Fix.Protect(pkg);
				Cache->MarkInstall(pkg);
			}
		}

		// install the "yes" packages
		for (pkgCache::VerIterator ver : yes) {
			const pkgCache::PkgIterator pkg = ver.ParentPkg();
			info[pkg->ID].in_yes = true;
			Fix.Protect(pkg);
			Cache->SetCandidateVersion(ver);
			Cache->MarkInstall(pkg);
		}

		// *shrug* in case anything goes wrong, I guess
		Fix.Resolve();
	}

	if (Cache->BrokenCount() != 0) {
		std::cerr << Cache->BrokenCount() << " broken" << std::endl;
	}

	// restore current state
	for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
		pkg->CurrentVer = info[pkg->ID].orig_cur == NULL ? 0 : info[pkg->ID].orig_cur - Cache.GetPkgCache()->VerP;
		pkgDepCache::StateCache &P = Cache[pkg];
		if (P.InstallVer == pkg.CurrentVer()) P.Mode = pkgDepCache::ModeKeep;
		else if (P.InstallVer == NULL && pkg.CurrentVer().IsGood()) P.Mode = pkgDepCache::ModeDelete;
		P.Update(pkg, Cache);
	}
	Cache->Update();

	// compare with simulation
	for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
		pkgDepCache::StateCache &P = Cache[pkg];
		if (P.NewInstall()) {
			if (!notable_new_install(Cache, info, pkg, P)) std::cout << "  ";
			std::cout << pkg.Name() << '+' << std::endl;
		} else if (P.Delete()) {
			if (!notable_remove(Cache, info, pkg)) std::cout << "  ";
			std::cout << pkg.Name() << '-' << std::endl;
		}
	}

	return true;
}

CommandLine::Args Args[] = {
	{'m', "debug-marker", "Debug::pkgDepCache::Marker", CommandLine::Boolean},
	{'a', "debug-autoinstall", "Debug::pkgDepCache::AutoInstall", CommandLine::Boolean},
	{'p', "debug-problemresolver", "Debug::pkgProblemResolver", CommandLine::Boolean},
	{'q', "quiet", "quiet", CommandLine::IntLevel},
	{'q', "silent", "quiet", CommandLine::IntLevel},
	{'c', "config-file", NULL, CommandLine::ConfigFile},
	{'o', "option", NULL, CommandLine::ArbItem},
	{'\0', NULL, NULL, 0}
};

CommandLine::Dispatch Cmds[] = {
	{"scan", &scan},
	{NULL, NULL}
};

bool ensure_command(CommandLine &CmdL) {
	if (!CmdL.FileList[0]) return _error->Error("No operation specified");
	return true;
}

int main(const int argc, const char ** const argv) {
	CommandLine CmdL(Args, _config);
	if (!pkgInitConfig(*_config) ||
	    !CmdL.Parse(argc, argv) ||
	    !ensure_command(CmdL) ||
	    !pkgInitSystem(*_config, _system) ||
	    !CmdL.DispatchArg(Cmds)) {
		_error->DumpErrors();
		return 1;
	}
}
