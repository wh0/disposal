#include <apt-pkg/algorithms.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/cacheset.h>
#include <apt-pkg/cmndline.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/init.h>
#include <apt-pkg/pkgcache.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/policy.h>
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

static bool in_base(pkgCacheFile &Cache, const pkgCache::PkgIterator pkg, const pkgCache::State::VerPriority reference_priority) {
	const pkgCache::VerIterator ver = Cache.GetPolicy()->GetCandidateVer(pkg);
	return ver.IsGood() && ver->Priority <= reference_priority;
}

// whatever per-package info we need
struct scan_info {
	pkgCache::VerIterator orig_cur;
	pkgCache::VerIterator orig_cand;
	bool in_no;
	bool in_yes;
};

template<typename callback_t>
static void fancy_reverse_deps(const pkgCache::PkgIterator pkg, const pkgCache::VerIterator ver, callback_t callback) {
	for (pkgCache::DepIterator dep = pkg.RevDependsList(); !dep.end(); ++dep) {
		if (dep.IsSatisfied(ver)) callback(dep);
	}
	for (pkgCache::PrvIterator prv = ver.ProvidesList(); !prv.end(); ++prv) {
		for (pkgCache::DepIterator dep = prv.ParentPkg().RevDependsList(); !dep.end(); ++dep) {
			if (dep.IsSatisfied(prv)) callback(dep);
		}
	}
}

bool notable_new_install(pkgCacheFile &Cache, const std::vector<scan_info> &info, const pkgCache::PkgIterator pkg) {
	// new install as requested
	pkgDepCache::StateCache &P = Cache[pkg];
	if (info[pkg->ID].in_yes) return true;
	bool notable = false;
	fancy_reverse_deps(pkg, P.InstVerIter(Cache), [&](const pkgCache::DepIterator dep) {
		if (dep.IsNegative()) return;
		if (!Cache->IsImportantDep(dep)) return;
		if (Cache[dep.ParentPkg()].NewInstall()) return;
		if (dep.ParentVer() != Cache[dep.ParentPkg()].InstallVer) return;
		// something that isn't a new install depends on it
		notable = true;
	});
	return notable;
}

bool notable_remove(pkgCacheFile &Cache, const std::vector<scan_info> &info, const pkgCache::PkgIterator pkg) {
	// removing as requested
	if (info[pkg->ID].in_no) return true;
	bool notable = true;
	fancy_reverse_deps(pkg, pkg.CurrentVer(), [&](const pkgCache::DepIterator dep) {
		if (dep.IsNegative()) return;
		if (!Cache->IsImportantDep(dep)) return;
		if (!Cache[dep.ParentPkg()].Delete()) return;
		if (dep.ParentVer() != dep.ParentPkg().CurrentVer()) return;
		// something else being removed depends on it
		notable = false;
	});
	return notable;
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
	pkgCache::State::VerPriority reference_priority = pkgCache::State::Required;
	{
		APT::CacheSetHelper helper;

		read_file(_config->FindFile("Disposal::State::No", "no.txt").c_str(), [&](const std::string &s) {
			helper.PackageFrom(APT::CacheSetHelper::STRING, &no, Cache, s);
		});

		read_file(_config->FindFile("Disposal::State::Yes", "yes.txt").c_str(), [&](const std::string &s) {
			// hhhh debian's "standard" task is done by priority
			if (s == "Priority: required") {
				reference_priority = pkgCache::State::Required;
				return;
			} else if (s == "Priority: important") {
				reference_priority = pkgCache::State::Important;
				return;
			} else if (s == "Priority: standard") {
				reference_priority = pkgCache::State::Standard;
				return;
			}
			APT::VersionContainerInterface::FromString(&yes, Cache, s, APT::CacheSetHelper::CANDIDATE, helper);
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
		APT::PackageSet autoInstall;

		// fill in the original candidate versions
		for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
			if (info[pkg->ID].orig_cand != NULL) {
				Cache->SetCandidateVersion(pkgCache::VerIterator(Cache, info[pkg->ID].orig_cand));
			}
		}

		// shallow-install base packages
		for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
			if (in_base(Cache, pkg, reference_priority)) {
				Fix.Protect(pkg);
				Cache->MarkInstall(pkg, false);
				autoInstall.insert(pkg);
			}
		}

		// prevent install of "no" packages
		for (const pkgCache::PkgIterator pkg : no) {
			info[pkg->ID].in_no = true;
			Fix.Protect(pkg);
			Fix.Remove(pkg);
			Cache->MarkProtected(pkg);
		}

		// shallow-install the "yes" packages
		for (const pkgCache::VerIterator ver : yes) {
			const pkgCache::PkgIterator pkg = ver.ParentPkg();
			info[pkg->ID].in_yes = true;
			Fix.Protect(pkg);
			Cache->SetCandidateVersion(ver);
			Cache->MarkInstall(pkg, false);
			autoInstall.insert(pkg);
		}

		// install everyone's dependencies
		for (const pkgCache::PkgIterator pkg : autoInstall) {
			if (Cache[pkg].InstBroken() || Cache[pkg].InstPolicyBroken()) Cache->MarkInstall(pkg);
		}

		// problems happen all the time
		Fix.Resolve();
	}

	if (Cache->BrokenCount() != 0) {
		std::cerr << Cache->BrokenCount() << " broken" << std::endl;
	}

	// the problem resolver might decide not to install a package we recursively marked for installation
	// it doesn't recursively unmark that package's dependencies
	{
		pkgDepCache::ActionGroup group(Cache);

		for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
			if (Cache[pkg].Garbage) {
				Cache->MarkDelete(pkg, false, 0, false);
			}
		}
	}

	// DoAutoRemove in private-install.cc does stuff when BrokenCount or PolicyBrokenCount are nonzero

	// restore current state
	for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
		pkg->CurrentVer = info[pkg->ID].orig_cur.MapPointer();
		pkgDepCache::StateCache &P = Cache[pkg];
		if (P.InstallVer == pkg.CurrentVer()) P.Mode = pkgDepCache::ModeKeep;
		else if (P.InstallVer == NULL && pkg.CurrentVer().IsGood()) P.Mode = pkgDepCache::ModeDelete;
		P.Update(pkg, Cache);
	}
	Cache->Update();

	// compare with simulation
	bool silence = true;
	for (pkgCache::PkgIterator pkg = Cache.GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
		pkgDepCache::StateCache &P = Cache[pkg];
		if (P.NewInstall()) {
			if (!notable_new_install(Cache, info, pkg)) std::cout << "  ";
			std::cout << pkg.Name() << '+' << std::endl;
			silence = false;
		} else if (P.Delete()) {
			if (!notable_remove(Cache, info, pkg)) std::cout << "  ";
			std::cout << pkg.Name() << '-' << std::endl;
			silence = false;
		}
	}
	if (silence) std::cerr << "no changes" << std::endl;

	return true;
}

CommandLine::Args Args[] = {
	{'m', "debug-marker", "Debug::pkgDepCache::Marker", CommandLine::Boolean},
	{'i', "debug-autoinstall", "Debug::pkgDepCache::AutoInstall", CommandLine::Boolean},
	{'p', "debug-problemresolver", "Debug::pkgProblemResolver", CommandLine::Boolean},
	{'r', "debug-autoremove", "Debug::pkgAutoRemove", CommandLine::Boolean},
	{'q', "quiet", "quiet", CommandLine::IntLevel},
	{'q', "silent", "quiet", CommandLine::IntLevel},
	{'c', "config-file", NULL, CommandLine::ConfigFile},
	{'\0', "install-recommends", "APT::Install-Recommends", CommandLine::Boolean},
	{'\0', "install-suggests", "APT::Install-Suggests", CommandLine::Boolean},
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
