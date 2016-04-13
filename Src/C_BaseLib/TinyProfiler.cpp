// We only support BL_PROFILE, BL_PROFILE_VAR, BL_PROFILE_VAR_STOP, BL_PROFILE_VAR_START,
// and BL_PROFILE_VAR_NS.

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>

#include <TinyProfiler.H>
#include <ParallelDescriptor.H>
#include <Utility.H>

#ifdef _OPENMP
#include <omp.h>
#endif

std::stack<std::pair<Real,Real> >          TinyProfiler::ttstack;
std::map<std::string, TinyProfiler::Stats> TinyProfiler::stats;
Real TinyProfiler::t_init = std::numeric_limits<Real>::max();

TinyProfiler::TinyProfiler (const std::string &funcname)
    : fname(funcname),
      running(false)
{
    start();
}

TinyProfiler::TinyProfiler (const std::string &funcname, bool start_)
    : fname(funcname),
      running(false)
{
    if (start_) start();
}

TinyProfiler::~TinyProfiler ()
{
    stop();
}

void
TinyProfiler::start ()
{
#ifdef _OPENMP
#pragma omp master
#endif
    if (!running) {
	running = true;
	Real t = ParallelDescriptor::second();
	ttstack.push(std::make_pair(t, 0.0));
    }
}

void
TinyProfiler::stop ()
{
#ifdef _OPENMP
#pragma omp master
#endif
    if (running) {
	running = false;
	Real t = ParallelDescriptor::second();

	std::pair<Real,Real> tt = ttstack.top();
	ttstack.pop();

	// first: wall time when the pair is pushed into the stack
	// second: accumulated dt of children

	Real dtin = t - tt.first; // elapsed time since start() is called.
	Real dtex = dtin - tt.second;

	Stats& st = stats[fname];
	++st.n;
	st.dtin += dtin;
	st.dtex += dtex;

	if (!ttstack.empty()) {
	    std::pair<Real,Real>& parent = ttstack.top();
	    parent.second += dtin;
	}
    }
}

void
TinyProfiler::Initialize ()
{
    t_init = ParallelDescriptor::second();
}

void
TinyProfiler::Finalize ()
{
    Real t_final = ParallelDescriptor::second();

    // make a local copy so that any functions call after this will be recorded in the local copy.
    std::map<std::string, Stats> lstats = stats;

    // make sure the set of profiled functions is the same on all processors
    Array<std::string> localStrings, syncedStrings;
    bool alreadySynced;

    for(std::map<std::string, Stats>::const_iterator it = lstats.begin();
	it != lstats.end(); ++it)
    {
	localStrings.push_back(it->first);
    }

    BoxLib::SyncStrings(localStrings, syncedStrings, alreadySynced);

    if( ! alreadySynced) {  // add the new name
	for(int i(0); i < syncedStrings.size(); ++i) {
	    std::map<std::string, Stats>::const_iterator it = lstats.find(syncedStrings[i]);
	    if(it == lstats.end()) {
		lstats.insert(std::make_pair(syncedStrings[i], Stats()));
	    }
	}
    }

    if (lstats.empty()) return;

    int nprocs = ParallelDescriptor::NProcs();
    int ioproc = ParallelDescriptor::IOProcessorNumber();

    std::vector<ProcStats> allprocstats;
    int maxfnamelen = 0;
    long maxncalls = 0;

    // now collect global data onto the ioproc
    for (std::map<std::string, Stats>::const_iterator it = lstats.begin();
	it != lstats.end(); ++it)
    {
	long n = it->second.n;
	Real dts[2] = {it->second.dtin, it->second.dtex};

	std::vector<long> ncalls(nprocs);
	std::vector<Real> dtdt(2*nprocs);

	if (ParallelDescriptor::NProcs() == 1) {
	    ncalls[0] = n;
	    dtdt[0] = dts[0];
	    dtdt[1] = dts[1];
	} else {
	    ParallelDescriptor::Gather(&n, 1, &ncalls[0], 1, ioproc);
	    ParallelDescriptor::Gather(dts, 2, &dtdt[0], 2, ioproc);
	}

	if (ParallelDescriptor::IOProcessor()) {
	    ProcStats pst;
	    for (int i = 0; i < nprocs; ++i) {
		pst.nmin  = std::min(pst.nmin, ncalls[i]);
		pst.navg +=                    ncalls[i];
		pst.nmax  = std::max(pst.nmax, ncalls[i]);
		pst.dtinmin  = std::min(pst.dtinmin, dtdt[2*i]);
		pst.dtinavg +=                       dtdt[2*i];
		pst.dtinmax  = std::max(pst.dtinmax, dtdt[2*i]);
		pst.dtexmin  = std::min(pst.dtexmin, dtdt[2*i+1]);
		pst.dtexavg +=                       dtdt[2*i+1];
		pst.dtexmax  = std::max(pst.dtexmax, dtdt[2*i+1]);
	    }
	    pst.navg /= nprocs;
	    pst.dtinavg /= nprocs;
	    pst.dtexavg /= nprocs;
	    pst.fname = it->first;
	    
	    allprocstats.push_back(pst);
	    maxfnamelen = std::max(maxfnamelen, int(pst.fname.size()));
	    maxncalls = std::max(maxncalls, pst.nmax);
	}
    }

    Real dt_max = t_final - t_init;
    ParallelDescriptor::ReduceRealMax(dt_max, ioproc);
    Real dt_min = t_final - t_init;
    ParallelDescriptor::ReduceRealMin(dt_min, ioproc);
    Real dt_avg = t_final - t_init;
    ParallelDescriptor::ReduceRealSum(dt_avg, ioproc);
    dt_avg /= Real(nprocs);

    if (ParallelDescriptor::IOProcessor()) {

	std::cout << std::setprecision(4);
	int wt = 9;

	std::cout << "\n\n";
	std::cout << "TinyProfiler total time across processes [min...avg...max]: " 
		  << dt_min << " ... " << dt_avg << " ... " << dt_max << "\n";

	int wnc = (int) std::log10 ((double) maxncalls) + 1;
	wnc = std::max(wnc, int(std::string("NCalls").size()));
	wt  = std::max(wt,  int(std::string("Excl. Min").size()));
	int wp = 6;
	wp  = std::max(wp,  int(std::string("Max %").size()));

	const std::string hline(maxfnamelen+wnc+2+(wt+2)*3+wp+2,'-');

	// Exclusive time
	std::sort(allprocstats.begin(), allprocstats.end(), ProcStats::compex);
	std::cout << "\n" << hline << "\n";
	std::cout << std::left
		  << std::setw(maxfnamelen) << "Name"
		  << std::right
		  << std::setw(wnc+2) << "NCalls"
		  << std::setw(wt+2) << "Excl. Min"
		  << std::setw(wt+2) << "Excl. Avg"
		  << std::setw(wt+2) << "Excl. Max"
		  << std::setw(wp+2)  << "Max %"
		  << "\n" << hline << "\n";
	for (std::vector<ProcStats>::const_iterator it = allprocstats.begin();
	     it != allprocstats.end(); ++it)
	{
	    std::cout << std::setprecision(4) << std::left
		      << std::setw(maxfnamelen) << it->fname
		      << std::right
		      << std::setw(wnc+2) << it->navg
		      << std::setw(wt+2) << it->dtexmin
		      << std::setw(wt+2) << it->dtexavg
		      << std::setw(wt+2) << it->dtexmax
		      << std::setprecision(2) << std::setw(wp+1) << std::fixed 
		      << it->dtexavg*(100.0/dt_max) << "%";
	    std::cout.unsetf(std::ios_base::fixed);
	    std::cout << "\n";
	}
	std::cout << hline << "\n";

	// Inclusive time
	std::sort(allprocstats.begin(), allprocstats.end(), ProcStats::compin);
	std::cout << "\n" << hline << "\n";
	std::cout << std::left
		  << std::setw(maxfnamelen) << "Name"
		  << std::right
		  << std::setw(wnc+2) << "NCalls"
		  << std::setw(wt+2) << "Incl. Min"
		  << std::setw(wt+2) << "Incl. Avg"
		  << std::setw(wt+2) << "Incl. Max"
		  << std::setw(wp+2)  << "Max %"
		  << "\n" << hline << "\n";
	for (std::vector<ProcStats>::const_iterator it = allprocstats.begin();
	     it != allprocstats.end(); ++it)
	{
	    std::cout << std::setprecision(4) << std::left
		      << std::setw(maxfnamelen) << it->fname
		      << std::right
		      << std::setw(wnc+2) << it->navg
		      << std::setw(wt+2) << it->dtinmin
		      << std::setw(wt+2) << it->dtinavg
		      << std::setw(wt+2) << it->dtinmax
		      << std::setprecision(2) << std::setw(wp+1) << std::fixed 
		      << it->dtinavg*(100.0/dt_max) << "%";
	    std::cout.unsetf(std::ios_base::fixed);
	    std::cout << "\n";
	}
	std::cout << hline << "\n";

	std::cout << std::endl;
    }
}